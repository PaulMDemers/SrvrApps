#include <srvros/desk_paths.h>
#include <srvros/desk_app.h>
#include <srvros/desk_runtime.h>
#include <srvros/desk_commands.h>
#include <srvros/desk_terminal_view.h>
#include <srvros/desk_ui.h>
#include <srvros/desk_dialog.h>
#include <srvros/desk_text.h>
#include <srvros/desk_clipboard.h>
#include <srvros/desk_log.h>
#include <srvros/sys.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#define INPUT_BYTES (DESK_CLIPBOARD_BYTES+32)
struct terminal_app {
    struct desk_app window;
    struct desk_connection connection;
    struct desk_runtime runtime;
    struct desk_terminal screen;
    struct desk_term_line lines[576],alternate[DESK_TERM_ROWS];
    struct desk_terminal_view view;
    struct desk_ui toolbar;
    struct desk_dialog dialog;
    struct desk_button buttons[4];
    struct desk_clipboard_client clipboard;
    char clipboard_store[DESK_CLIPBOARD_BYTES+1],copy[DESK_CLIPBOARD_BYTES+1];
    uint8_t input[INPUT_BYTES];unsigned input_head,input_size;
    uint64_t child,exit_status,blink;
    int master,started,exited,quit,modal,clip_paste,pty_eof;
    char status[160];
};
static struct terminal_app app;
enum { TERM_CLOSE=10,TERM_SELECT_ALL,TERM_PAGE_UP,TERM_PAGE_DOWN,TERM_FOLLOW };
static int network_trial;
static unsigned terminal_commands(struct desk_command commands[9]) {
    unsigned modal=app.modal?DESK_MENU_DISABLED:0;
    unsigned stopped=modal|(app.exited || !app.child?DESK_MENU_DISABLED:0);
    const struct desk_command values[]={
        {1,modal,0x11,SRV_KEY_CTRL|SRV_KEY_SHIFT,"New terminal","Ctrl-Shift-N"},
        {2,modal|(app.clipboard.mode || app.view.anchor==app.view.caret?DESK_MENU_DISABLED:0),6,SRV_KEY_CTRL|SRV_KEY_SHIFT,"Copy","Ctrl-Shift-C"},
        {3,stopped|(app.clipboard.mode?DESK_MENU_DISABLED:0),25,SRV_KEY_CTRL|SRV_KEY_SHIFT,"Paste","Ctrl-Shift-V"},
        {4,stopped,0,0,"Clear screen","Ctrl-L"},
        {TERM_CLOSE,modal,26,SRV_KEY_CTRL|SRV_KEY_SHIFT,"Close window","Ctrl-Shift-W"},
        {TERM_SELECT_ALL,modal,4,SRV_KEY_CTRL|SRV_KEY_SHIFT,"Select all","Ctrl-Shift-A"},
        {TERM_PAGE_UP,modal,SRV_KEY_PAGEUP,SRV_KEY_SHIFT,"Page up","Shift-PageUp"},
        {TERM_PAGE_DOWN,modal,SRV_KEY_PAGEDOWN,SRV_KEY_SHIFT,"Page down","Shift-PageDown"},
        {TERM_FOLLOW,modal,0,0,"Follow output",""}};
    memcpy(commands,values,sizeof(values));return 9;
}
/* Identify app bindings even when disabled/repeated, so they cannot fall through
 * into shell control bytes. Ctrl-C/D/L remain ordinary PTY input. Meta aliases
 * the app's Ctrl-Shift bindings; it never aliases raw shell Ctrl bindings. */
