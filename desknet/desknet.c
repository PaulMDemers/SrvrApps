#include <srvros/desk_app.h>
#include <srvros/desk_ui.h>
#include <srvros/desk_popup.h>
#include <srvros/desk_command_job.h>
#include <srvros/desk_clipboard.h>
#include <srvros/desk_preferences.h>
#include <srvros/desk_log.h>
#include <srvros/sys.h>
#include <stdio.h>
#include <string.h>
enum { NET_TABS=1,NET_TARGET,NET_RUN,NET_STOP,NET_COPY,NET_OUTPUT,NET_PING,NET_LOOKUP,NET_CLEAR,NET_CLOSE,NET_EDIT=100 };
struct network_app {
    struct desk_app window;
    struct desk_ui ui;
    struct desk_tabs tabs;
    struct desk_edit target;
    struct desk_text_buffer target_text;
    char target_storage[2][256];
    struct desk_button run,stop,copy;
    struct desk_document document;
    struct desk_popup context;
    struct desk_command_job job;
    char adapter[96];
    char output[4096],heading[448],run_target[256],result_status[128],status[160],report[8192];
    unsigned run_mode;
    size_t shown_length;
    uint64_t now,display_second;
    struct desk_clipboard_client clipboard;
    unsigned char clipboard_data[DESK_CLIPBOARD_BYTES+1];
    unsigned clip_action;
    uint64_t clip_revision;
    size_t clip_caret,clip_anchor;
    int body_size,quit;
};
static struct network_app app;
static void status(const char *text) { snprintf(app.status,sizeof(app.status),"%s",text);app.window.dirty=1; }
/* DNS/IPv4 targets only. No option prefixes, shell syntax, empty labels or
 * leading/trailing hyphens. A trailing DNS root dot is allowed. */
