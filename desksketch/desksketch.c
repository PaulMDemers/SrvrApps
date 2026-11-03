#include <srvros/desk_paths.h>
#include <srvros/desk_app.h>
#include <srvros/desk_ui.h>
#include <srvros/desk_color_panel.h>
#include <srvros/desk_dialog.h>
#include <srvros/desk_file_picker.h>
#include <srvros/desk_listing.h>
#include <srvros/desk_file_job.h>
#include <srvros/desk_image_job.h>
#include <srvros/desk_capture_job.h>
#include <srvros/desk_clipboard.h>
#include <srvros/desk_log.h>
#include <srvros/sys.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
enum { SK_NEW=1,SK_OPEN,SK_SAVE,SK_COLOR,SK_UNDO,SK_REDO,SK_FIT,SK_ACTUAL,SK_OUT,SK_IN,SK_CLOSE,SK_CLEAR,SK_STOP,
    SK_TOOLS=20,SK_SIZE,SK_CANVAS,SK_PEN=30,SK_LINE,SK_BOX,SK_PAN,SK_KEEP=50,SK_DISCARD,SK_SAVE_FIRST };
struct sketch_app {
    struct desk_app window;
    struct desk_theme theme;
    struct desk_ui ui;
    struct desk_button buttons[7];
    struct desk_tabs tools;
    struct desk_slider size;
    struct desk_paint paint;
    struct desk_bitmap bitmap;
    void *arena;size_t arena_bytes;
    struct desk_color_panel colors;
    struct desk_dialog dialog;
    struct desk_image_job image_job;
    struct desk_capture_job save_job;
    struct desk_file_job file_job;
    struct desk_file_picker picker;
    struct desk_file_entry entries[DESK_FILE_ENTRIES];
    struct desk_list_item items[DESK_FILE_ENTRIES];
    char details[DESK_FILE_ENTRIES][32];
    struct desk_clipboard_client clipboard;
    unsigned char clipboard_data[DESK_CLIPBOARD_BYTES+1];
    uint64_t clip_revision,save_revision;
    size_t clip_caret,clip_anchor;
    unsigned clip_action,clip_field;
    char path[160],pending_path[160],status[160];
    unsigned picker_mode,dialog_operation,after_save;
    int file_ready,color_open,quit,started,title_dirty,last_dirty;
};
static struct sketch_app app;
static void layout(void);
static void picker_open(unsigned mode);
static void request(unsigned operation);
static void status(const char *message) { snprintf(app.status,sizeof(app.status),"%s",message);app.window.dirty=1; }
static int modal(void) { return app.picker_mode || app.color_open || app.dialog_operation; }
static int file_busy(void) { return app.image_job.busy || app.image_job.retiring_pid || app.save_job.busy || app.save_job.retiring_pid || app.file_job.busy; }
static void focus_changed(void) {
    desk_ui_window_focus(&app.ui,app.window.focused && !modal());
    desk_picker_focus(&app.picker,app.window.focused && app.picker_mode);
    desk_color_panel_focus(&app.colors,app.window.focused && app.color_open);
    desk_dialog_focus(&app.dialog,app.window.focused && app.dialog_operation);app.window.dirty=1;
}
static void layout(void) {
    int w=app.window.width,h=app.window.height,x=12;static const int widths[]={60,60,92,72,60,60,52};
    for(unsigned i=0;i<7;i++) {
        int visible=w>=600 || i<4,width=w>=600?widths[i]:(w-36)/4;
        int enabled=i<3?!file_busy():i==4?!app.bitmap.active && app.bitmap.index>0:i==5?!app.bitmap.active && app.bitmap.index+1<app.bitmap.count:app.arena!=NULL;
        desk_button_configure(&app.buttons[i],visible?(struct desk_rect){x,12,width,28}:(struct desk_rect){0},enabled && !app.image_job.busy);
        if(visible) x+=width+(w>=600?8:4);
    }
    app.ui.controls[2].label=w<600?"Save":"Save copy";
    desk_tabs_configure(&app.tools,(struct desk_rect){12,48,w-24,28},!app.image_job.busy);
    desk_slider_configure(&app.size,(struct desk_rect){84,84,w<360?w-96:240,28},!app.image_job.busy);
    desk_paint_configure(&app.paint,(struct desk_rect){12,124,w-24,h-174},!app.image_job.busy);
    desk_picker_configure(&app.picker,(struct desk_rect){0,0,w,h});desk_color_panel_configure(&app.colors,(struct desk_rect){0,0,w,h});desk_dialog_configure(&app.dialog,(struct desk_rect){0,0,w,h});
    desk_ui_layout_changed(&app.ui);focus_changed();app.window.dirty=1;
}
static int document(uint32_t width,uint32_t height,const uint32_t *source) {
    if(!desk_capture_bytes(width,height)) return -1;
    size_t bytes=desk_bitmap_storage_bytes(width,height,5);if(!bytes) return -1;
    void *arena=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(arena==MAP_FAILED) return -1;
    struct desk_bitmap bitmap;
    if(desk_bitmap_init(&bitmap,arena,bytes,width,height,5,source)) { munmap(arena,bytes);return -1; }
    desk_paint_event(&app.paint,(struct desk_widget_event){.type=DESK_WIDGET_CANCEL});
    if(app.arena) munmap(app.arena,app.arena_bytes);
    app.bitmap=bitmap;app.arena=arena;app.arena_bytes=bytes;desk_paint_bind(&app.paint,&app.bitmap);app.title_dirty=1;return 0;
}
static void execute(unsigned operation) {
    if(operation==SK_CLOSE) { app.quit=1;return; }
    if(operation==SK_NEW) {
        if(document(640,360,NULL)) status("There is not enough memory for a new drawing.");
        else { app.path[0]=0;status("Draw with Pen, Line or Box. Pan moves a zoomed canvas.");layout();desk_ui_focus(&app.ui,SK_CANVAS); }
    } else if(operation==SK_OPEN) picker_open(DESK_PICKER_OPEN);
}
static void request(unsigned operation) {
    if(app.save_job.busy) {
        if(operation==SK_CLOSE) { app.after_save=SK_CLOSE;status("Finishing the saved copy before closing…"); }
        return;
    }
    if(operation==SK_CLOSE && modal()) {
        app.picker_mode=app.color_open=app.dialog_operation=app.after_save=app.clip_action=0;desk_app_cancel_close(&app.window);layout();return;
    }
    if(modal()) return;
    if(operation==SK_CLOSE && app.file_job.busy) { app.after_save=SK_CLOSE;status("Finishing the folder read before closing…");return; }
    if(app.image_job.busy) { if(operation!=SK_CLOSE) return;desk_image_job_cancel(&app.image_job); }
    if(operation!=SK_CLOSE && file_busy()) return;
    desk_paint_event(&app.paint,(struct desk_widget_event){.type=DESK_WIDGET_CANCEL});
    if(!desk_bitmap_dirty(&app.bitmap)) { execute(operation);return; }
    struct desk_dialog_choice choices[]={{SK_KEEP,"Keep",1},{SK_DISCARD,"Discard",1},{SK_SAVE_FIRST,"Save",1}};
    if(desk_dialog_open(&app.dialog,"Unsaved drawing","Save a BMP copy, discard these changes, or keep working.",choices,3,SK_KEEP,SK_KEEP)) return;
    app.dialog_operation=operation;layout();desk_log("desksketch: confirm operation=%u\n",operation);
}
static void dialog_action(struct desk_action a) {
    if(a.flags&DESK_WIDGET_REDRAW) app.window.dirty=1;
    if(!(a.flags&DESK_WIDGET_ACTIVATED)) return;
    unsigned operation=app.dialog_operation;app.dialog_operation=0;
    if(a.id==SK_KEEP) { app.after_save=0;desk_app_cancel_close(&app.window);layout(); }
    else if(a.id==SK_DISCARD) { layout();execute(operation); }
    else if(a.id==SK_SAVE_FIRST) { app.after_save=operation;layout();picker_open(DESK_PICKER_SAVE); }
}
static void color_action(struct desk_action a) {
    if(a.flags&DESK_WIDGET_REDRAW) app.window.dirty=1;
    if(!(a.flags&DESK_WIDGET_ACTIVATED)) return;
    if(a.id==DESK_COLOR_COPY) {
        if(!app.clipboard.mode && !desk_clipboard_copy(&app.clipboard,app.window.server,app.colors.hex,7,srv_ticks())) app.clip_action=0;
    } else if(a.id==DESK_COLOR_ACCEPT || a.id==DESK_COLOR_CANCEL) {
        if(a.id==DESK_COLOR_ACCEPT) { desk_paint_tool(&app.paint,app.paint.tool,app.colors.color,app.paint.size);desk_log("desksketch: color=%06x\n",app.paint.color); }
        app.color_open=0;layout();desk_ui_focus(&app.ui,SK_CANVAS);
    }
}
static void load_image(const char *path) {
    if(desk_image_job_start(&app.image_job,path,srv_ticks())) { status("The image worker could not start. Try again.");return; }
    strcpy(app.pending_path,path);status("Opening image… Escape cancels.");layout();desk_log("desksketch: loading path=%s\n",path);
}
static void image_finish(int result) {
    if(result<0 || app.image_job.response.status) status("The image could not be opened. The current drawing was kept.");
    else {
        uint32_t *pixels=desk_image_job_take(&app.image_job);
        unsigned w=app.image_job.response.width,h=app.image_job.response.height;
        if(!pixels || document(w,h,pixels)) status("Image is too large or memory is unavailable. Limit: an 8 MiB BMP.");
        else { strcpy(app.path,app.pending_path);status("Image opened. Save copy writes a new BMP; the original is kept.");desk_log("desksketch: loaded path=%s width=%u height=%u\n",app.path,w,h); }
        desk_image_pixels_release(pixels,w,h);
    }
    layout();desk_ui_focus(&app.ui,SK_CANVAS);
}
static int bmp_path(const char *path) {
    size_t n=strlen(path);return n>4 && path[n-4]=='.' && (path[n-3]=='b' || path[n-3]=='B') && (path[n-2]=='m' || path[n-2]=='M') && (path[n-1]=='p' || path[n-1]=='P');
}
static void save_image(const char *path) {
    app.save_revision=desk_bitmap_revision(&app.bitmap);
    if(desk_capture_job_start(&app.save_job,app.bitmap.pixels,app.bitmap.width,app.bitmap.height,path)) {
        app.after_save=0;desk_app_cancel_close(&app.window);status("The save worker could not start. Try again.");
    } else { strcpy(app.pending_path,path);status("Saving a verified BMP copy… You can keep drawing.");desk_log("desksketch: saving revision=%llu path=%s\n",(unsigned long long)app.save_revision,path); }
    layout();desk_ui_focus(&app.ui,SK_CANVAS);
}
static void save_finish(int result) {
    int success=result>0 && !app.save_job.response.status;unsigned after=app.after_save;app.after_save=0;
    if(success) {
        app.bitmap.saved_revision=app.save_revision;strcpy(app.path,app.pending_path);app.title_dirty=1;
        status(desk_bitmap_dirty(&app.bitmap)?"Copy saved. Newer drawing changes are still unsaved.":"BMP copy saved and verified.");
    } else {
        status(result>0 && app.save_job.response.status==DESK_CAPTURE_EXISTS?"That file already exists. Save copy with a different name.":"The copy could not be verified. Your drawing remains in memory.");desk_app_cancel_close(&app.window);
    }
    desk_log("desksketch: saved success=%d status=%u revision=%llu current=%llu transport=%d\n",success,app.save_job.response.status,(unsigned long long)app.save_revision,(unsigned long long)desk_bitmap_revision(&app.bitmap),result);
    layout();if(success && after) app.after_save=after;
}
static void browse(void) {
    if(!app.file_ready || desk_file_job_start(&app.file_job,DESK_FILE_LIST,app.picker.target,NULL,0,0,0)) desk_picker_error(&app.picker,"The folder could not be opened. Check the path.");
    else desk_picker_loading(&app.picker);
    desk_picker_focus(&app.picker,app.window.focused);app.window.dirty=1;
}
static void picker_close(void) { app.picker_mode=app.clip_action=0;desk_picker_focus(&app.picker,0);layout();desk_ui_focus(&app.ui,SK_CANVAS); }
static void picker_open(unsigned mode) {
    if(file_busy() || modal()) return;
    if(app.file_ready && app.file_job.broken) { desk_file_job_close(&app.file_job);app.file_ready=0; }
    if(!app.file_ready) app.file_ready=!desk_file_job_init(&app.file_job);
    if(!app.file_ready) { app.after_save=0;desk_app_cancel_close(&app.window);status("The folder worker could not start. Try again.");return; }
    char directory[160],name[96]="";snprintf(directory,sizeof(directory),"%s",desk_home());
    if(app.path[0]) { strcpy(directory,app.path);char *slash=strrchr(directory,'/');if(slash==directory) slash[1]=0;else if(slash) *slash=0; }
    if(mode==DESK_PICKER_SAVE) snprintf(name,sizeof(name),"Drawing-%llu-%llu.bmp",(unsigned long long)srv_getpid(),(unsigned long long)srv_ticks());
    if(desk_picker_open(&app.picker,mode,directory,name)) return;
    app.picker_mode=mode;layout();browse();desk_log("desksketch: picker opened mode=%u\n",mode);
}
static struct desk_edit *clipboard_target(void) {
    if(!app.picker_mode) return NULL;
    return app.clip_field==DESK_PICKER_LOCATION?&app.picker.location:app.clip_field==DESK_PICKER_FILENAME?&app.picker.name:NULL;
}
static void clipboard_action(struct desk_picker_result r) {
    unsigned operation=r.flags&(DESK_EDIT_COPY|DESK_EDIT_CUT|DESK_EDIT_PASTE);if(!operation || app.clipboard.mode) return;
    app.clip_field=r.field;struct desk_edit *edit=clipboard_target();if(!edit) return;
    struct desk_text_buffer *t=edit->text;size_t lo=t->caret<t->anchor?t->caret:t->anchor,hi=t->caret>t->anchor?t->caret:t->anchor;
    if(operation!=DESK_EDIT_PASTE && lo==hi) return;
    int result=operation==DESK_EDIT_PASTE?desk_clipboard_paste(&app.clipboard,app.window.server,srv_ticks()):
        desk_clipboard_copy(&app.clipboard,app.window.server,t->data+lo,hi-lo,srv_ticks());
    if(result) return;
    app.clip_action=operation;app.clip_revision=t->revision;app.clip_caret=t->caret;app.clip_anchor=t->anchor;
}
static void clipboard_finish(int result) {
    unsigned action=app.clip_action;app.clip_action=0;
    if(!app.picker_mode || !action || action==DESK_EDIT_COPY) return;
    struct desk_edit *edit=clipboard_target();if(!edit) return;struct desk_text_buffer *t=edit->text;
    if(result<0 || !edit->enabled || t->revision!=app.clip_revision || t->caret!=app.clip_caret || t->anchor!=app.clip_anchor) {
        desk_picker_message(&app.picker,"Clipboard transfer canceled. Repeat Cut or Paste when ready.");app.window.dirty=1;return;
    }
    size_t length=action==DESK_EDIT_CUT?0:app.clipboard.length;
    if(action==DESK_EDIT_PASTE && !length) return;
    if(memchr(app.clipboard_data,'\n',length) || memchr(app.clipboard_data,'\r',length) ||
        desk_text_buffer_insert(t,action==DESK_EDIT_CUT?"":(char *)app.clipboard_data,length)<0)
        desk_picker_message(&app.picker,"Use a single line that fits this field.");
    else desk_edit_reveal(edit);
    app.window.dirty=1;
}
static void picker_action(struct desk_picker_result r) {
    if(r.flags&DESK_WIDGET_REDRAW) app.window.dirty=1;
    clipboard_action(r);
    if(r.operation==DESK_PICKER_CANCEL) { picker_close();app.after_save=0;desk_app_cancel_close(&app.window);status("Canceled. The drawing was kept."); }
    else if(r.operation==DESK_PICKER_BROWSE) browse();
    else if(r.operation==DESK_PICKER_ACCEPT) {
        char path[160];strcpy(path,app.picker.target);unsigned mode=app.picker_mode;
        if(mode==DESK_PICKER_SAVE && !bmp_path(path)) { desk_picker_message(&app.picker,"Choose a filename ending in .bmp. Existing files are kept.");app.window.dirty=1;return; }
        picker_close();if(mode==DESK_PICKER_SAVE) save_image(path);else load_image(path);
    }
}
static void file_finish(int result) {
    if(!app.picker_mode) { layout();return; }
    int count=-1;
    if(result>0 && !app.file_job.response.status) count=desk_listing_decode(app.entries,app.items,app.details,app.file_job.incoming,app.file_job.response.length,app.file_job.response.count);
    if(count<0 || desk_picker_listing(&app.picker,app.file_job.request.path,app.items,count)) desk_picker_error(&app.picker,"Could not read this folder. Check the path and volume.");
    else desk_log("desksketch: picker ready mode=%u items=%d\n",app.picker_mode,count);
    desk_picker_focus(&app.picker,app.window.focused);app.window.dirty=1;
}
static void action(struct desk_action a) {
    if(a.flags&DESK_WIDGET_REDRAW) app.window.dirty=1;
    if(modal()) return;
    if(a.id==SK_CANVAS) {
        if(a.flags&DESK_WIDGET_ACTIVATED) { layout();desk_log("desksketch: stroke revision=%llu tool=%u\n",(unsigned long long)desk_bitmap_revision(&app.bitmap),app.paint.tool); }
        return;
    }
    if(a.id==SK_SIZE && (a.flags&DESK_WIDGET_CHANGED)) desk_paint_tool(&app.paint,app.paint.tool,app.paint.color,app.size.value);
    if(a.id==SK_TOOLS && (a.flags&DESK_WIDGET_CHANGED)) desk_paint_tool(&app.paint,desk_tabs_selected(&app.tools)-1,app.paint.color,app.paint.size);
    if(!(a.flags&DESK_WIDGET_ACTIVATED)) return;
    if(a.id==SK_NEW || a.id==SK_OPEN || a.id==SK_CLOSE) { request(a.id);return; }
    if(a.id==SK_SAVE) { if(!file_busy()) picker_open(DESK_PICKER_SAVE);return; }
    if(a.id==SK_COLOR) { if(app.image_job.busy) return;desk_color_panel_open(&app.colors,app.paint.color);app.color_open=1;layout();return; }
    if(app.image_job.busy) {
        if(a.id==SK_STOP) { desk_image_job_cancel(&app.image_job);status("Opening canceled. The drawing was kept.");layout(); }return;
    }
    if(a.id==SK_UNDO) desk_bitmap_undo(&app.bitmap);
    else if(a.id==SK_REDO) desk_bitmap_redo(&app.bitmap);
    else if(a.id==SK_CLEAR) desk_bitmap_clear(&app.bitmap,0xffffff);
    else if(a.id==SK_FIT) desk_image_fit(&app.paint.view);
    else if(a.id==SK_ACTUAL) desk_image_actual(&app.paint.view);
    else if(a.id==SK_IN) desk_image_zoom(&app.paint.view,1);
    else if(a.id==SK_OUT) desk_image_zoom(&app.paint.view,-1);
    else if(a.id>=SK_PEN && a.id<=SK_PAN) { desk_paint_tool(&app.paint,a.id-SK_PEN,app.paint.color,app.paint.size);desk_tabs_select(&app.tools,a.id-SK_PEN+1); }
    layout();desk_ui_focus(&app.ui,SK_CANVAS);desk_log("desksketch: action=%u revision=%llu\n",a.id,(unsigned long long)desk_bitmap_revision(&app.bitmap));
}
static void key(const struct srv_key_event *k) {
    if(app.dialog_operation) { dialog_action(desk_dialog_key(&app.dialog,k));return; }
    if(app.color_open) { color_action(desk_color_panel_key(&app.colors,k));return; }
    if(app.picker_mode) { picker_action(desk_picker_key(&app.picker,k));return; }
    unsigned id=0;
    if((k->flags&SRV_KEY_DOWN_EVENT) && !(k->flags&SRV_KEY_REPEAT)) {
        if((k->modifiers&(SRV_KEY_CTRL|SRV_KEY_META)) && !(k->modifiers&SRV_KEY_ALT)) {
            if(k->usage==17) id=SK_NEW;else if(k->usage==18) id=SK_OPEN;else if(k->usage==22) id=SK_SAVE;else if(k->usage==26) id=SK_CLOSE;
            else if(k->usage==29) id=k->modifiers&SRV_KEY_SHIFT?SK_REDO:SK_UNDO;else if(k->usage==14) id=SK_COLOR;
        } else if(!(k->modifiers&(SRV_KEY_CTRL|SRV_KEY_ALT|SRV_KEY_META))) {
            if(k->usage==SRV_KEY_ESCAPE && app.image_job.busy) id=SK_STOP;
            else if(k->codepoint=='b') id=SK_PEN;else if(k->codepoint=='l') id=SK_LINE;else if(k->codepoint=='r') id=SK_BOX;else if(k->codepoint=='h') id=SK_PAN;
        }
    }
    if(id) action((struct desk_action){id,DESK_WIDGET_ACTIVATED});else action(desk_ui_key(&app.ui,k));
}
static void menus(void) {
    unsigned blocked=modal()?DESK_MENU_DISABLED:0,busy=blocked|(file_busy()?DESK_MENU_DISABLED:0);
    unsigned edits=blocked|(app.bitmap.active || app.image_job.busy?DESK_MENU_DISABLED:0);
    struct desk_menu_item items[]={
        {0,DESK_MENU_HEADING,"File",""},{SK_NEW,busy,"New drawing","Ctrl-N"},{SK_OPEN,busy,"Open image…","Ctrl-O"},{SK_SAVE,busy,"Save BMP copy…","Ctrl-S"},
        {SK_STOP,blocked|(!app.image_job.busy?DESK_MENU_DISABLED:0),"Cancel opening","Esc"},{SK_CLOSE,blocked,"Close window","Ctrl-W"},
        {0,DESK_MENU_HEADING,"Edit",""},{SK_UNDO,edits|(!app.bitmap.index?DESK_MENU_DISABLED:0),"Undo","Ctrl-Z"},
        {SK_REDO,edits|(app.bitmap.index+1>=app.bitmap.count?DESK_MENU_DISABLED:0),"Redo","Ctrl-Shift-Z"},{SK_CLEAR,edits,"Clear canvas",""},{SK_COLOR,edits,"Choose color…","Ctrl-K"},
        {0,DESK_MENU_HEADING,"Tools",""},{SK_PEN,edits|(app.paint.tool==DESK_PAINT_PEN?DESK_MENU_CHECKED:0),"Pen","B"},
        {SK_LINE,edits|(app.paint.tool==DESK_PAINT_LINE?DESK_MENU_CHECKED:0),"Line","L"},{SK_BOX,edits|(app.paint.tool==DESK_PAINT_BOX?DESK_MENU_CHECKED:0),"Box","R"},
        {SK_PAN,edits|(app.paint.tool==DESK_PAINT_PAN?DESK_MENU_CHECKED:0),"Pan","H"},
        {0,DESK_MENU_HEADING,"View",""},{SK_FIT,edits|(app.paint.view.fit?DESK_MENU_CHECKED:0),"Fit in window","0"},
        {SK_ACTUAL,edits|(!app.paint.view.fit && app.paint.view.scale==65536?DESK_MENU_CHECKED:0),"Actual size","1"},
        {SK_IN,edits|(app.paint.view.scale>=262144?DESK_MENU_DISABLED:0),"Zoom in","+"},{SK_OUT,edits|(app.paint.view.scale<=4096?DESK_MENU_DISABLED:0),"Zoom out","−"}
    };
    desk_app_menus(&app.window,items,sizeof(items)/sizeof(*items));desk_app_menu_tick(&app.window);
}
static void draw(struct desk_canvas *c) {
    struct desk_theme *t=&app.theme;desk_fill(c,(struct desk_rect){0,0,c->width,c->height},t->surface);desk_ui_draw(c,&app.ui,t);
    char brush[24],detail[120];snprintf(brush,sizeof(brush),"Size %u",app.paint.size);
    desk_label_draw(c,(struct desk_rect){12,86,68,24},&desk_font_16,brush,t->text);
    if(c->width>=400) { desk_fill(c,(struct desk_rect){c->width-64,86,48,22},t->border);desk_fill(c,(struct desk_rect){c->width-62,88,44,18},app.paint.color); }
    snprintf(detail,sizeof(detail),"%u × %u · %u%% · %s",app.bitmap.width,app.bitmap.height,(app.paint.view.scale*100u+32768)/65536,desk_bitmap_dirty(&app.bitmap)?"Edited":app.path[0]?"Unchanged":"New drawing");
    desk_label_ellipsis(c,(struct desk_rect){12,c->height-44,c->width-24,22},&desk_font_16,detail,t->text);
    desk_label_ellipsis(c,(struct desk_rect){12,c->height-22,c->width-24,22},&desk_font_16,app.status,t->muted);
    if(app.picker_mode) desk_picker_draw(c,&app.picker,t);
    if(app.color_open) desk_color_panel_draw(c,&app.colors,t);
    if(app.dialog_operation) desk_dialog_draw(c,&app.dialog,t);
}
static void initialize(void) {
    app.theme=desk_theme_quiet;desk_ui_init(&app.ui);desk_paint_init(&app.paint);desk_slider_init(&app.size);desk_slider_range(&app.size,1,32,1);desk_slider_set(&app.size,3);
    desk_tabs_init(&app.tools);struct desk_tab tabs[]={{1,"Pen",0},{2,"Line",0},{3,"Box",0},{4,"Pan",0}};desk_tabs_items(&app.tools,tabs,4);
    static const char *labels[]={"New","Open","Save copy","Color","Undo","Redo","Fit"};
    for(unsigned i=0;i<7;i++) { desk_button_centered(&app.buttons[i],1);desk_ui_add(&app.ui,i+1,DESK_CONTROL_BUTTON,&app.buttons[i],labels[i]); }
    desk_ui_add(&app.ui,SK_TOOLS,DESK_CONTROL_TABS,&app.tools,NULL);desk_ui_add(&app.ui,SK_SIZE,DESK_CONTROL_SLIDER,&app.size,NULL);desk_ui_add(&app.ui,SK_CANVAS,DESK_CONTROL_PAINT,&app.paint,NULL);
    desk_color_panel_init(&app.colors);desk_dialog_init(&app.dialog);desk_picker_init(&app.picker);desk_image_job_init(&app.image_job);desk_capture_job_init(&app.save_job);desk_clipboard_init(&app.clipboard,app.clipboard_data);
}
int main(int argc,char **argv) {
    initialize();if(document(640,360,NULL)) return 1;
    status("Pen, Line, Box or Pan · Ctrl-K chooses a color · Ctrl-S saves a BMP copy");
    if(desk_app_open(&app.window,"Sketch",800,560)) { munmap(app.arena,app.arena_bytes);return 1; }
    desk_log("desksketch: starting\n");
    while(!app.quit && !app.window.failed && !app.window.closed) {
        if(desk_clipboard_tick(&app.clipboard,srv_ticks())<0) clipboard_finish(-1);
        for(unsigned batch=0;batch<32;batch++) {
            struct desk_packet p;int result=desk_app_poll(&app.window,&p);if(result<=0) break;if(result==2) continue;
            if(p.type==DESK_CONFIGURE) {
                layout();desk_ui_focus(&app.ui,SK_CANVAS);
                if(!app.started) { app.started=1;if(argc>2 && argv[2][0]) load_image(argv[2]); }
            } else if(p.type==DESK_FOCUS) focus_changed();
            else if(p.type==DESK_PREFERENCES) { struct desk_preferences preferences;memcpy(&preferences,p.text,sizeof(preferences));app.theme.body=preferences.text_size==20?&desk_font_20:&desk_font_16;layout(); }
            else if(p.type==DESK_MENU_ACTION) action((struct desk_action){p.value,DESK_WIDGET_ACTIVATED});
            else if(p.type==DESK_KEY) { struct srv_key_event k;memcpy(&k,p.text,sizeof(k));key(&k); }
            else if(p.type==DESK_POINTER) {
                struct desk_widget_event e={.type=p.value&DESK_POINTER_CANCEL?DESK_WIDGET_CANCEL:DESK_WIDGET_POINTER,.x=p.x,.y=p.y,.down=(p.value&DESK_POINTER_LEFT)!=0,.pressed=(p.value&DESK_POINTER_PRESS)!=0};
                if(app.dialog_operation) dialog_action(desk_dialog_pointer(&app.dialog,e,srv_monotonic_us()));
                else if(app.color_open) color_action(desk_color_panel_pointer(&app.colors,e,srv_monotonic_us()));
                else if(app.picker_mode) picker_action(desk_picker_pointer(&app.picker,e,srv_monotonic_us()));
                else action(desk_ui_pointer(&app.ui,e,srv_monotonic_us()));
            } else if(p.type==DESK_SCROLL) {
                if(app.picker_mode) picker_action(desk_picker_scroll(&app.picker,p.width,p.height,p.y));
                else if(!modal()) action(desk_ui_scroll(&app.ui,p.width,p.height,p.x,p.y));
            } else if(p.type==DESK_CLIPBOARD_ACK || p.type==DESK_CLIPBOARD_DATA) { int done=desk_clipboard_receive(&app.clipboard,&p);if(done) clipboard_finish(done); }
            else if(p.type==DESK_CLOSE_REQUEST) request(SK_CLOSE);
        }
        if(app.quit || app.window.failed || app.window.closed) break;
        int retiring=app.image_job.retiring_pid || app.save_job.retiring_pid;
        int result=desk_image_job_poll(&app.image_job,srv_ticks());if(result) image_finish(result);
        result=desk_capture_job_poll(&app.save_job);if(result) save_finish(result);
        if(retiring && !app.image_job.retiring_pid && !app.save_job.retiring_pid) layout();
        if(app.file_job.busy) { result=desk_file_job_poll(&app.file_job);if(result) file_finish(result); }
        if(app.after_save && !file_busy() && !modal()) { unsigned operation=app.after_save;app.after_save=0;request(operation); }
        int dirty=desk_bitmap_dirty(&app.bitmap);if(dirty!=app.last_dirty) { app.title_dirty=1;app.last_dirty=dirty; }
        if(app.title_dirty) {
            const char *name=strrchr(app.path,'/');char title[88];snprintf(title,sizeof(title),"Sketch — %.68s%s",name?name+1:"Untitled",dirty?" *":"");
            size_t n=strlen(title);while(n && !desk_text_utf8_valid(title,n)) title[--n]=0;
            if(!desk_app_title(&app.window,title)) app.title_dirty=0;
        }
        menus();struct desk_canvas canvas;if(desk_app_canvas(&app.window,&canvas)==1) { draw(&canvas);desk_app_present(&app.window); }
        srv_sleep_ticks(1);
    }
    if(app.file_ready) desk_file_job_close(&app.file_job);
    desk_image_job_close(&app.image_job);desk_capture_job_close(&app.save_job);munmap(app.arena,app.arena_bytes);desk_app_close(&app.window);
    desk_log("desksketch: exited failure=%d\n",app.window.failed);return app.window.failed?1:0;
}
