#include "content.h"
#include <srvros/desk_app.h>
#include <srvros/desk_preferences.h>
#include <srvros/desk_palette.h>
#include <srvros/desk_clipboard.h>
#include <srvros/desk_log.h>
#include <srvros/sys.h>
#include <stdio.h>
#include <string.h>
enum { HELP_BACK=1,HELP_FORWARD,HELP_TOPICS,HELP_OPEN,HELP_COPY,HELP_DOCUMENT,HELP_HOME,HELP_CLOSE,HELP_TOP,HELP_BOTTOM };
#define HELP_HISTORY 16
#define HELP_COPY_PAGE (1u<<30)
struct help_app {
    struct desk_app window;
    struct desk_ui ui;
    struct desk_button buttons[5];
    struct desk_document document;
    struct desk_palette topics;
    struct desk_clipboard_client clipboard;
    unsigned char clipboard_data[DESK_CLIPBOARD_BYTES+1];
    struct { unsigned topic;int offset; } history[HELP_HISTORY];
    unsigned count,index,clip_action;
    uint64_t clip_revision;
    size_t clip_caret,clip_anchor;
    const struct help_topic *page;
    char status[128];
    int chooser,quit,title_dirty,body_size;
};
static struct help_app app;
static void status(const char *message) { snprintf(app.status,sizeof(app.status),"%s",message);app.window.dirty=1; }
static void layout(void) {
    static const int small[]={32,32,70,48,50},large[]={64,80,88,96,96};
    static const char *labels[]={"Back","Forward","Topics","Open app","Copy page"};
    static const char *short_labels[]={"<",">","Topics","Open","Copy"};
    int x=12,compact=app.window.width<480;
    for(unsigned i=0;i<5;i++) {
        int width=compact?small[i]:large[i];
        int enabled=i==0?app.index>0:i==1?app.index+1<app.count:i==3?app.page && app.page->application:i==4?!app.clipboard.mode:1;
        desk_button_configure(&app.buttons[i],(struct desk_rect){x,12,width,28},enabled);x+=width+4;
        app.ui.controls[i].label=compact?short_labels[i]:labels[i];
    }
    desk_document_configure(&app.document,(struct desk_rect){12,52,app.window.width-24,app.window.height-92},app.body_size==20?&desk_font_20:&desk_font_16,1);
    desk_ui_layout_changed(&app.ui);desk_ui_window_focus(&app.ui,app.window.focused && !app.chooser);
    desk_palette_configure(&app.topics,(struct desk_rect){0,0,app.window.width,app.window.height});
    desk_palette_focus(&app.topics,app.window.focused && app.chooser);app.window.dirty=1;
}
static int page_set(unsigned id) {
    const struct help_topic *page=help_topic(id);
    if(!page || desk_document_set(&app.document,page->blocks,page->count)) return -1;
    app.page=page;app.title_dirty=1;
    status("Topics: Ctrl-L · Back / Forward: Alt-arrows · Copy page: Ctrl-C");
    desk_log("deskhelp: page=%s\n",page->alias);return 0;
}
static void visit(unsigned id) {
    if(!help_topic(id)) return;
    if(app.page && app.page->id==id) { desk_document_scroll(&app.document,-app.document.scroll.offset);app.window.dirty=1;return; }
    if(app.count) app.history[app.index].offset=app.document.scroll.offset;
    if(page_set(id)) return;
    if(app.count) {
        app.count=app.index+1;
        if(app.count==HELP_HISTORY) { memmove(app.history,app.history+1,(HELP_HISTORY-1)*sizeof(*app.history));app.count--; }
    }
    app.index=app.count++;app.history[app.index].topic=id;app.history[app.index].offset=0;
    layout();desk_ui_focus(&app.ui,HELP_DOCUMENT);
}
static void history(int direction) {
    if((direction<0 && !app.index) || (direction>0 && app.index+1>=app.count) || !app.count) return;
    app.history[app.index].offset=app.document.scroll.offset;
    unsigned next=direction<0?app.index-1:app.index+1;
    if(page_set(app.history[next].topic)) return;
    app.index=next;desk_document_scroll(&app.document,app.history[next].offset);
    layout();desk_ui_focus(&app.ui,HELP_DOCUMENT);
    desk_log("deskhelp: history topic=%s index=%u offset=%d\n",app.page->alias,app.index,app.document.scroll.offset);
}
static void chooser_close(void) {
    app.chooser=0;if(app.clip_action!=HELP_COPY_PAGE) app.clip_action=0;desk_palette_focus(&app.topics,0);layout();desk_ui_focus(&app.ui,HELP_DOCUMENT);
}
static void chooser_open(void) {
    if(app.chooser) return;
    struct desk_palette_item items[DESK_PALETTE_ITEMS];
    for(unsigned i=0;i<help_topic_count;i++) {
        items[i].id=help_topics[i].id;snprintf(items[i].label,sizeof(items[i].label),"%s",help_topics[i].title);
        snprintf(items[i].detail,sizeof(items[i].detail),"%s",help_topics[i].detail);
    }
    if(desk_palette_open(&app.topics,"Help topics",items,help_topic_count)) return;
    app.chooser=1;layout();desk_log("deskhelp: topics opened\n");
}
static void copy_page(void) {
    if(app.clipboard.mode) return;
    long length=desk_document_text(&app.document,(char *)app.clipboard_data,sizeof(app.clipboard_data));
    if(length<0 || desk_clipboard_copy(&app.clipboard,app.window.server,app.clipboard_data,length,srv_ticks())) { status("The page could not be copied. Try again.");return; }
    app.clip_action=HELP_COPY_PAGE;status("Copying page…");layout();
}
static void query_clipboard(unsigned flags) {
    unsigned operation=flags&(DESK_EDIT_COPY|DESK_EDIT_CUT|DESK_EDIT_PASTE);
    if(!operation || app.clipboard.mode) return;
    struct desk_text_buffer *text=&app.topics.text;
    size_t lo=text->caret<text->anchor?text->caret:text->anchor,hi=text->caret>text->anchor?text->caret:text->anchor;
    if(operation!=DESK_EDIT_PASTE && lo==hi) return;
    int result=operation==DESK_EDIT_PASTE?desk_clipboard_paste(&app.clipboard,app.window.server,srv_ticks()):
        desk_clipboard_copy(&app.clipboard,app.window.server,text->data+lo,hi-lo,srv_ticks());
    if(result) return;
    app.clip_action=operation;app.clip_revision=text->revision;app.clip_caret=text->caret;app.clip_anchor=text->anchor;
}
static void clipboard_finish(int result) {
    unsigned operation=app.clip_action;app.clip_action=0;
    if(operation==HELP_COPY_PAGE) { desk_log("deskhelp: copy-page result=%d bytes=%zu\n",result,app.clipboard.length);status(result>0?"Page copied.":"The page could not be copied. Try again.");layout();return; }
    if(!operation || !app.chooser || operation==DESK_EDIT_COPY) { layout();return; }
    struct desk_text_buffer *text=&app.topics.text;
    if(result<0 || text->revision!=app.clip_revision || text->caret!=app.clip_caret || text->anchor!=app.clip_anchor) {
        desk_palette_message(&app.topics,"Clipboard transfer canceled. Repeat Cut or Paste when ready.");app.window.dirty=1;return;
    }
    size_t length=operation==DESK_EDIT_CUT?0:app.clipboard.length;
    if(operation==DESK_EDIT_PASTE && !length) return;
    if(memchr(app.clipboard_data,'\n',length) || memchr(app.clipboard_data,'\r',length) ||
        desk_text_buffer_insert(text,operation==DESK_EDIT_CUT?"":(char *)app.clipboard_data,length)<0)
        desk_palette_message(&app.topics,"Use a short, single-line search.");
    else { desk_palette_filter(&app.topics);desk_edit_reveal(&app.topics.query); }
    app.window.dirty=1;
}
static void chooser_action(struct desk_palette_result result) {
    if(result.flags&DESK_WIDGET_REDRAW) app.window.dirty=1;
    query_clipboard(result.flags);
    if(result.cancel) chooser_close();
    else if(result.flags&DESK_WIDGET_ACTIVATED && help_topic(result.id)) { unsigned id=result.id;chooser_close();visit(id); }
}
static void action(struct desk_action a) {
    if(a.flags&DESK_WIDGET_REDRAW) app.window.dirty=1;
    if(a.id==HELP_DOCUMENT && (a.flags&DESK_WIDGET_CHANGED)) desk_log("deskhelp: scroll=%d\n",app.document.scroll.offset);
    if(!(a.flags&DESK_WIDGET_ACTIVATED) || app.chooser) return;
    switch(a.id) {
    case HELP_BACK: history(-1);break;
    case HELP_FORWARD: history(1);break;
    case HELP_TOPICS: chooser_open();break;
    case HELP_OPEN:
        if(app.page && app.page->application) status(desk_app_launch(&app.window,app.page->application,"")?"The application could not be opened. Try again.":"Opening application…");
        break;
    case HELP_COPY: copy_page();break;
    case HELP_HOME: visit(1);break;
    case HELP_CLOSE: app.quit=1;break;
    case HELP_TOP: desk_document_scroll(&app.document,-app.document.scroll.offset);app.window.dirty=1;break;
    case HELP_BOTTOM: desk_document_scroll(&app.document,app.document.scroll.content);app.window.dirty=1;break;
    }
}
static void key(const struct srv_key_event *k) {
    if(app.chooser) { chooser_action(desk_palette_key(&app.topics,k));return; }
    if(k->flags&SRV_KEY_DOWN_EVENT) {
        unsigned id=0;
        if(k->modifiers&(SRV_KEY_CTRL|SRV_KEY_META)) {
            if(k->usage==15) id=HELP_TOPICS;else if(k->usage==6) id=HELP_COPY;
            else if(k->usage==26) id=HELP_CLOSE;else if(k->usage==SRV_KEY_HOME) id=HELP_HOME;
        } else if(k->modifiers&SRV_KEY_ALT) {
            if(k->usage==SRV_KEY_LEFT) id=HELP_BACK;else if(k->usage==SRV_KEY_RIGHT) id=HELP_FORWARD;
        }
        if(id) { action((struct desk_action){id,DESK_WIDGET_ACTIVATED});return; }
    }
    action(desk_ui_key(&app.ui,k));
}
static void menus(void) {
    unsigned modal=app.chooser?DESK_MENU_DISABLED:0;
    struct desk_menu_item items[]={
        {0,DESK_MENU_HEADING,"File",""},
        {HELP_TOPICS,modal,"Topics…","Ctrl-L"},
        {HELP_OPEN,modal|(!app.page || !app.page->application?DESK_MENU_DISABLED:0),"Open related application",""},
        {HELP_COPY,modal|(app.clipboard.mode?DESK_MENU_DISABLED:0),"Copy page","Ctrl-C"},
        {HELP_CLOSE,modal,"Close window","Ctrl-W"},
        {0,DESK_MENU_HEADING,"View",""},
        {HELP_HOME,modal,"Welcome","Ctrl-Home"},
        {HELP_BACK,modal|(!app.index?DESK_MENU_DISABLED:0),"Back","Alt-Left"},
        {HELP_FORWARD,modal|(app.index+1>=app.count?DESK_MENU_DISABLED:0),"Forward","Alt-Right"},
        {HELP_TOP,modal,"Top of page","Home"},{HELP_BOTTOM,modal,"Bottom of page","End"}
    };
    desk_app_menus(&app.window,items,sizeof(items)/sizeof(*items));desk_app_menu_tick(&app.window);
}
static void draw(struct desk_canvas *c) {
    desk_fill(c,(struct desk_rect){0,0,c->width,c->height},desk_theme_quiet.surface);desk_ui_draw(c,&app.ui,&desk_theme_quiet);
    const char *message=app.status;
    if(c->width<480 && !strncmp(message,"Topics:",7)) message="Ctrl-L: topics · Alt-Left: back";
    desk_label_draw(c,(struct desk_rect){12,c->height-30,c->width-24,24},desk_theme_quiet.body,message,desk_theme_quiet.muted);
    if(app.chooser) desk_palette_draw(c,&app.topics,&desk_theme_quiet);
}
static void initialize(void) {
    desk_ui_init(&app.ui);desk_document_init(&app.document);desk_palette_init(&app.topics);desk_clipboard_init(&app.clipboard,app.clipboard_data);
    for(unsigned i=0;i<5;i++) { desk_button_centered(&app.buttons[i],1);desk_ui_add(&app.ui,i+1,DESK_CONTROL_BUTTON,&app.buttons[i],""); }
    desk_ui_add(&app.ui,HELP_DOCUMENT,DESK_CONTROL_DOCUMENT,&app.document,NULL);
}
int main(int argc,char **argv) {
    initialize();
    if(desk_app_open(&app.window,"Help",760,540)) return 1;
    visit(help_topic_alias(argc>2?argv[2]:NULL)->id);
    desk_log("deskhelp: starting\n");
    while(!app.quit && !app.window.failed && !app.window.closed) {
        if(desk_clipboard_tick(&app.clipboard,srv_ticks())<0) clipboard_finish(-1);
        for(unsigned batch=0;batch<32;batch++) {
            struct desk_packet p;int result=desk_app_poll(&app.window,&p);if(result<=0) break;if(result==2) continue;
            if(p.type==DESK_CONFIGURE) { layout();desk_ui_focus(&app.ui,HELP_DOCUMENT); }
            else if(p.type==DESK_PREFERENCES) {
                struct desk_preferences preferences;memcpy(&preferences,p.text,sizeof(preferences));
                if(desk_preferences_valid(&preferences) && app.body_size!=(int)preferences.text_size) { app.body_size=preferences.text_size;layout(); }
            } else if(p.type==DESK_FOCUS) { if(app.chooser) desk_palette_focus(&app.topics,p.value!=0);else desk_ui_window_focus(&app.ui,p.value!=0);app.window.dirty=1; }
            else if(p.type==DESK_MENU_ACTION) action((struct desk_action){p.value,DESK_WIDGET_ACTIVATED});
            else if(p.type==DESK_KEY) { struct srv_key_event k;memcpy(&k,p.text,sizeof(k));key(&k); }
            else if(p.type==DESK_POINTER) {
                struct desk_widget_event e={.type=p.value&DESK_POINTER_CANCEL?DESK_WIDGET_CANCEL:DESK_WIDGET_POINTER,.x=p.x,.y=p.y,.down=(p.value&DESK_POINTER_LEFT)!=0,.pressed=(p.value&DESK_POINTER_PRESS)!=0};
                if(app.chooser) chooser_action(desk_palette_pointer(&app.topics,e,srv_monotonic_us()));else action(desk_ui_pointer(&app.ui,e,srv_monotonic_us()));
            } else if(p.type==DESK_SCROLL) {
                if(app.chooser) chooser_action(desk_palette_scroll(&app.topics,p.width,p.height,p.y));else action(desk_ui_scroll(&app.ui,p.width,p.height,p.x,p.y));
            } else if(p.type==DESK_CLIPBOARD_ACK || p.type==DESK_CLIPBOARD_DATA) {
                int done=desk_clipboard_receive(&app.clipboard,&p);if(done) clipboard_finish(done);
            } else if(p.type==DESK_CLOSE_REQUEST) app.quit=1;
        }
        if(app.quit || app.window.closed || app.window.failed) break;
        if(app.title_dirty) { char title[88];snprintf(title,sizeof(title),"Help — %s",app.page->title);if(!desk_app_title(&app.window,title)) app.title_dirty=0; }
        menus();struct desk_canvas canvas;if(desk_app_canvas(&app.window,&canvas)==1) { draw(&canvas);desk_app_present(&app.window); }
        srv_sleep_ticks(1);
    }
    desk_app_close(&app.window);desk_log("deskhelp: exited failure=%d\n",app.window.failed);return app.window.failed?1:0;
}