static int target_valid(const char *s) {
    size_t n=strlen(s);if(!n || n>253) return 0;size_t label=0;
    for(size_t i=0;i<n;i++) {
        unsigned ch=(unsigned char)s[i];
        if(ch=='.') { if(!label || s[i-1]=='-') return 0;label=0;continue; }
        if(!((ch>='a' && ch<='z') || (ch>='A' && ch<='Z') || (ch>='0' && ch<='9') || ch=='-')) return 0;
        if((!label && ch=='-') || ++label>63) return 0;
    }
    return s[n-1]!='-';
}
static unsigned page(void) { return desk_tabs_selected(&app.tabs); }
static const char *operation(unsigned mode) { return mode==NET_LOOKUP?"DNS lookup":"Ping"; }
static void report_view(void) {
    struct desk_document_block blocks[3];unsigned count=0;
    if(app.run_mode) {
        snprintf(app.heading,sizeof(app.heading),"%s · %s · %s",operation(app.run_mode),app.run_target,app.adapter);
        blocks[count++]=(struct desk_document_block){1,DESK_DOCUMENT_HEADING,app.heading};
        blocks[count++]=(struct desk_document_block){2,DESK_DOCUMENT_CODE,app.job.length?app.output:app.job.busy?"Waiting for results…":"No output."};
        blocks[count++]=(struct desk_document_block){3,DESK_DOCUMENT_NOTE,app.result_status};
    } else {
        blocks[count++]=(struct desk_document_block){1,DESK_DOCUMENT_HEADING,"Check a connection"};
        blocks[count++]=(struct desk_document_block){2,DESK_DOCUMENT_PARAGRAPH,"Ping sends four echo requests to an IPv4 address or host. Lookup asks the configured DNS server for an IPv4 address."};
        blocks[count++]=(struct desk_document_block){3,DESK_DOCUMENT_NOTE,"Diagnostics use the active adapter. System > Network lists all supported adapters. Copy report retains the adapter used; Stop cancels a diagnostic."};
    }
    if(desk_document_set(&app.document,blocks,count)) status("The diagnostic report could not be displayed.");
    app.shown_length=app.job.length;app.window.dirty=1;
}
static void layout(void) {
    int w=app.window.width,h=app.window.height,compact=w<400;
    desk_tabs_configure(&app.tabs,(struct desk_rect){12,12,w-24,30},!app.job.busy);
    desk_edit_configure(&app.target,(struct desk_rect){72,54,w-84,30},&desk_font_16,1);
    desk_button_configure(&app.run,(struct desk_rect){12,96,compact?72:96,28},!app.job.busy && !app.job.retiring_pid);
    desk_button_configure(&app.stop,(struct desk_rect){compact?92:116,96,compact?64:80,28},app.job.busy);
    desk_button_configure(&app.copy,(struct desk_rect){compact?164:204,96,compact?104:128,28},app.run_mode && !app.clipboard.mode);
    desk_document_configure(&app.document,(struct desk_rect){12,140,w-24,h-176},app.body_size==20?&desk_font_20:&desk_font_16,1);
    desk_popup_cancel(&app.context);desk_ui_layout_changed(&app.ui);desk_ui_window_focus(&app.ui,app.window.focused);app.window.dirty=1;
}
static void run(void) {
    if(app.job.busy || app.job.retiring_pid) return;
    if(!target_valid(app.target_text.data)) { status("Enter an IPv4 address or a host name, without spaces or options.");desk_ui_focus(&app.ui,NET_TARGET);return; }
    struct srv_net_status_info network={0};
    if(srv_net_status_info(&network) || !network.initialized || !network.worker_started) { status("No supported network connection is available. Open System for details.");desk_log("desknet: unavailable\n");return; }
    if((network.link_flags&SRV_NET_LINK_KNOWN) && !(network.link_flags&SRV_NET_LINK_UP)) { status("The network link is disconnected. Reconnect it and try again.");desk_log("desknet: disconnected\n");return; }
    if(!network.local_ip) { status("An IPv4 address has not been configured. Open System for details.");return; }
    const char *program=page()==NET_LOOKUP?"/fat/bin/host":"/fat/bin/ping";
    char *argv[]={(char *)program,app.target_text.data,NULL};
    if(desk_command_job_start(&app.job,program,argv,app.now,15000000)) { status("The diagnostic could not start. Try again.");return; }
    snprintf(app.adapter,sizeof(app.adapter),"net0");
    for(unsigned i=0;i<network.adapter_count && i<SRV_NET_ADAPTER_MAX;i++) {
        const struct srv_net_adapter_info *a=&network.adapters[i];
        if(a->flags&SRV_NET_ADAPTER_ACTIVE) {
            snprintf(app.adapter,sizeof(app.adapter),"Adapter %u · %.31s · net0",a->id,a->driver);break;
        }
    }
    app.run_mode=page();strcpy(app.run_target,app.target_text.data);strcpy(app.result_status,"Running…");
    status("Running… You can keep working in other windows.");report_view();layout();
    desk_log("desknet: started mode=%s target=%s\n",app.run_mode==NET_LOOKUP?"lookup":"ping",app.run_target);
}
static void stop(void) {
    if(!app.job.busy) return;
    desk_command_job_cancel(&app.job);strcpy(app.result_status,"Stopped. Partial output is retained.");status(app.result_status);report_view();layout();desk_log("desknet: canceled\n");
}
static void poll_job(void) {
    size_t old=app.job.length;uint64_t retiring=app.job.retiring_pid;
    int done=desk_command_job_poll(&app.job,app.now);
    for(size_t i=old;i<app.job.length;i++) { unsigned ch=(unsigned char)app.output[i];if((ch<32 && ch!='\n' && ch!='\t') || ch>=127) app.output[i]='?'; }
    if(done) {
        if(done<0) snprintf(app.result_status,sizeof(app.result_status),"%s",app.job.timed_out?"Diagnostic timed out after 15 seconds.":"The diagnostic output could not be collected.");
        else snprintf(app.result_status,sizeof(app.result_status),"%s%s",app.job.exit_status?"Diagnostic did not succeed.":"Diagnostic completed.",app.job.truncated?" Output was truncated.":"");
        status(app.result_status);report_view();layout();desk_log("desknet: finished result=%d exit=%llu bytes=%zu truncated=%d\n",done,(unsigned long long)app.job.exit_status,app.job.length,app.job.truncated);
    } else if(app.job.length!=app.shown_length) report_view();
    if(retiring && !app.job.retiring_pid) layout();
    if(app.job.busy && app.now/1000000!=app.display_second) {
        app.display_second=app.now/1000000;char message[128];snprintf(message,sizeof(message),"Running · %llu s · Stop cancels",(unsigned long long)((app.now-app.job.started_us)/1000000));status(message);
    }
}
static void copy_report(void) {
    if(!app.run_mode || app.clipboard.mode) return;
    int n=snprintf(app.report,sizeof(app.report),"%s\nTarget: %s\nAdapter: %s\nStatus: %s\n\n%s",operation(app.run_mode),app.run_target,app.adapter,app.result_status,app.output);
    if(n<0 || (size_t)n>=sizeof(app.report) || desk_clipboard_copy(&app.clipboard,app.window.server,app.report,n,srv_ticks())) { status("The report could not be copied. Try again.");return; }
    app.clip_action=NET_COPY;status("Copying report…");layout();desk_log("desknet: copy-report bytes=%d\n",n);
}
static void clipboard_action(unsigned flags) {
    unsigned action=flags&(DESK_EDIT_COPY|DESK_EDIT_CUT|DESK_EDIT_PASTE);if(!action || app.clipboard.mode) return;
    struct desk_text_buffer *t=&app.target_text;size_t lo=t->anchor<t->caret?t->anchor:t->caret,hi=t->anchor>t->caret?t->anchor:t->caret;
    if(action!=DESK_EDIT_PASTE && lo==hi) return;
    int failed=action==DESK_EDIT_PASTE?desk_clipboard_paste(&app.clipboard,app.window.server,srv_ticks()):desk_clipboard_copy(&app.clipboard,app.window.server,t->data+lo,hi-lo,srv_ticks());
    if(failed) { status("The clipboard is unavailable. Try again.");return; }
    app.clip_action=NET_EDIT+action;app.clip_revision=t->revision;app.clip_caret=t->caret;app.clip_anchor=t->anchor;
}
static void clipboard_finish(int result) {
    unsigned action=app.clip_action;app.clip_action=0;
    if(result<0) { status("Clipboard transfer did not finish. Try again.");layout();return; }
    if(action==NET_COPY) { status("Report copied.");layout();return; }
    action-=NET_EDIT;if(action==DESK_EDIT_COPY) return;
    struct desk_text_buffer *t=&app.target_text;
    if(!app.target.focused || t->revision!=app.clip_revision || t->caret!=app.clip_caret || t->anchor!=app.clip_anchor) { status("The target changed during transfer. Repeat Cut or Paste.");return; }
    if(action==DESK_EDIT_PASTE && (memchr(app.clipboard.buffer,'\n',app.clipboard.length) || memchr(app.clipboard.buffer,'\r',app.clipboard.length))) { status("The target accepts one line.");return; }
    if(desk_text_buffer_insert(t,action==DESK_EDIT_CUT?"":(char *)app.clipboard.buffer,action==DESK_EDIT_CUT?0:app.clipboard.length)<0) status("That text is too long or invalid. The target was kept.");
    else { desk_edit_reveal(&app.target);app.window.dirty=1; }
}
static void action(struct desk_action a) {
    if(a.flags&DESK_WIDGET_REDRAW) app.window.dirty=1;
    if(a.id==NET_TARGET) { clipboard_action(a.flags);if(a.flags&DESK_EDIT_SUBMIT) run(); }
    if(!(a.flags&DESK_WIDGET_ACTIVATED)) return;
    if(a.id>=NET_EDIT+DESK_EDIT_SELECT_ALL && a.id<=NET_EDIT+DESK_EDIT_REDO) { unsigned flags=desk_edit_command(&app.target,a.id-NET_EDIT);clipboard_action(flags);if(flags&DESK_WIDGET_REDRAW) app.window.dirty=1;return; }
    switch(a.id) {
    case NET_RUN:run();break;case NET_STOP:stop();break;case NET_COPY:copy_report();break;
    case NET_CLOSE:app.quit=1;break;
    case NET_CLEAR:if(!app.job.busy) { app.run_mode=0;app.output[0]=0;app.job.length=0;report_view();status("Results cleared.");layout(); }break;
    case NET_PING:case NET_LOOKUP:if(!app.job.busy) { desk_tabs_select(&app.tabs,a.id);layout();desk_ui_focus(&app.ui,NET_TARGET); }break;
    case NET_TABS:layout();break;
    }
}
static void context(int x,int y,int keyboard) {
    int field=keyboard?app.target.focused:desk_widget_contains(app.target.frame,x,y);
    struct desk_menu_item items[8];unsigned count=0;
    if(field) {
        desk_ui_focus(&app.ui,NET_TARGET);if(keyboard) { x=app.target.frame.x+12;y=app.target.frame.y+24; }
        unsigned selected=app.target_text.anchor!=app.target_text.caret?0:DESK_MENU_DISABLED;
        unsigned busy=app.clipboard.mode?DESK_MENU_DISABLED:0;
        items[count++]=(struct desk_menu_item){NET_EDIT+DESK_EDIT_UNDO,desk_text_buffer_can_undo(&app.target_text,0)?0:DESK_MENU_DISABLED,"Undo","Ctrl-Z"};
        items[count++]=(struct desk_menu_item){NET_EDIT+DESK_EDIT_COMMAND_CUT,selected|busy,"Cut","Ctrl-X"};
        items[count++]=(struct desk_menu_item){NET_EDIT+DESK_EDIT_COMMAND_COPY,selected|busy,"Copy","Ctrl-C"};
        items[count++]=(struct desk_menu_item){NET_EDIT+DESK_EDIT_COMMAND_PASTE,busy,"Paste","Ctrl-V"};
        items[count++]=(struct desk_menu_item){NET_EDIT+DESK_EDIT_SELECT_ALL,0,"Select all","Ctrl-A"};
    } else items[count++]=(struct desk_menu_item){NET_COPY,!app.run_mode || app.clipboard.mode?DESK_MENU_DISABLED:0,"Copy report","Ctrl-Shift-C"};
    desk_popup_open(&app.context,items,count,x,y,(struct desk_rect){0,0,app.window.width,app.window.height},&desk_font_16,keyboard);app.window.dirty=1;
}
static void popup_action(struct desk_menu_result result) { if(result.redraw) app.window.dirty=1;if(result.action) action((struct desk_action){result.action,DESK_WIDGET_ACTIVATED}); }
static void key(const struct srv_key_event *k) {
    if(desk_popup_active(&app.context)) { popup_action(desk_popup_key(&app.context,k));return; }
    if((k->flags&SRV_KEY_DOWN_EVENT) && !(k->flags&SRV_KEY_REPEAT)) {
        if(k->usage==SRV_KEY_ESCAPE && app.job.busy) { stop();return; }
        if(k->modifiers&(SRV_KEY_CTRL|SRV_KEY_META)) {
            unsigned id=k->usage==SRV_KEY_ENTER?NET_RUN:k->usage==26?NET_CLOSE:k->usage==30?NET_PING:k->usage==31?NET_LOOKUP:0;
            if(k->usage==6 && ((k->modifiers&SRV_KEY_SHIFT) || !app.target.focused)) id=NET_COPY;
            if(k->usage==15) { desk_ui_focus(&app.ui,NET_TARGET);desk_text_buffer_select(&app.target_text,0,app.target_text.length);desk_edit_reveal(&app.target);app.window.dirty=1;return; }
            if(id) { action((struct desk_action){id,DESK_WIDGET_ACTIVATED});return; }
        }
    }
    action(desk_ui_key(&app.ui,k));
}
static void menus(void) {
    unsigned busy=app.job.busy?DESK_MENU_DISABLED:0;
    struct desk_menu_item items[]={
        {0,DESK_MENU_HEADING,"Diagnostic",""},{NET_RUN,busy|(app.job.retiring_pid?DESK_MENU_DISABLED:0),"Run","Ctrl-Enter"},
        {NET_STOP,app.job.busy?0:DESK_MENU_DISABLED,"Stop","Esc"},{NET_COPY,!app.run_mode || app.clipboard.mode?DESK_MENU_DISABLED:0,"Copy report","Ctrl-Shift-C"},
        {NET_CLEAR,busy,"Clear results",""},{NET_CLOSE,0,"Close window","Ctrl-W"},
        {0,DESK_MENU_HEADING,"View",""},{NET_PING,busy|(page()==NET_PING?DESK_MENU_CHECKED:0),"Ping","Ctrl-1"},
        {NET_LOOKUP,busy|(page()==NET_LOOKUP?DESK_MENU_CHECKED:0),"DNS lookup","Ctrl-2"}
    };
    desk_app_menus(&app.window,items,sizeof(items)/sizeof(*items));desk_app_menu_tick(&app.window);
}
static void draw(struct desk_canvas *canvas) {
    const struct desk_theme *theme=&desk_theme_quiet;desk_fill(canvas,(struct desk_rect){0,0,canvas->width,canvas->height},theme->surface);
    desk_ui_draw(canvas,&app.ui,theme);desk_label_draw(canvas,(struct desk_rect){12,58,56,24},&desk_font_16,"Target",theme->muted);
    desk_label_ellipsis(canvas,(struct desk_rect){12,canvas->height-26,canvas->width-24,22},&desk_font_16,app.status,theme->muted);
    desk_popup_draw(canvas,&app.context,theme);
}
static void initialize(void) {
    app.body_size=16;desk_ui_init(&app.ui);desk_tabs_init(&app.tabs);desk_popup_init(&app.context);
    const struct desk_tab tabs[]={{NET_PING,"Ping",0},{NET_LOOKUP,"DNS lookup",0}};desk_tabs_items(&app.tabs,tabs,2);
    desk_text_buffer_init(&app.target_text,app.target_storage[0],app.target_storage[1],sizeof(app.target_storage[0]));desk_edit_init(&app.target,&app.target_text,0);
    struct srv_net_status_info network={0};if(!srv_net_status_info(&network) && network.initialized && network.router_ip) { char target[32];snprintf(target,sizeof(target),"%u.%u.%u.%u",network.router_ip>>24,(network.router_ip>>16)&255,(network.router_ip>>8)&255,network.router_ip&255);desk_text_buffer_set(&app.target_text,target,strlen(target)); }
    desk_document_init(&app.document);desk_clipboard_init(&app.clipboard,app.clipboard_data);desk_command_job_init(&app.job,app.output,sizeof(app.output));
    desk_ui_add(&app.ui,NET_TABS,DESK_CONTROL_TABS,&app.tabs,NULL);desk_ui_add(&app.ui,NET_TARGET,DESK_CONTROL_EDIT,&app.target,NULL);
    desk_ui_add(&app.ui,NET_RUN,DESK_CONTROL_BUTTON,&app.run,"Run");desk_ui_add(&app.ui,NET_STOP,DESK_CONTROL_BUTTON,&app.stop,"Stop");
    desk_ui_add(&app.ui,NET_COPY,DESK_CONTROL_BUTTON,&app.copy,"Copy report");desk_ui_add(&app.ui,NET_OUTPUT,DESK_CONTROL_DOCUMENT,&app.document,NULL);
    desk_button_centered(&app.run,1);desk_button_centered(&app.stop,1);desk_button_centered(&app.copy,1);report_view();status("Choose a target, then Run. Ctrl-L selects the target.");
}
int main(void) {
    initialize();if(desk_app_open(&app.window,"Network Utility",680,460)) return 1;desk_log("desknet: starting\n");
    while(!app.quit && !app.window.failed && !app.window.closed) {
        long now=srv_monotonic_us();app.now=now<0?(uint64_t)srv_ticks()*10000:(uint64_t)now;poll_job();
        if(desk_clipboard_tick(&app.clipboard,srv_ticks())<0) clipboard_finish(-1);
        for(unsigned batch=0;batch<32;batch++) {
            struct desk_packet p;int result=desk_app_poll(&app.window,&p);if(result<=0) break;if(result==2) continue;
            if(p.type==DESK_CONFIGURE) { layout();desk_ui_focus(&app.ui,NET_TARGET); }
            else if(p.type==DESK_FOCUS) { desk_ui_window_focus(&app.ui,p.value!=0);desk_popup_cancel(&app.context);app.window.dirty=1; }
            else if(p.type==DESK_PREFERENCES) { struct desk_preferences preferences;memcpy(&preferences,p.text,sizeof(preferences));app.body_size=preferences.text_size==20?20:16;layout(); }
            else if(p.type==DESK_MENU_ACTION) action((struct desk_action){p.value,DESK_WIDGET_ACTIVATED});
            else if(p.type==DESK_KEY) { struct srv_key_event k;memcpy(&k,p.text,sizeof(k));key(&k); }
            else if(p.type==DESK_CONTEXT) context(p.x,p.y,p.value!=0);
            else if(p.type==DESK_POINTER) {
                struct desk_widget_event e={.type=p.value&DESK_POINTER_CANCEL?DESK_WIDGET_CANCEL:DESK_WIDGET_POINTER,.x=p.x,.y=p.y,.down=(p.value&DESK_POINTER_LEFT)!=0,.pressed=(p.value&DESK_POINTER_PRESS)!=0};
                if(desk_popup_active(&app.context)) popup_action(desk_popup_pointer(&app.context,e));else action(desk_ui_pointer(&app.ui,e,app.now));
            } else if(p.type==DESK_SCROLL) { if(desk_popup_active(&app.context)) popup_action(desk_popup_scroll(&app.context,p.y));else action(desk_ui_scroll(&app.ui,p.width,p.height,p.x,p.y)); }
            else if(p.type==DESK_CLIPBOARD_ACK || p.type==DESK_CLIPBOARD_DATA) { int done=desk_clipboard_receive(&app.clipboard,&p);if(done) clipboard_finish(done); }
            else if(p.type==DESK_CLOSE_REQUEST) app.quit=1;
        }
        if(app.quit || app.window.closed || app.window.failed) break;
        menus();struct desk_canvas canvas;if(desk_app_canvas(&app.window,&canvas)==1) { draw(&canvas);desk_app_present(&app.window); }srv_sleep_ticks(1);
    }
    desk_command_job_close(&app.job);desk_app_close(&app.window);desk_log("desknet: exited failure=%d\n",app.window.failed);return app.window.failed?1:0;
}