static unsigned terminal_shortcut(const struct srv_key_event *key) {
    struct srv_key_event copy=*key;struct desk_command commands[9];
    unsigned count=terminal_commands(commands);
    for(unsigned i=0;i<count;i++)commands[i].flags&=~DESK_MENU_DISABLED;
    if(copy.modifiers&SRV_KEY_META)copy.modifiers|=SRV_KEY_SHIFT;
    copy.flags&=~SRV_KEY_REPEAT;
    return desk_commands_key(commands,count,&copy);
}
static void run_command(unsigned id);
static void status(const char *message) { snprintf(app.status,sizeof(app.status),"%s",message);app.window.dirty=1; }
static int enqueue(const void *data,size_t size) {
    if(size>INPUT_BYTES-app.input_size || app.master<0 || app.exited) return -1;
    unsigned position=(app.input_head+app.input_size)%INPUT_BYTES;
    size_t first=INPUT_BYTES-position;if(first>size) first=size;
    memcpy(app.input+position,data,first);memcpy(app.input,(const uint8_t *)data+first,size-first);
    app.input_size+=size;return 0;
}
static void layout(void) {
    int w=app.window.width,h=app.window.height;
    int button_width=(w-48)/4;if(button_width>84) button_width=84;
    struct desk_command commands[9];unsigned count=terminal_commands(commands);
    for(int i=0;i<4;i++) desk_button_configure(&app.buttons[i],(struct desk_rect){12+i*(button_width+8),12,button_width,28},desk_commands_enabled(commands,count,i+1));
    desk_ui_layout_changed(&app.toolbar);desk_ui_focus(&app.toolbar,0);
    desk_terminal_view_configure(&app.view,(struct desk_rect){12,50,w-24,h-86},1);
    if(app.master>=0) {
        struct srv_winsize size={.row=app.view.rows,.col=app.view.columns,
            .xpixel=app.view.columns*app.view.cell_width,.ypixel=app.view.rows*app.view.cell_height};
        if(srv_ioctl(app.master,SRV_TIOCSWINSZ,&size)<0) status("The shell's window size could not be updated.");
    }
    desk_dialog_configure(&app.dialog,(struct desk_rect){0,0,w,h});
    desk_terminal_view_event(&app.view,(struct desk_widget_event){.type=app.window.focused && !app.modal?DESK_WIDGET_FOCUS_IN:DESK_WIDGET_FOCUS_OUT});
    app.window.dirty=1;
}
static void start(void) {
    app.started=1;int pair[2];
    if(srv_pty_create(pair)<0) { status("A terminal session could not be created.");return; }
    app.master=pair[0];
    if(srv_fcntl(app.master,SRV_F_SETFL,SRV_FD_NONBLOCK)<0) goto failed;
    layout();
    char *argv[]={"/fat/bin/sh",network_trial?"/fat/share/examples/network-trial.sh":NULL,NULL};
    network_trial=0; /* New sessions always open a normal shell. */
    /* Keep per-window command history in memory. Empty HISTFILE explicitly
     * disables disk history, avoiding cross-session overwrites. */
    char home[192],pwd[192];snprintf(home,sizeof(home),"HOME=%s",desk_home());
    snprintf(pwd,sizeof(pwd),"PWD=%s",desk_home());
    char *environment[]={"PATH=/fat/bin:/:/fat",home,pwd,"TERM=vt100","HISTFILE=","PS1=\\w $ ",NULL};
    struct srv_exec_request request={.path=argv[0],.argv=argv,.envp=environment,
        .flags=SRV_EXEC_BACKGROUND,.stdin_fd=pair[1],.stdout_fd=pair[1],.stderr_fd=pair[1],
        .process_group=UINT64_MAX,.foreground=1};
    char previous[160];
    if(!getcwd(previous,sizeof(previous)) || chdir(desk_home())<0) goto failed;
    long pid=srv_exec(&request);
    (void)chdir(previous);
    if(pid<=0) goto failed;
    app.child=pid;srv_close(pair[1]);status("Ctrl-Shift-C copies selection · Ctrl-Shift-V pastes · Ctrl-C interrupts");
    desk_log("deskterm: ready shell=%llu columns=%u rows=%u\n",(unsigned long long)app.child,app.view.columns,app.view.rows);
    return;
failed:
    srv_close(pair[1]);srv_close(app.master);app.master=-1;status("The shell could not be started.");
}
static void close_request(void) {
    if(!app.child || app.exited) { app.quit=1;return; }
    app.modal=1;desk_terminal_view_event(&app.view,(struct desk_widget_event){.type=DESK_WIDGET_FOCUS_OUT});
    desk_ui_window_focus(&app.toolbar,0);
    const struct desk_dialog_choice choices[]={ {1,"Close",1},{2,"Cancel",1} };
    desk_dialog_open(&app.dialog,"Close this terminal?","The shell and its commands will receive hangup. Cancel returns to the session.",choices,2,2,2);
    desk_dialog_configure(&app.dialog,(struct desk_rect){0,0,app.window.width,app.window.height});
    desk_dialog_focus(&app.dialog,app.window.focused);app.window.dirty=1;
}
static void dialog_action(struct desk_action a) {
    if(a.flags&DESK_WIDGET_REDRAW) app.window.dirty=1;
    if(!(a.flags&DESK_WIDGET_ACTIVATED)) return;
    if(a.id==1) app.quit=1;
    else if(a.id==2) {
        app.modal=0;desk_app_cancel_close(&app.window);desk_dialog_focus(&app.dialog,0);desk_ui_window_focus(&app.toolbar,1);
        desk_terminal_view_event(&app.view,(struct desk_widget_event){.type=DESK_WIDGET_FOCUS_IN});app.window.dirty=1;
    }
}
static void copy_selection(void) {
    if(app.clipboard.mode) { status("The clipboard is busy. Try again.");return; }
    long length=desk_terminal_view_copy(&app.view,app.copy,sizeof(app.copy));
    if(length<0) { status("The selection is larger than the 64 KiB clipboard.");return; }
    if(!length) { status("Drag across terminal text to select it first.");return; }
    app.clip_paste=0;
    if(desk_clipboard_copy(&app.clipboard,app.window.server,app.copy,length,srv_ticks())) status("Copy could not be started.");
}
static void paste(void) {
    if(app.exited || !app.child) { status("This session has ended. Open a new terminal to continue.");return; }
    if(app.clipboard.mode) { status("The clipboard is busy. Try again.");return; }
    app.clip_paste=1;
    if(desk_clipboard_paste(&app.clipboard,app.window.server,srv_ticks())) status("Paste could not be started.");
}
static void clipboard_result(int result) {
    if(result<0) { status("The clipboard transfer failed. Try again.");return; }
    if(app.clip_paste && app.modal) { status("Paste was canceled while the close dialog was open.");return; }
    if(!app.clip_paste) { status("Selection copied.");desk_log("deskterm: clipboard copied\n");return; }
    if(!desk_text_utf8_valid(app.clipboard_store,app.clipboard.length)) { status("The clipboard does not contain valid text.");return; }
    for(unsigned i=0;i<app.clipboard.length;i++) {
        unsigned c=(unsigned char)app.clipboard_store[i];
        if((c<32 && c!='\n' && c!='\r' && c!='\t') || c==127) { status("Paste was not sent: the text contains control characters.");return; }
    }
    unsigned multiline=0;
    for(unsigned i=0;i<app.clipboard.length;i++)
        if(app.clipboard_store[i]=='\r' || app.clipboard_store[i]=='\n') multiline=1;
    unsigned bracket=app.screen.bracketed_paste;
    unsigned size=desk_terminal_paste_normalize(&app.screen,app.clipboard_store,app.clipboard.length);
    if(size+12*bracket>INPUT_BYTES-app.input_size || app.exited || app.master<0) {
        status("The input queue is full or the session has ended. Paste was not sent.");return;
    }
    if(bracket) enqueue("\033[200~",6);
    enqueue(app.clipboard_store,size);
    if(bracket) enqueue("\033[201~",6);
    desk_terminal_view_follow(&app.view);status(multiline && !bracket ? "Pasted as one line. Press Enter to submit." : "Clipboard text pasted.");desk_log("deskterm: clipboard pasted bytes=%u single_line=%u\n",size,multiline && !bracket);
}
static void command(void *context,unsigned id) {
    (void)context;
    if(id==1) {if(desk_app_launch(&app.window,DESK_APP_TERMINAL,""))status("A new terminal could not be opened.");}
    else if(id==2)copy_selection();else if(id==3)paste();
    else if(id==4) {if(enqueue("\014",1))status("This session cannot accept input.");}
    else if(id==TERM_CLOSE)close_request();
    else if(id==TERM_SELECT_ALL) {desk_terminal_view_select_all(&app.view);app.window.dirty=1;}
    else if(id==TERM_PAGE_UP || id==TERM_PAGE_DOWN) {
        desk_terminal_view_scroll(&app.view,(id==TERM_PAGE_UP?-1:1)*(int)app.view.rows*app.view.cell_height);app.window.dirty=1;
    } else if(id==TERM_FOLLOW) {desk_terminal_view_follow(&app.view);app.window.dirty=1;}
    desk_ui_focus(&app.toolbar,0);
    desk_terminal_view_event(&app.view,(struct desk_widget_event){.type=app.modal?DESK_WIDGET_FOCUS_OUT:DESK_WIDGET_FOCUS_IN});
}
static void run_command(unsigned id) {
    struct desk_command commands[9];unsigned count=terminal_commands(commands);
    desk_commands_invoke(commands,count,id,command,NULL);
}
static void toolbar_action(struct desk_action action) {
    if(action.flags&DESK_WIDGET_REDRAW)app.window.dirty=1;
    if(action.flags&DESK_WIDGET_ACTIVATED)run_command(action.id);
}
static void keyboard(const struct srv_key_event *key) {
    if(app.modal) {dialog_action(desk_dialog_key(&app.dialog,key));return;}
    if(!(key->flags&SRV_KEY_DOWN_EVENT) || (key->flags&SRV_KEY_CANCEL))return;
    unsigned id=terminal_shortcut(key);
    if(id) {if(!(key->flags&SRV_KEY_REPEAT))run_command(id);return;}
    char bytes[32];size_t length=desk_terminal_key(&app.screen,key,bytes,sizeof(bytes));
    if(length) {
        if(enqueue(bytes,length))status("The input queue is full or this session has ended.");
        else {desk_terminal_view_follow(&app.view);app.window.dirty=1;}
    }
}
static void menus(void) {
    struct desk_command commands[9];unsigned count=terminal_commands(commands);
    for(unsigned i=0;i<4;i++) {
        int enabled=desk_commands_enabled(commands,count,i+1);
        if(app.buttons[i].enabled!=enabled) {desk_button_configure(&app.buttons[i],app.buttons[i].frame,enabled);app.window.dirty=1;}
    }
    struct desk_menu_item items[12]={{0,DESK_MENU_HEADING,"File",""}};unsigned used=1;
    const unsigned order[]={0,4,1,2,5,3,6,7,8};
    for(unsigned i=0;i<9;i++) {
        if(i==2)items[used++]=(struct desk_menu_item){0,DESK_MENU_HEADING,"Edit",""};
        if(i==5)items[used++]=(struct desk_menu_item){0,DESK_MENU_HEADING,"View",""};
        desk_command_menu(&commands[order[i]],&items[used++]);
    }
    desk_app_menus(&app.window,items,used);desk_app_menu_tick(&app.window);
}
static void menu_action(unsigned id) {run_command(id);}
static void pump(void) {
    if(app.master<0) return;
    for(unsigned batch=0;batch<4 && app.input_size;batch++) {
        unsigned size=INPUT_BYTES-app.input_head;if(size>app.input_size) size=app.input_size;if(size>4096) size=4096;
        long n=srv_write(app.master,app.input+app.input_head,size);
        if(n==SRV_ERR_AGAIN || n==SRV_ERR_INTR) break;
        if(n<=0) { app.input_size=0;break; }
        app.input_head=(app.input_head+n)%INPUT_BYTES;app.input_size-=n;
    }
    for(unsigned batch=0;batch<4 && !app.pty_eof;batch++) {
        char output[4096];long n=srv_read(app.master,output,sizeof(output));
        if(n==SRV_ERR_AGAIN || n==SRV_ERR_INTR) break;
        if(n<=0) { app.pty_eof=1;break; }
        desk_terminal_feed(&app.screen,output,n);
    }
    if(INPUT_BYTES-app.input_size>=DESK_TERM_REPLY && app.screen.reply_size) {
        char replies[DESK_TERM_REPLY];size_t size=desk_terminal_reply(&app.screen,replies,sizeof(replies));enqueue(replies,size);
    }
    if(desk_terminal_view_sync(&app.view)) app.window.dirty=1;
    if(app.child && !app.exited) {
        long result=srv_wait(app.child,&app.exit_status,SRV_WAIT_NOHANG);
        if(result==(long)app.child) {
            app.exited=1;app.input_size=0;char text[160];snprintf(text,sizeof(text),"Session ended · status %llu · New opens another terminal",(unsigned long long)app.exit_status);status(text);
            desk_app_title(&app.window,"Terminal — Session ended");desk_log("deskterm: shell exited status=%llu\n",(unsigned long long)app.exit_status);
        }
    }
}
static void draw(struct desk_canvas *c) {
    const struct desk_theme *theme=&desk_theme_quiet;
    desk_fill(c,(struct desk_rect){0,0,c->width,c->height},theme->surface);
    desk_ui_draw(c,&app.toolbar,theme);desk_terminal_view_draw(c,&app.view,theme,!app.exited && !(app.blink&1));
    desk_label_draw(c,(struct desk_rect){12,c->height-28,c->width-24,24},theme->body,app.status,theme->muted);
    if(app.modal) desk_dialog_draw(c,&app.dialog,theme);
}
static int terminal_event(void *context,struct desk_app *window,struct desk_packet p) {
    (void)context;(void)window;
    if(p.type==DESK_CONFIGURE) { layout();if(!app.started) start(); }
    else if(p.type==DESK_FOCUS) {
        desk_terminal_view_event(&app.view,(struct desk_widget_event){.type=p.value && !app.modal?DESK_WIDGET_FOCUS_IN:DESK_WIDGET_FOCUS_OUT});
        if(app.modal) desk_dialog_focus(&app.dialog,p.value!=0);else desk_ui_window_focus(&app.toolbar,p.value!=0);app.window.dirty=1;
    } else if(p.type==DESK_MENU_ACTION) menu_action(p.value);
    else if(p.type==DESK_KEY) { struct srv_key_event k;memcpy(&k,p.text,sizeof(k));keyboard(&k); }
    else if(p.type==DESK_POINTER) {
        struct desk_widget_event event={.type=p.value&DESK_POINTER_CANCEL?DESK_WIDGET_CANCEL:DESK_WIDGET_POINTER,
            .x=p.x,.y=p.y,.down=(p.value&DESK_POINTER_LEFT)!=0,.pressed=(p.value&DESK_POINTER_PRESS)!=0};
        if(app.modal) dialog_action(desk_dialog_pointer(&app.dialog,event,srv_monotonic_us()));
        else {
            toolbar_action(desk_ui_pointer(&app.toolbar,event,srv_monotonic_us()));
            if(desk_terminal_view_event(&app.view,event)) app.window.dirty=1;
        }
    } else if(p.type==DESK_SCROLL && !app.modal) {
        if(desk_terminal_view_scroll(&app.view,p.y)) app.window.dirty=1;
    } else if(p.type==DESK_CLIPBOARD_ACK || p.type==DESK_CLIPBOARD_DATA) {
        int done=desk_clipboard_receive(&app.clipboard,&p);if(done) clipboard_result(done);
    } else if(p.type==DESK_CLOSE_REQUEST) close_request();
    return app.quit?1:0;
}
int main(int argc,char **argv) {
    network_trial=argc==3 && !strcmp(argv[1],"terminal") && !strcmp(argv[2],"network-trial");
    app.master=-1;
    if(desk_terminal_init(&app.screen,app.lines,576,app.alternate,80,1)) return 1;
    desk_terminal_view_init(&app.view,&app.screen);desk_ui_init(&app.toolbar);desk_dialog_init(&app.dialog);
    static const char *labels[]={"New","Copy","Paste","Clear"};
    for(int i=0;i<4;i++) desk_ui_add(&app.toolbar,i+1,DESK_CONTROL_BUTTON,&app.buttons[i],labels[i]);
    desk_clipboard_init(&app.clipboard,app.clipboard_store);
    desk_connection_init(&app.connection);desk_runtime_init(&app.runtime,&app.connection);
    desk_log("deskterm: starting\n");if(desk_window_open(&app.connection,&app.window,"Terminal",820,500))return 1;
    while(!app.quit && !app.window.failed && !app.window.closed) {
        if(desk_clipboard_tick(&app.clipboard,srv_ticks())<0) clipboard_result(-1);
        if(desk_runtime_dispatch(&app.runtime,terminal_event,NULL)<0)app.window.failed=1;
        if(app.quit || app.window.closed || app.window.failed) break;
        pump();uint64_t ticks=(uint64_t)srv_ticks(),blink=ticks/50;
        if(blink!=app.blink) { app.blink=blink;if(app.view.focused && !app.modal) app.window.dirty=1; }
        menus();
        struct desk_canvas canvas;if(desk_app_canvas(&app.window,&canvas)==1) { draw(&canvas);desk_app_present(&app.window); }
        if(app.master>=0) {
            unsigned events=(app.pty_eof?0:SRV_POLLIN)|(app.input_size?SRV_POLLOUT:0);
            if(events)desk_runtime_watch(&app.runtime,app.master,events);
        }
        long now=srv_monotonic_us();
        if(now>=0 && app.view.focused && !app.modal)desk_runtime_deadline(&app.runtime,(uint64_t)now+(50-ticks%50)*10000);
        desk_runtime_idle(&app.runtime,app.clipboard.mode || app.screen.reply_size || !app.started);
    }
    desk_runtime_timers_clear(&app.runtime);
    if(app.master>=0) srv_close(app.master);
    if(app.child && !app.exited) for(unsigned i=0;i<100;i++) {
        long result=srv_wait(app.child,&app.exit_status,SRV_WAIT_NOHANG);if(result!=0) break;srv_sleep_ticks(1);
    }
    desk_app_close(&app.window);desk_log("deskterm: exited failure=%d\n",app.window.failed);return app.window.failed?1:0;
}
