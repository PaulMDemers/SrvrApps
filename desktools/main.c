#include "app.h"
#include "runtime.h"
#include "calculator.h"
#include "timing.h"
#include <srvros/desk_log.h>
#include <srvros/sys.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static struct tool app;
static const char *roles[]={"files","text","calculator","system","settings"};
static const char *titles[]={"Files","Text — Untitled","Calculator","System","Settings"};
void tool_status(struct tool *t,const char *message) {
    snprintf(t->status,sizeof(t->status),"%s",message);if(t->picker_mode) desk_picker_message(&t->picker,message);
    if(t->prompt_mode) snprintf(t->prompt.dialog.message,sizeof(t->prompt.dialog.message),"%s",message);t->window.dirty=1;
}
void tool_launch(struct tool *t,int application,const char *path) {
    if(desk_app_launch(&t->window,application,path)) tool_status(t,"The application could not be opened. Try again.");
}
void tool_dialog(struct tool *t,int mode) {
    tool_text_search_close(t);t->modal=mode;desk_ui_window_focus(&t->ui,0);
    struct desk_dialog_choice choices[]={ {1001,"Save",1},{1002,"Discard",1},{1003,"Cancel",1} };
    if(mode==2) { strcpy(choices[0].label,"Replace");choices[1]=choices[2]; }
    desk_dialog_open(&t->dialog,mode==2?"Replace this file?":"Save your changes?",
        mode==2?"A file already exists at this path. Replace saves this document over it. Cancel keeps the existing file.":
        "This document has unsaved edits. Save keeps them on disk. Discard loses these edits. Cancel returns to the document.",
        choices,mode==2?2:3,1003,1003);
    tool_layout(t);desk_dialog_focus(&t->dialog,t->window.focused);t->window.dirty=1;
}
void tool_layout(struct tool *t) {
    desk_popup_cancel(&t->context);
    int w=t->window.width,h=t->window.height;
    if(w<280 || h<240) return;
    if(t->info_mode) { tool_files_info_layout(t);return; }
    int busy=t->job.busy,read_busy=busy && t->job.request.operation==DESK_FILE_READ;
    if(t->role==DESK_APP_TEXT) {
        int replacing=t->search_open && t->search.replacing;
        for(int i=0;i<3;i++) desk_button_configure(&t->buttons[i],replacing?(struct desk_rect){0}:(struct desk_rect){16+i*84,16,76,28},!busy && !replacing);
        desk_edit_configure(&t->path,replacing?(struct desk_rect){0}:(struct desk_rect){16,56,w-32,34},&desk_font_16,!read_busy && !replacing);
        int top=replacing?168:t->search_open?182:102;
        if(t->search_open) desk_searchbar_configure(&t->search,(struct desk_rect){16,replacing?16:102,w-32,replacing?144:72},!read_busy);
        desk_edit_configure(&t->body,(struct desk_rect){16,top,w-32,h-42-top},t->body_size==20?&desk_font_20:&desk_font_16,!read_busy);
    } else if(t->role==DESK_APP_FILES) tool_files_layout(t);
    else if(t->role==DESK_APP_CALCULATOR) {
        tool_calculator_layout(t);
    } else if(t->role==DESK_APP_SYSTEM) {
        tool_system_layout(t);
    } else if(t->role==DESK_APP_SETTINGS) {
        tool_settings_pointer(t,(struct desk_widget_event){.type=DESK_WIDGET_CANCEL});tool_settings_layout(t);
    }
    desk_ui_layout_changed(&t->ui);
    if(t->picker_mode) desk_picker_configure(&t->picker,(struct desk_rect){0,0,w,h});
    if(t->prompt_mode) desk_prompt_configure(&t->prompt,(struct desk_rect){0,0,w,h});
    if(t->modal) desk_dialog_configure(&t->dialog,(struct desk_rect){0,0,w,h});
    t->window.dirty=1;
}
void tool_dispatch(struct tool *t,struct desk_action a) {
    if(a.flags&DESK_WIDGET_REDRAW) t->window.dirty=1;
    tool_clipboard_action(t,a);
    if(t->role==DESK_APP_TEXT) {
        if(!(a.flags&DESK_WIDGET_ACTIVATED) || !tool_command_action(t,a.id))tool_text_action(t,a);
    }
    else if(t->role==DESK_APP_FILES) tool_files_action(t,a);
    else if(t->role==DESK_APP_CALCULATOR) tool_calculator_action(t,a);
    else if(t->role==DESK_APP_SETTINGS) tool_settings_action(t,a);
    else if(t->role==DESK_APP_SYSTEM) tool_system_action(t,a);
}
static void key(struct tool *t,const struct srv_key_event *k) {
    if(tool_command_key(t,k))return;
    if(t->info_mode) { tool_files_info_key(t,k);return; }
    if(desk_popup_active(&t->context)) { tool_context_result(t,desk_popup_key(&t->context,k));return; }
    if(t->role==DESK_APP_SETTINGS && (k->flags&SRV_KEY_DOWN_EVENT) &&
        !(k->modifiers&(SRV_KEY_CTRL|SRV_KEY_ALT|SRV_KEY_META)) &&
        (k->usage==SRV_KEY_PAGEUP || k->usage==SRV_KEY_PAGEDOWN)) {
        tool_settings_scroll(t,(t->window.height-108)*(k->usage==SRV_KEY_PAGEUP?-1:1));return;
    }
    if(t->prompt_mode) { tool_files_prompt(t,desk_prompt_key(&t->prompt,k));return; }
    if(t->picker_mode) { tool_picker_result(t,desk_picker_key(&t->picker,k));return; }
    if(t->modal) {
        struct desk_action a=desk_dialog_key(&t->dialog,k);
        if(a.flags&DESK_WIDGET_REDRAW) t->window.dirty=1;
        if(a.flags&DESK_WIDGET_ACTIVATED) { if(t->role==DESK_APP_FILES) tool_files_dialog(t,a.id);else tool_text_dialog(t,a.id); }
        return;
    }
    if((k->flags&SRV_KEY_DOWN_EVENT) && (k->modifiers&(SRV_KEY_CTRL|SRV_KEY_META))) {
        if(k->usage==0x1a) { if(t->role!=DESK_APP_TEXT)t->closing=1;return; }
        if(t->role==DESK_APP_FILES) {
            if(k->usage==12) { tool_files_info_open(t);return; }
            if(k->usage==9 && !(k->flags&SRV_KEY_REPEAT)) { tool_files_action(t,(struct desk_action){TOOL_FIND,DESK_WIDGET_ACTIVATED});return; }
            if(k->usage==0x0f) { desk_ui_focus(&t->ui,TOOL_PATH);desk_text_buffer_select(&t->path_text,0,t->path_text.length);t->window.dirty=1;return; }
            if(k->usage==0x15) { tool_files_action(t,(struct desk_action){TOOL_REFRESH,DESK_WIDGET_ACTIVATED});return; }
        }
        if(t->role==DESK_APP_TEXT) {
            if(k->usage==0x0b) { tool_text_replace_open(t);return; }
            if(k->usage==0x09) { tool_text_search_open(t);return; }
            if(k->usage==0x0a) { tool_text_search_step(t,(k->modifiers&SRV_KEY_SHIFT)!=0,0);return; }
            if(k->usage==0x0f) { if(t->search_open && t->search.replacing) tool_text_search_close(t);t->search_focus=0;tool_content_focus(t,t->window.focused);desk_ui_focus(&t->ui,TOOL_PATH);desk_text_buffer_select(&t->path_text,0,t->path_text.length);t->window.dirty=1;return; }
        }
    }
    if(t->role==DESK_APP_FILES && (k->flags&SRV_KEY_DOWN_EVENT) && !(k->modifiers&(SRV_KEY_CTRL|SRV_KEY_ALT|SRV_KEY_META)) &&
        t->ui.focus>=0 && t->ui.controls[t->ui.focus].id==TOOL_LIST && (k->usage==0x3b || k->usage==SRV_KEY_DELETE)) {
        tool_files_action(t,(struct desk_action){k->usage==0x3b?TOOL_RENAME:TOOL_DELETE,DESK_WIDGET_ACTIVATED});return;
    }
    if(t->role==DESK_APP_FILES && (k->flags&SRV_KEY_DOWN_EVENT) && (k->modifiers&SRV_KEY_ALT)) {
        unsigned action=k->usage==SRV_KEY_UP?TOOL_UP:k->usage==SRV_KEY_LEFT?TOOL_BACK:k->usage==SRV_KEY_RIGHT?TOOL_FORWARD:k->usage==SRV_KEY_HOME?TOOL_HOME:0;
        if(action) { tool_files_action(t,(struct desk_action){action,DESK_WIDGET_ACTIVATED});return; }
    }
    if(t->role==DESK_APP_FILES && (k->flags&SRV_KEY_DOWN_EVENT) && k->usage==SRV_KEY_F5) { tool_files_action(t,(struct desk_action){TOOL_REFRESH,DESK_WIDGET_ACTIVATED});return; }
    if(t->role==DESK_APP_SYSTEM && (k->flags&SRV_KEY_DOWN_EVENT) && k->usage==0x3e) { tool_system_refresh(t,1);return; }
    if(t->role==DESK_APP_TEXT) {
        if((k->flags&SRV_KEY_DOWN_EVENT) && k->usage==0x3c) { tool_text_search_step(t,(k->modifiers&SRV_KEY_SHIFT)!=0,0);return; }
        if(t->search_open && (k->flags&SRV_KEY_DOWN_EVENT) && k->usage==SRV_KEY_ESCAPE) { tool_text_search_close(t);return; }
        if(t->search_open && t->search_focus) { tool_text_search_action(t,desk_searchbar_key(&t->search,k));return; }
    }
    if(t->role==DESK_APP_CALCULATOR && tool_calculator_key(t,k)) return;
    tool_dispatch(t,desk_ui_key(&t->ui,k));
}
static void draw(struct tool *t,struct desk_canvas *c) {
    if(t->info_mode) { tool_files_info_draw(t,c);return; }
    const struct desk_theme *theme=&desk_theme_quiet;
    desk_fill(c,(struct desk_rect){0,0,c->width,c->height},theme->surface);
    if(t->role==DESK_APP_SETTINGS) tool_settings_draw(t,c);else if(t->role==DESK_APP_FILES) tool_files_draw(t,c);else desk_ui_draw(c,&t->ui,theme);
    if(t->search_open) desk_searchbar_draw(c,&t->search,theme);
    if(t->role==DESK_APP_CALCULATOR) tool_calculator_draw(t,c);
    else if(t->role==DESK_APP_SYSTEM) {
        tool_system_draw(t,c);
    }
    desk_label_draw(c,(struct desk_rect){16,c->height-28,c->width-32,24},theme->body,t->status,theme->muted);
    if(t->picker_mode && !t->dialog_separate) desk_picker_draw(c,&t->picker,theme);
    if(t->modal && !t->dialog_separate) desk_dialog_draw(c,&t->dialog,theme);
    desk_popup_draw(c,&t->context,theme);
    if(t->prompt_mode && !t->dialog_separate) desk_prompt_draw(c,&t->prompt,theme);
}
/* Explicit diagnostic only: snapshots of the RAM document, never user files. */
struct capture_snapshot { uint64_t serial;unsigned length,caret,anchor,busy;char hex[129]; };
static struct capture_snapshot capture_states[5];
static struct capture_snapshot capture_state(struct tool *t,uint64_t serial) {
    struct capture_snapshot v={.serial=serial,.length=t->body_text.length,
        .caret=t->body_text.caret,.anchor=t->body_text.anchor,.busy=t->clip.mode!=0};
    static const char digits[]="0123456789abcdef";
    for(unsigned i=0;i<t->body_text.length && i<64;i++) {
        unsigned char c=t->body_text.data[i];v.hex[2*i]=digits[c>>4];v.hex[2*i+1]=digits[c&15];
    }
    return v;
}
static void capture_print(const char *kind,struct capture_snapshot v) {
    desk_log("text-coexist: %s serial=%llu length=%u caret=%u anchor=%u clipboard_busy=%u hex=%s\n",
        kind,(unsigned long long)v.serial,v.length,v.caret,v.anchor,v.busy,v.hex[0]?v.hex:"-");
}
void tool_runtime_init(struct tool *t,int role) {
    uint64_t timing=tool_timing_begin(role,0,"runtime-init");
    t->role=role;t->body_size=16;t->title_dirty=1;
    desk_ui_init(&t->ui);desk_dialog_init(&t->dialog);desk_prompt_init(&t->prompt);desk_picker_init(&t->picker);
    desk_chooser_init(&t->chooser,&t->picker,&t->job,t->entries,t->items,t->details);
    desk_clipboard_init(&t->clip,t->clipboard);
    desk_text_buffer_init(&t->path_text,t->path_store[0],t->path_store[1],DESK_FILE_PATH);
    desk_text_buffer_init(&t->body_text,t->body_store[0],t->body_store[1],t->role==DESK_APP_CALCULATOR?CALC_EXPRESSION_BYTES:DESK_FILE_BYTES+1);
    desk_popup_init(&t->context);
    desk_text_buffer_init(&t->name_text,t->name_store[0],t->name_store[1],128);
    desk_edit_init(&t->path,&t->path_text,0);desk_edit_init(&t->body,&t->body_text,t->role==DESK_APP_TEXT);
    desk_edit_init(&t->name,&t->name_text,0);
    tool_timing_end(role,0,"runtime-init",timing);
}
void tool_runtime_cancel_modal(struct tool *t) {
    if(t->info_mode)tool_files_info_close(t);
    if(t->prompt_mode)tool_files_prompt(t,(struct desk_action){DESK_PROMPT_CANCEL,DESK_WIDGET_ACTIVATED});
    if(t->modal) {if(t->role==DESK_APP_FILES)tool_files_dialog(t,1003);else tool_text_dialog(t,1003);}
    if(t->picker_mode)tool_picker_cancel(t);
}
void tool_runtime_close_request(struct tool *t) {
    desk_log("window-lifecycle: close-request role=%d window=%llu busy=%u operation=%u\n",t->role,
        (unsigned long long)t->window.window,(unsigned)t->job.busy,t->job.busy?t->job.request.operation:0);
    desk_popup_cancel(&t->context);
    if(t->role==DESK_APP_TEXT) {
        if(t->picker_mode)tool_picker_cancel(t);
        t->close_canceled=0;tool_text_request(t,TOOL_PENDING_CLOSE);
    } else {tool_runtime_cancel_modal(t);t->closing=1;}
}
void tool_runtime_event(struct tool *t,struct desk_packet p) {
    if(p.type==DESK_CLIPBOARD_ACK || p.type==DESK_CLIPBOARD_DATA) {
        int result=desk_clipboard_receive(&t->clip,&p);if(result) tool_clipboard_result(t,result);
    } else if(p.type==DESK_CONFIGURE) { tool_layout(t);if(t->role==DESK_APP_TEXT || t->role==DESK_APP_CALCULATOR) desk_ui_focus(&t->ui,TOOL_BODY); }
    else if(p.type==DESK_AUDIO && t->role==DESK_APP_SETTINGS) tool_settings_audio(t,&p);
    else if(p.type==DESK_BRIGHTNESS && t->role==DESK_APP_SETTINGS) tool_settings_brightness(t,&p);
    else if(p.type==DESK_PREFERENCES) {
        if(t->role==DESK_APP_SETTINGS) tool_settings_receive(t,&p);
        else {
            struct desk_preferences preferences;memcpy(&preferences,p.text,sizeof(preferences));
            if(desk_preferences_valid(&preferences) && t->body_size!=(int)preferences.text_size) { t->body_size=preferences.text_size;tool_layout(t); }
        }
    } else if(p.type==DESK_CONTEXT) tool_context_open(t,p.x,p.y,p.value!=0);
    else if(p.type==DESK_FOCUS) {
        if(t->role==DESK_APP_SETTINGS && p.value) t->brightness_refresh=1;
        if(!p.value) desk_popup_cancel(&t->context);
        if(t->role==DESK_APP_SETTINGS && !p.value) tool_settings_pointer(t,(struct desk_widget_event){.type=DESK_WIDGET_CANCEL});
        if(t->info_mode) desk_ui_window_focus(&t->info_ui,p.value!=0);else if(t->prompt_mode) desk_prompt_focus(&t->prompt,p.value!=0);else if(t->picker_mode) desk_picker_focus(&t->picker,p.value!=0);else if(t->modal) desk_dialog_focus(&t->dialog,p.value!=0);else tool_content_focus(t,p.value!=0);t->window.dirty=1;
    } else if(p.type==DESK_MENU_ACTION) tool_menu_action(t,p.value);
    else if(p.type==DESK_KEY) {
        struct srv_key_event k;memcpy(&k,p.text,sizeof(k));key(t,&k);

    }
    else if(p.type==DESK_POINTER) {
        struct desk_widget_event e={.type=p.value&DESK_POINTER_CANCEL?DESK_WIDGET_CANCEL:DESK_WIDGET_POINTER,
            .x=p.x,.y=p.y,.down=(p.value&DESK_POINTER_LEFT)!=0,.pressed=(p.value&DESK_POINTER_PRESS)!=0};
        if(t->info_mode) { tool_files_info_action(t,desk_ui_pointer(&t->info_ui,e,srv_monotonic_us()));return; }
        if(desk_popup_active(&t->context)) { tool_context_result(t,desk_popup_pointer(&t->context,e));return; }
        if(t->role==DESK_APP_SETTINGS && tool_settings_pointer(t,e)) return;
        if(t->prompt_mode) { tool_files_prompt(t,desk_prompt_pointer(&t->prompt,e,srv_monotonic_us()));return; }
        if(t->picker_mode) { tool_picker_result(t,desk_picker_pointer(&t->picker,e,srv_monotonic_us()));return; }
        if(t->search_open && !t->modal) {
            if(e.type==DESK_WIDGET_CANCEL) tool_text_search_action(t,desk_searchbar_pointer(&t->search,e,srv_monotonic_us()));
            if(e.pressed) { t->search_focus=desk_widget_contains(t->search.frame,e.x,e.y);tool_content_focus(t,t->window.focused); }
            if(t->search_focus) { tool_text_search_action(t,desk_searchbar_pointer(&t->search,e,srv_monotonic_us()));return; }
        }
        struct desk_action a=t->modal?desk_dialog_pointer(&t->dialog,e,srv_monotonic_us()):desk_ui_pointer(&t->ui,e,srv_monotonic_us());
        if(t->modal) { if(a.flags&DESK_WIDGET_REDRAW) t->window.dirty=1;if(a.flags&DESK_WIDGET_ACTIVATED) { if(t->role==DESK_APP_FILES) tool_files_dialog(t,a.id);else tool_text_dialog(t,a.id); } }
        else tool_dispatch(t,a);
    } else if(p.type==DESK_SCROLL && !t->modal && !t->prompt_mode) {
        if(t->info_mode) tool_files_info_action(t,desk_ui_scroll(&t->info_ui,p.width,p.height,p.x,p.y));
        else if(desk_popup_active(&t->context)) tool_context_result(t,desk_popup_scroll(&t->context,p.y));
        else if(t->role==DESK_APP_SETTINGS) tool_settings_scroll(t,p.y);
        else if(t->picker_mode) tool_picker_result(t,desk_picker_scroll(&t->picker,p.width,p.height,p.y));
        else tool_dispatch(t,desk_ui_scroll(&t->ui,p.width,p.height,p.x,p.y));
    }
    else if(p.type==DESK_CLOSE_REQUEST) {
        tool_timing_event(t->role,t->window.window,"close-received");
        tool_runtime_close_request(t);
    }
}
int tool_runtime_file_poll(struct tool *t,uint64_t *logged_job) {
    if(!t->job.busy)return 0;
    /* Observe admission before reading a reply: a submitted job may
     * outlive its window and must remain owned during session exit. */
    int submitted=desk_file_job_submit(&t->job);
    if(submitted>0 && *logged_job!=t->job.request.id) {
        *logged_job=t->job.request.id;
        desk_log("desktools: file-submitted role=%s op=%u id=%llu worker=%llu\n",roles[t->role-1],t->job.request.operation,
            (unsigned long long)*logged_job,(unsigned long long)t->job.pid);
    }
    int result=submitted<0?-1:submitted>0?desk_file_job_poll(&t->job):0;
    if(result) desk_log("desktools: file-result role=%s op=%u status=%u bytes=%u\n",roles[t->role-1],t->job.request.operation,t->job.response.status,t->job.response.length);
    return result;
}
int tool_file_pool_retire(struct desk_file_pool *pool,int role) {
    uint64_t timing=tool_timing_begin(role,0,"pool-retire");
    struct tool_phase_trace phase;tool_phase_begin(&phase);
    int result;
    while(!(result=desk_file_pool_finish(pool)))srv_sleep_ticks(1);
    tool_phase_end(&phase,role,0,"pool-retire");
    desk_log("file-pool: retired role=%s spawned=%llu reaped=%llu peak=%llu live=%llu bound=%u\n",
        roles[role-1],(unsigned long long)pool->spawned,(unsigned long long)pool->reaped,
        (unsigned long long)pool->peak,(unsigned long long)pool->live,pool->bound);
    tool_timing_end(role,0,"pool-retire",timing);
    return result==1?0:-1;
}
void tool_runtime_draw(struct tool *t) {
    int first=!t->window.serial && t->window.surface;
    uint64_t at=0;struct tool_phase_trace phase;
    if(first) {at=tool_timing_begin(t->role,t->window.window,"first-controls");tool_phase_begin(&phase);}
    if((t->role==DESK_APP_TEXT || t->role==DESK_APP_FILES) && t->title_dirty && t->window.server) {
        const char *name=t->saved_path[0]?strrchr(t->saved_path,'/')+1:t->role==DESK_APP_TEXT?"Untitled":"Opening";char title[88];
        if(t->role==DESK_APP_TEXT)snprintf(title,sizeof(title),"Text — %s%s",name,tool_modified(t)?" *":"");
        else snprintf(title,sizeof(title),"Files — %s",name[0]?name:"System root");
        size_t length=strlen(title);while(length && !desk_text_utf8_valid(title,length))title[--length]=0;
        if(!desk_app_title(&t->window,title)) t->title_dirty=0;
    }
    tool_menu_refresh(t);desk_app_menu_tick(&t->window);
    if(first) {tool_phase_end(&phase,t->role,t->window.window,"first-controls");tool_timing_end(t->role,t->window.window,"first-controls",at);}
    struct desk_canvas c;if(desk_app_canvas(&t->window,&c)==1) {
        if(first) {at=tool_timing_begin(t->role,t->window.window,"first-draw");tool_phase_begin(&phase);}
        draw(t,&c);
        if(first) {tool_phase_end(&phase,t->role,t->window.window,"first-draw");tool_timing_end(t->role,t->window.window,"first-draw",at);}
        if(first) {at=tool_timing_begin(t->role,t->window.window,"first-present");tool_phase_begin(&phase);}
        int published=desk_app_present(&t->window);
        if(first) {tool_phase_end(&phase,t->role,t->window.window,"first-present");tool_timing_end(t->role,t->window.window,"first-present",at);}
        if(first && published==1)tool_timing_event(t->role,t->window.window,"first-publish");
    }
}
int tool_file_pool_capture(void);
int tool_wait_capture(void);
int tool_multi_capture(void);
int tool_growth_capture(void);
int tool_chooser_capture(void);
int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"text") && !strcmp(argv[2],"--chooser-capture"))return tool_chooser_capture();
    if(argc==3 && !strcmp(argv[1],"text") && !strcmp(argv[2],"--file-pool-wait-capture"))return tool_wait_capture();
    if(argc==3 && !strcmp(argv[1],"text") && !strcmp(argv[2],"--file-pool-capture"))return tool_file_pool_capture();
    if(argc==3 && !strcmp(argv[1],"text") && !strcmp(argv[2],"--multi-window-capture"))return tool_multi_capture();
    if(argc==3 && !strcmp(argv[1],"text") && !strcmp(argv[2],"--window-growth-capture"))return tool_growth_capture();
    struct tool *t=&app;t->role=DESK_APP_TEXT;t->body_size=16;t->title_dirty=1;
    if(argc>1) for(unsigned i=0;i<sizeof(roles)/sizeof(*roles);i++) if(!strcmp(argv[1],roles[i])) t->role=i+1;
    tool_timing_event(t->role,0,"main-entry");
    int controls_cancel=t->role==DESK_APP_TEXT && argc==3 && !strcmp(argv[2],"--coexist-control-files-cancel");
    int controls_files=controls_cancel || (t->role==DESK_APP_TEXT && argc==3 && !strcmp(argv[2],"--coexist-control-files"));
    int workspace_capture=controls_files || (t->role==DESK_APP_TEXT && argc==3 && !strcmp(argv[2],"--coexist-workspace"));
    if(controls_files)tool_file_capture_controls_enable(controls_cancel);
    int file_capture=workspace_capture || (t->role==DESK_APP_TEXT && argc==3 && !strcmp(argv[2],"--coexist-files"));
    if(file_capture)tool_file_capture_setup(workspace_capture);
    int coexist=file_capture || (t->role==DESK_APP_TEXT && argc==3 && !strcmp(argv[2],"--coexist-capture"));
    int battery_capture=t->role==DESK_APP_SYSTEM && argc==3 && !strcmp(argv[2],"--battery-control-capture");
    if((t->role==DESK_APP_TEXT && !coexist) || t->role==DESK_APP_FILES) {
        int result=tool_window_runtime(t->role,argc>2?argv[2]:"");
        tool_timing_event(t->role,0,"main-return");return result;
    }
    unsigned captured=0;uint64_t logged_job=0;
    struct desk_file_pool pool;struct desk_file_channel channels[2];
    int pooled=t->role==DESK_APP_FILES;
    tool_runtime_init(t,t->role);
    uint64_t timing=tool_timing_begin(t->role,0,"pool-init");
    if(pooled)t->worker_ready=desk_file_pool_init(&pool,channels,2)==0 && desk_file_job_bind(&t->job,&pool)==0;
    else if((!coexist || file_capture) && t->role==DESK_APP_TEXT)t->worker_ready=desk_file_job_init(&t->job)==0;
    tool_timing_end(t->role,0,"pool-init",timing);
    desk_log("desktools: starting role=%s\n",roles[t->role-1]);
    timing=tool_timing_begin(t->role,0,"window-open");
    int opened=desk_app_open(&t->window,titles[t->role-1],t->role==DESK_APP_SYSTEM?760:t->role==DESK_APP_CALCULATOR?640:t->role<=2?720:560,t->role==DESK_APP_SYSTEM?500:t->role==DESK_APP_CALCULATOR?520:t->role==DESK_APP_SETTINGS?560:t->role<=2?480:320);
    tool_timing_end(t->role,0,"window-open",timing);if(opened)return 1;
    desk_log("desktools: window-request role=%s pid=%llu at_us=%llu\n",roles[t->role-1],(unsigned long long)srv_getpid(),(unsigned long long)srv_monotonic_us());
    timing=tool_timing_begin(t->role,t->window.window,"role-init");
    if(t->role==DESK_APP_TEXT) tool_text_init(t,!coexist && argc>2?argv[2]:"");
    else if(t->role==DESK_APP_FILES) tool_files_init(t,argc>2?argv[2]:"");
    else if(t->role==DESK_APP_CALCULATOR) tool_calculator_init(t);
    else if(t->role==DESK_APP_SETTINGS) tool_settings_init(t);
    else if(t->role==DESK_APP_SYSTEM) {
        tool_system_init(t);
        if(argc==3 && !strcmp(argv[2],"--battery-controls")) {desk_tabs_select(&t->system_tabs,SYSTEM_SENSORS);tool_system_layout(t);desk_ui_layout_changed(&t->ui);}
    }
    tool_timing_end(t->role,t->window.window,"role-init",timing);
    while(!t->quit && !t->window.failed && !t->window.closed) {
        if(t->role==DESK_APP_SETTINGS) tool_settings_tick(t);
        if(desk_clipboard_tick(&t->clip,srv_ticks())<0) tool_clipboard_result(t,-1);
        for(unsigned n=0;n<32;n++) {
            struct desk_packet p;int r=desk_app_poll(&t->window,&p);
            if(r<=0) break;
            if(r==2) continue;
            tool_runtime_event(t,p);
            if(p.type==DESK_KEY) {
                if(coexist && p.input_serial && p.input_serial<=60 && p.input_serial%12==0 && captured<5) {
                    if(file_capture) tool_file_capture_begin(t,p.input_serial);
                    else capture_states[captured++]=capture_state(t,p.input_serial);
                }
            }
        }
        int poll_file=!controls_files || tool_file_capture_controls_tick(t);
        if(controls_cancel && t->quit)break;
        if(poll_file && t->job.busy) {
            int result=tool_runtime_file_poll(t,&logged_job);if(result) {
            if(file_capture && tool_file_capture_busy()) {
                tool_file_capture_finish(t,result);
                if(!tool_file_capture_busy() && !t->window.failed && captured<5)
                    capture_states[captured++]=capture_state(t,t->window.input_serial);
            } else if(t->chooser.pending) tool_picker_finish(t,result);else if(t->role==DESK_APP_TEXT) tool_text_finish(t,result);else tool_files_finish(t,result);
        } }
        if(t->closing && !t->job.busy && t->role!=DESK_APP_TEXT) t->quit=1;
        if(t->role==DESK_APP_SYSTEM) tool_system_refresh(t,0);
        if(battery_capture)tool_battery_capture_tick(t);
        tool_runtime_draw(t);
        /* Text has no periodic animation when quiescent. Queue readiness wakes
         * it immediately on input/release/close; keep job/clipboard/diagnostic
         * progress and unfinished publication on the existing one-tick path. */
        if(pooled && !t->quit && !t->closing && !t->clip.mode && !t->title_dirty) {
            struct srv_gui_wait wait={.deadline_us=(uint64_t)srv_monotonic_us()+100000};
            if(desk_file_pool_interests(&pool,&wait)!=1 || desk_app_wait_io(&t->window,&wait)<0)srv_sleep_ticks(1);
        } else if(t->role!=DESK_APP_TEXT || coexist || t->quit || t->closing ||
           t->job.busy || t->clip.mode || t->title_dirty || desk_app_wait(&t->window,10)<0)
            srv_sleep_ticks(1);
    }
    if(coexist) {
        for(unsigned i=0;i<captured;i++) capture_print("checkpoint",capture_states[i]);
        capture_print("final",capture_state(t,t->window.input_serial));
        desk_log("text-coexist: file_worker=%s checkpoints=%u\n",file_capture?(t->worker_ready?"enabled":"MISSING"):(t->worker_ready?"UNEXPECTED":"disabled"),captured);
        if(file_capture) { if(!tool_file_capture_report() || tool_file_capture_cycles()!=5 || tool_file_capture_busy()) t->window.failed=1; }
    }
    tool_timing_event(t->role,t->window.window,"loop-exit");
    timing=tool_timing_begin(t->role,t->window.window,"history-free");
    tool_text_history_release(t);
    tool_timing_end(t->role,t->window.window,"history-free",timing);
    timing=tool_timing_begin(t->role,t->window.window,"job-drain");
    if(t->worker_ready && pooled) {
        /* Even a failed GUI connection cannot abandon an accepted mutation. */
        while(t->job.busy) { if(tool_runtime_file_poll(t,&logged_job)<0)t->window.failed=1;if(t->job.busy)srv_sleep_ticks(1); }
        if(desk_file_job_detach(&t->job) || tool_file_pool_retire(&pool,t->role))t->window.failed=1;
    } else if(t->worker_ready)desk_file_job_close(&t->job);
    tool_timing_end(t->role,t->window.window,"job-drain",timing);
    timing=tool_timing_begin(t->role,t->window.window,"window-close");
    desk_app_close(&t->window);
    tool_timing_end(t->role,t->window.window,"window-close",timing);
    desk_log("desktools: exited role=%s failure=%d\n",roles[t->role-1],t->window.failed);
    tool_timing_event(t->role,0,"main-return");
    return t->window.failed?1:0;
}
