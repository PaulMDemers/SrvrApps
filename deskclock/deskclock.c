#include <srvros/desk_app.h>
#include <srvros/desk_ui.h>
#include <srvros/desk_dialog.h>
#include <srvros/desk_notice.h>
#include <srvros/desk_time_model.h>
#include <srvros/desk_notify.h>
#include <srvros/desk_clipboard.h>
#include <srvros/desk_log.h>
#include <srvros/sys.h>
#include <stdio.h>
#include <string.h>
enum { CLOCK_TABS=1,CLOCK_DURATION,CLOCK_TOGGLE,CLOCK_RESET,CLOCK_LAP,CLOCK_COPY,CLOCK_LAPS,
    CLOCK_TIMER,CLOCK_STOPWATCH,CLOCK_COPY_LAPS,CLOCK_CLOSE,CLOCK_KEEP_OPEN,CLOCK_CONFIRM_CLOSE };
#define CLOCK_LAP_LIMIT 64
#define CLOCK_CLIP_DISCARD (1u<<30)
struct clock_app {
    struct desk_app window;
    struct desk_ui ui;
    struct desk_tabs tabs;
    struct desk_edit duration;
    struct desk_text_buffer duration_text;
    char duration_storage[2][24];
    struct desk_button toggle,reset,lap,copy;
    struct desk_table table;
    struct desk_table_row rows[CLOCK_LAP_LIMIT];
    struct { uint64_t id,total,split;char number[24],elapsed[16],interval[16]; } laps[CLOCK_LAP_LIMIT];
    unsigned lap_count;
    uint64_t lap_serial,last_lap,now,display_tick;
    struct desk_countdown timer;
    struct desk_stopwatch watch;
    struct desk_dialog dialog;
    struct desk_notice explanation;
    struct desk_notify_client notify;
    struct desk_clipboard_client clipboard;
    unsigned char clipboard_data[DESK_CLIPBOARD_BYTES+1];
    unsigned clip_action;
    uint64_t clip_revision;
    size_t clip_caret,clip_anchor;
    char status[128];
    int modal,quit,title_dirty;
};
static struct clock_app app;
static int timer_page(void) { return desk_tabs_selected(&app.tabs)==CLOCK_TIMER; }
static void status(const char *text) { snprintf(app.status,sizeof(app.status),"%s",text);app.window.dirty=1; }
static void short_time(char *output,size_t capacity,uint64_t us) {
    char full[16];desk_duration_format(full,sizeof(full),us,1);snprintf(output,capacity,"%s",us<UINT64_C(3600000000)?full+3:full);
}
static void rows(void) {
    for(unsigned i=0;i<app.lap_count;i++) {
        snprintf(app.laps[i].number,sizeof(app.laps[i].number),"%llu",(unsigned long long)app.laps[i].id);
        short_time(app.laps[i].elapsed,sizeof(app.laps[i].elapsed),app.laps[i].total);short_time(app.laps[i].interval,sizeof(app.laps[i].interval),app.laps[i].split);
        app.rows[i]=(struct desk_table_row){.id=app.laps[i].id,.cells={app.laps[i].number,app.laps[i].elapsed,app.laps[i].interval}};
    }
    desk_table_rows(&app.table,app.rows,app.lap_count);
}
static void layout(void) {
    int width=app.window.width,height=app.window.height,timer=timer_page(),compact=width<400;
    desk_tabs_configure(&app.tabs,(struct desk_rect){12,12,width-24,30},1);
    desk_edit_configure(&app.duration,timer?(struct desk_rect){100,146,width-112,28}:(struct desk_rect){0},&desk_font_mono_16,timer && !app.timer.running);
    int x=12,y=timer?184:132,toggle_width=compact?76:100,small=compact?54:76;
    desk_button_configure(&app.toggle,(struct desk_rect){x,y,toggle_width,28},timer || desk_stopwatch_value(&app.watch,app.now)<DESK_STOPWATCH_MAX_US);x+=toggle_width+8;
    desk_button_configure(&app.lap,timer?(struct desk_rect){0}:(struct desk_rect){x,y,compact?48:76,28},!timer && app.watch.running);
    if(!timer) x+=(compact?48:76)+8;
    desk_button_configure(&app.reset,(struct desk_rect){x,y,small,28},timer?(app.timer.running || app.timer.finished || app.timer.remaining<app.timer.duration):(!app.watch.running && app.watch.elapsed>0));x+=small+8;
    desk_button_configure(&app.copy,(struct desk_rect){x,y,small,28},!app.clipboard.mode);
    app.ui.controls[2].label=timer?(app.timer.running?"Pause":app.timer.finished?"Again":app.timer.remaining<app.timer.duration?"Resume":"Start"):(app.watch.running?"Pause":app.watch.elapsed?"Resume":"Start");
    static const struct desk_table_column columns[]={{"Lap",48,0,0},{"Elapsed",0,0,0},{"Split",0,0,0}};
    desk_table_columns(&app.table,columns,compact?2:3);
    int table_visible=!timer && height>=290;
    desk_table_configure(&app.table,table_visible?(struct desk_rect){12,172,width-24,height-210}:(struct desk_rect){0},table_visible);
    desk_notice_configure(&app.explanation,timer && width>=400 && height>=430?(struct desk_rect){12,236,width-24,height-278}:(struct desk_rect){0},&desk_font_16);
    desk_dialog_configure(&app.dialog,(struct desk_rect){0,0,width,height});
    desk_ui_layout_changed(&app.ui);desk_ui_window_focus(&app.ui,app.window.focused && !app.modal);desk_dialog_focus(&app.dialog,app.window.focused && app.modal);app.window.dirty=1;
}
static void page(unsigned id) {
    if(app.clip_action && app.clip_action!=CLOCK_CLIP_DISCARD) app.clip_action=CLOCK_CLIP_DISCARD;
    if(desk_tabs_select(&app.tabs,id)<0) return;
    app.title_dirty=1;layout();desk_ui_focus(&app.ui,CLOCK_TABS);desk_log("deskclock: page=%s\n",timer_page()?"timer":"stopwatch");
}
static void completed(void) {
    char duration[16],message[DESK_NOTIFY_MESSAGE];desk_duration_format(duration,sizeof(duration),app.timer.duration,0);
    snprintf(message,sizeof(message),"Your %s timer has finished.",duration);
    int result=desk_notify_begin(&app.notify,app.window.server,DESK_NOTICE_SUCCESS,"Timer finished",message,srv_ticks());
    status(result?"Timer finished. Alert could not be sent.":"Timer finished.");layout();
    desk_log("deskclock: timer finished duration_us=%llu notification=%s\n",(unsigned long long)app.timer.duration,result?"unavailable":"pending");
}
static void update(uint64_t now) {
    app.now=now;
    if(desk_countdown_update(&app.timer,now)) completed();
    if(desk_stopwatch_update(&app.watch,now)) { status("Stopwatch stopped at 100 hours.");layout(); }
    if((app.timer.running || app.watch.running) && now/100000!=app.display_tick) { app.display_tick=now/100000;app.window.dirty=1; }
}
static void toggle(void) {
    if(timer_page()) {
        if(app.timer.running) { if(desk_countdown_pause(&app.timer,app.now)==2) { completed();return; }status("Timer paused."); }
        else {
            uint64_t duration;
            if(desk_duration_parse(app.duration_text.data,&duration)) { status("Use 1–86400s or h:mm:ss.");desk_log("deskclock: invalid duration\n");desk_ui_focus(&app.ui,CLOCK_DURATION);return; }
            if(duration!=app.timer.duration && desk_countdown_set(&app.timer,duration)) return;
            desk_countdown_start(&app.timer,app.now);status("Minimize to keep the timer running.");
        }
        desk_log("deskclock: timer running=%d remaining_us=%llu\n",app.timer.running,(unsigned long long)desk_countdown_value(&app.timer,app.now));
    } else {
        desk_stopwatch_toggle(&app.watch,app.now);status(app.watch.running?"Stopwatch running. Lap records a split.":"Stopwatch paused.");
        desk_log("deskclock: stopwatch running=%d elapsed_us=%llu\n",app.watch.running,(unsigned long long)desk_stopwatch_value(&app.watch,app.now));
    }
    layout();desk_ui_focus(&app.ui,CLOCK_TOGGLE);
}
static void lap(void) {
    if(!app.watch.running || app.lap_serial==UINT64_MAX) return;
    uint64_t value=desk_stopwatch_value(&app.watch,app.now);
    if(app.lap_count==CLOCK_LAP_LIMIT) { memmove(app.laps,app.laps+1,(CLOCK_LAP_LIMIT-1)*sizeof(*app.laps));app.lap_count--; }
    unsigned i=app.lap_count++;app.laps[i].id=++app.lap_serial;app.laps[i].total=value;app.laps[i].split=value-app.last_lap;app.last_lap=value;
    rows();desk_list_select(&app.table.body,app.lap_count-1);status("Lap recorded. The latest 64 laps are kept.");app.window.dirty=1;
    desk_log("deskclock: lap=%llu total_us=%llu split_us=%llu\n",(unsigned long long)app.lap_serial,(unsigned long long)value,(unsigned long long)app.laps[i].split);
}
static void reset(void) {
    if(timer_page()) { desk_countdown_reset(&app.timer);status("Timer reset."); }
    else { if(app.watch.running) return;desk_stopwatch_reset(&app.watch);app.lap_count=0;app.lap_serial=app.last_lap=0;rows();status("Stopwatch reset."); }
    layout();desk_log("deskclock: reset page=%s\n",timer_page()?"timer":"stopwatch");
}
static void copy_time(int laps) {
    if(app.clipboard.mode) return;
    size_t length=0;
    if(laps) {
        if(!app.lap_count) return;
        length=snprintf((char *)app.clipboard_data,sizeof(app.clipboard_data),"Lap\tElapsed\tSplit\n");
        for(unsigned i=0;i<app.lap_count;i++) length+=snprintf((char *)app.clipboard_data+length,sizeof(app.clipboard_data)-length,"%s\t%s\t%s\n",app.laps[i].number,app.laps[i].elapsed,app.laps[i].interval);
    } else {
        uint64_t value=timer_page()?desk_countdown_value(&app.timer,app.now):desk_stopwatch_value(&app.watch,app.now);
        if(timer_page() && value%1000000) value+=1000000-value%1000000;
        desk_duration_format((char *)app.clipboard_data,sizeof(app.clipboard_data),value,!timer_page());length=strlen((char *)app.clipboard_data);
    }
    if(desk_clipboard_copy(&app.clipboard,app.window.server,app.clipboard_data,length,srv_ticks())) { status("Copy did not start. Try again.");return; }
    app.clip_action=0;status(laps?"Copying laps…":"Copying time…");layout();desk_log("deskclock: copy kind=%s bytes=%zu\n",laps?"laps":"time",length);
}
static void edit_clipboard(unsigned flags) {
    unsigned operation=flags&(DESK_EDIT_COPY|DESK_EDIT_CUT|DESK_EDIT_PASTE);if(!operation || app.clipboard.mode) return;
    struct desk_text_buffer *t=&app.duration_text;size_t lo=t->caret<t->anchor?t->caret:t->anchor,hi=t->caret>t->anchor?t->caret:t->anchor;
    if(operation!=DESK_EDIT_PASTE && lo==hi) return;
    int result=operation==DESK_EDIT_PASTE?desk_clipboard_paste(&app.clipboard,app.window.server,srv_ticks()):desk_clipboard_copy(&app.clipboard,app.window.server,t->data+lo,hi-lo,srv_ticks());
    if(!result) { app.clip_action=operation;app.clip_revision=t->revision;app.clip_caret=t->caret;app.clip_anchor=t->anchor; }
}
static void clipboard_finish(int result) {
    unsigned operation=app.clip_action;app.clip_action=0;
    if(operation==CLOCK_CLIP_DISCARD) { layout();return; }
    if(!operation || operation==DESK_EDIT_COPY) { status(result>0?"Copied.":"Copy did not finish. Try again.");layout();return; }
    struct desk_text_buffer *t=&app.duration_text;
    if(result<0 || app.modal || !timer_page() || !app.duration.enabled || t->revision!=app.clip_revision || t->caret!=app.clip_caret || t->anchor!=app.clip_anchor) {
        status("Clipboard edit canceled. Try again.");layout();return;
    }
    size_t length=operation==DESK_EDIT_CUT?0:app.clipboard.length;
    if(operation==DESK_EDIT_PASTE && !length) return;
    if(memchr(app.clipboard_data,'\n',length) || memchr(app.clipboard_data,'\r',length) || desk_text_buffer_insert(t,operation==DESK_EDIT_CUT?"":(char *)app.clipboard_data,length)<0) status("Use a short, single-line duration.");
    else { desk_edit_reveal(&app.duration);app.window.dirty=1; }
    layout();
}
static void request_close(void) {
    if(!app.timer.running && !app.watch.running) { app.quit=1;return; }
    if(app.modal) return;
    struct desk_dialog_choice choices[]={{CLOCK_KEEP_OPEN,"Keep open",1},{CLOCK_CONFIRM_CLOSE,"Close",1}};
    if(desk_dialog_open(&app.dialog,"Close Clock?","Closing this window stops its timer and stopwatch. Minimize it to keep them running. Laps are not saved automatically.",choices,2,CLOCK_KEEP_OPEN,CLOCK_KEEP_OPEN)) return;
    app.modal=1;if(app.clip_action) app.clip_action=CLOCK_CLIP_DISCARD;layout();desk_log("deskclock: close confirmation\n");
}
static void dialog_action(struct desk_action a) {
    if(a.flags&DESK_WIDGET_REDRAW) app.window.dirty=1;
    if(!(a.flags&DESK_WIDGET_ACTIVATED)) return;
    if(a.id==CLOCK_CONFIRM_CLOSE) app.quit=1;
    else if(a.id==CLOCK_KEEP_OPEN) { app.modal=0;desk_dialog_focus(&app.dialog,0);desk_app_cancel_close(&app.window);layout();desk_log("deskclock: close canceled\n"); }
}
static void action(struct desk_action a) {
    if(a.flags&DESK_WIDGET_REDRAW) app.window.dirty=1;
    if(app.modal) return;
    if(a.id==CLOCK_TABS && (a.flags&DESK_WIDGET_CHANGED)) { page(desk_tabs_selected(&app.tabs));return; }
    if(a.id==CLOCK_DURATION) { edit_clipboard(a.flags);if(a.flags&DESK_EDIT_SUBMIT) toggle();return; }
    if(!(a.flags&DESK_WIDGET_ACTIVATED)) return;
    switch(a.id) {
    case CLOCK_TOGGLE: toggle();break;case CLOCK_RESET: reset();break;case CLOCK_LAP: lap();break;
    case CLOCK_COPY: copy_time(0);break;case CLOCK_COPY_LAPS: copy_time(1);break;
    case CLOCK_TIMER:case CLOCK_STOPWATCH:page(a.id);break;case CLOCK_CLOSE:request_close();break;
    }
}
static void key(const struct srv_key_event *k) {
    if(app.modal) { dialog_action(desk_dialog_key(&app.dialog,k));return; }
    if((k->flags&SRV_KEY_DOWN_EVENT) && !(k->flags&SRV_KEY_REPEAT) && (k->modifiers&(SRV_KEY_CTRL|SRV_KEY_META))) {
        unsigned id=0;
        if(k->usage==SRV_KEY_ENTER) id=CLOCK_TOGGLE;else if(k->usage==21) id=CLOCK_RESET;
        else if(k->usage==15) id=k->modifiers&SRV_KEY_SHIFT?CLOCK_COPY_LAPS:CLOCK_LAP;
        else if(k->usage==6 && (k->modifiers&SRV_KEY_SHIFT)) id=CLOCK_COPY;
        else if(k->usage==30) id=CLOCK_TIMER;else if(k->usage==31) id=CLOCK_STOPWATCH;else if(k->usage==26) id=CLOCK_CLOSE;
        if(id) { action((struct desk_action){id,DESK_WIDGET_ACTIVATED});return; }
    }
    action(desk_ui_key(&app.ui,k));
}
static void menus(void) {
    unsigned modal=app.modal?DESK_MENU_DISABLED:0;
    struct desk_menu_item items[]={
        {0,DESK_MENU_HEADING,"Clock",""},
        {CLOCK_TOGGLE,modal|(!app.toggle.enabled?DESK_MENU_DISABLED:0),"Start / pause","Ctrl-Enter"},
        {CLOCK_RESET,modal|(!app.reset.enabled?DESK_MENU_DISABLED:0),"Reset","Ctrl-R"},
        {CLOCK_LAP,modal|(!app.watch.running?DESK_MENU_DISABLED:0),"Record lap","Ctrl-L"},
        {0,DESK_MENU_SEPARATOR,"",""},
        {CLOCK_COPY,modal|(app.clipboard.mode?DESK_MENU_DISABLED:0),"Copy time","Ctrl-Shift-C"},
        {CLOCK_COPY_LAPS,modal|(!app.lap_count || app.clipboard.mode?DESK_MENU_DISABLED:0),"Copy laps","Ctrl-Shift-L"},
        {CLOCK_CLOSE,modal,"Close window","Ctrl-W"},
        {0,DESK_MENU_HEADING,"View",""},
        {CLOCK_TIMER,modal|(timer_page()?DESK_MENU_CHECKED:0),"Timer","Ctrl-1"},
        {CLOCK_STOPWATCH,modal|(!timer_page()?DESK_MENU_CHECKED:0),"Stopwatch","Ctrl-2"}
    };
    desk_app_menus(&app.window,items,sizeof(items)/sizeof(*items));desk_app_menu_tick(&app.window);
}
static void draw(struct desk_canvas *c) {
    const struct desk_theme *t=&desk_theme_quiet;desk_fill(c,(struct desk_rect){0,0,c->width,c->height},t->surface);desk_ui_draw(c,&app.ui,t);
    int timer=timer_page();uint64_t value=timer?desk_countdown_value(&app.timer,app.now):desk_stopwatch_value(&app.watch,app.now);char display[16],description[96];
    if(timer && value%1000000) value+=1000000-value%1000000;
    desk_duration_format(display,sizeof(display),value,!timer);int text_width=desk_text_width(&desk_font_digits_32,display);
    desk_label_draw(c,(struct desk_rect){(c->width-text_width)/2,52,text_width,44},&desk_font_digits_32,display,t->text);
    if(timer) {
        uint64_t remaining=desk_countdown_value(&app.timer,app.now);
        desk_progress_draw(c,(struct desk_rect){12,104,c->width-24,8},(app.timer.duration-remaining)/1000,app.timer.duration/1000,t);
        snprintf(description,sizeof(description),"%s",app.timer.finished?"Timer finished":app.timer.running?"Counting down":app.timer.remaining<app.timer.duration?"Paused":"Ready when you are");
        desk_label_draw(c,(struct desk_rect){12,118,c->width-24,24},t->body,description,app.timer.finished?t->accent:t->muted);
        desk_label_draw(c,(struct desk_rect){12,150,80,24},t->body,"Duration",t->muted);desk_notice_draw(c,&app.explanation,t);
    } else {
        snprintf(description,sizeof(description),"%s · %u lap%s",app.watch.running?"Running":app.watch.elapsed?"Paused":"Ready",app.lap_count,app.lap_count==1?"":"s");
        desk_label_draw(c,(struct desk_rect){12,102,c->width-24,24},t->body,description,t->muted);
        if(c->height<290) {
            if(app.lap_count) snprintf(description,sizeof(description),"Last lap: %s",app.laps[app.lap_count-1].interval);else strcpy(description,"Use Lap while the stopwatch is running.");
            desk_label_ellipsis(c,(struct desk_rect){12,174,c->width-24,24},t->body,description,t->muted);
        }
    }
    desk_label_ellipsis(c,(struct desk_rect){12,c->height-26,c->width-24,22},t->body,app.status,t->muted);
    if(app.modal) desk_dialog_draw(c,&app.dialog,t);
}
static void initialize(void) {
    desk_ui_init(&app.ui);desk_tabs_init(&app.tabs);struct desk_tab tabs[]={{CLOCK_TIMER,"Timer",0},{CLOCK_STOPWATCH,"Stopwatch",0}};desk_tabs_items(&app.tabs,tabs,2);
    desk_text_buffer_init(&app.duration_text,app.duration_storage[0],app.duration_storage[1],sizeof(app.duration_storage[0]));desk_text_buffer_set(&app.duration_text,"5:00",4);desk_edit_init(&app.duration,&app.duration_text,0);
    desk_table_init(&app.table);desk_dialog_init(&app.dialog);desk_notice_init(&app.explanation);desk_notify_init(&app.notify);desk_clipboard_init(&app.clipboard,app.clipboard_data);
    desk_notice_set(&app.explanation,"Keep time in the background","Minimize this window to keep its timer and stopwatch running. A completed timer appears in Alerts. Closing Clock stops both; there is no sound or saved alarm.",DESK_NOTICE_INFO);
    desk_countdown_set(&app.timer,300000000);
    desk_button_centered(&app.toggle,1);desk_button_centered(&app.reset,1);desk_button_centered(&app.lap,1);desk_button_centered(&app.copy,1);
    desk_ui_add(&app.ui,CLOCK_TABS,DESK_CONTROL_TABS,&app.tabs,NULL);desk_ui_add(&app.ui,CLOCK_DURATION,DESK_CONTROL_EDIT,&app.duration,NULL);
    desk_ui_add(&app.ui,CLOCK_TOGGLE,DESK_CONTROL_BUTTON,&app.toggle,"Start");desk_ui_add(&app.ui,CLOCK_RESET,DESK_CONTROL_BUTTON,&app.reset,"Reset");
    desk_ui_add(&app.ui,CLOCK_LAP,DESK_CONTROL_BUTTON,&app.lap,"Lap");desk_ui_add(&app.ui,CLOCK_COPY,DESK_CONTROL_BUTTON,&app.copy,"Copy");desk_ui_add(&app.ui,CLOCK_LAPS,DESK_CONTROL_TABLE,&app.table,NULL);
    status("Minimize to keep the timer running.");app.title_dirty=1;
}
int main(void) {
    initialize();if(desk_app_open(&app.window,"Clock",560,440)) return 1;desk_log("deskclock: starting\n");
    while(!app.quit && !app.window.failed && !app.window.closed) {
        long now=srv_monotonic_us();update(now<0?(uint64_t)srv_ticks()*10000:(uint64_t)now);
        if(desk_notify_tick(&app.notify,srv_ticks())<0) { status("Timer finished. Alert was not confirmed.");desk_log("deskclock: notification result=-1\n"); }
        if(desk_clipboard_tick(&app.clipboard,srv_ticks())<0) clipboard_finish(-1);
        for(unsigned batch=0;batch<32;batch++) {
            struct desk_packet p;int result=desk_app_poll(&app.window,&p);if(result<=0) break;if(result==2) continue;
            if(p.type==DESK_CONFIGURE) { layout();desk_ui_focus(&app.ui,CLOCK_DURATION); }
            else if(p.type==DESK_FOCUS) { if(app.modal) desk_dialog_focus(&app.dialog,p.value!=0);else desk_ui_window_focus(&app.ui,p.value!=0);app.window.dirty=1; }
            else if(p.type==DESK_MENU_ACTION) action((struct desk_action){p.value,DESK_WIDGET_ACTIVATED});
            else if(p.type==DESK_KEY) { struct srv_key_event k;memcpy(&k,p.text,sizeof(k));key(&k); }
            else if(p.type==DESK_POINTER) {
                struct desk_widget_event e={.type=p.value&DESK_POINTER_CANCEL?DESK_WIDGET_CANCEL:DESK_WIDGET_POINTER,.x=p.x,.y=p.y,.down=(p.value&DESK_POINTER_LEFT)!=0,.pressed=(p.value&DESK_POINTER_PRESS)!=0};
                if(app.modal) dialog_action(desk_dialog_pointer(&app.dialog,e,app.now));else action(desk_ui_pointer(&app.ui,e,app.now));
            } else if(p.type==DESK_SCROLL) { if(!app.modal) action(desk_ui_scroll(&app.ui,p.width,p.height,p.x,p.y)); }
            else if(p.type==DESK_NOTIFY_ACK) { int done=desk_notify_receive(&app.notify,&p);if(done) { desk_log("deskclock: notification result=%d\n",done);if(done<0) status("Timer finished. Alert was not accepted."); } }
            else if(p.type==DESK_CLIPBOARD_ACK || p.type==DESK_CLIPBOARD_DATA) { int done=desk_clipboard_receive(&app.clipboard,&p);if(done) clipboard_finish(done); }
            else if(p.type==DESK_CLOSE_REQUEST) request_close();
        }
        if(app.quit || app.window.closed || app.window.failed) break;
        if(app.title_dirty && !desk_app_title(&app.window,timer_page()?"Clock — Timer":"Clock — Stopwatch")) app.title_dirty=0;
        menus();struct desk_canvas c;if(desk_app_canvas(&app.window,&c)==1) { draw(&c);desk_app_present(&app.window); }
        srv_sleep_ticks(1);
    }
    desk_app_close(&app.window);desk_log("deskclock: exited failure=%d\n",app.window.failed);return app.window.failed?1:0;
}
