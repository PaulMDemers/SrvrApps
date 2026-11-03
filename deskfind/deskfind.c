#include <srvros/desk_paths.h>
#include <srvros/desk_app.h>
#include <srvros/desk_ui.h>
#include <srvros/desk_popup.h>
#include <srvros/desk_file_search.h>
#include <srvros/desk_path.h>
#include <srvros/desk_command_job.h>
#include <srvros/desk_clipboard.h>
#include <srvros/desktop_apps.h>
#include <srvros/desk_log.h>
#include <srvros/sys.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
enum { FIND_QUERY=1,FIND_ROOT,FIND_RUN,FIND_STOP,FIND_TABLE,FIND_OPEN,FIND_REVEAL,FIND_COPY,FIND_TABS,FIND_ALL=20,FIND_FILES,FIND_FOLDERS,FIND_CLOSE,FIND_EDIT=100 };
struct search_app {
    struct desk_app window;
    struct desk_ui ui;
    struct desk_edit query,root;
    struct desk_text_buffer query_text,root_text;
    char query_storage[2][DESK_SEARCH_QUERY],root_storage[2][DESK_SEARCH_PATH];
    struct desk_tabs tabs;
    struct desk_button search,stop,open,reveal,copy;
    struct desk_table table;
    struct desk_popup context;
    struct desk_command_job job;
    unsigned char incoming[DESK_SEARCH_BYTES+1];
    struct desk_search_header header;
    struct desk_search_entry entries[DESK_SEARCH_LIMIT];
    struct desk_table_row rows[DESK_SEARCH_LIMIT];
    unsigned order[DESK_SEARCH_LIMIT];
    char folders[DESK_SEARCH_LIMIT][DESK_SEARCH_PATH],sizes[DESK_SEARCH_LIMIT][32];
    char run_root[DESK_SEARCH_PATH],run_query[DESK_SEARCH_QUERY],status[256];
    unsigned run_kind;
    struct desk_clipboard_client clipboard;
    unsigned char clipboard_data[DESK_CLIPBOARD_BYTES+1];
    unsigned clip_action,clip_field,context_field;
    uint64_t clip_revision,now;
    size_t clip_caret,clip_anchor;
    int quit;
};
static struct search_app app;
static const struct desk_table_column columns[]={{"Name",0,0,1},{"Kind",68,0,1},{"Size",88,1,1},{"Folder",0,0,1}};
static const struct desk_table_column compact_columns[]={{"Name",0,0,1},{"Kind",68,0,1}};
static void rows(void);
static void status(const char *s) { snprintf(app.status,sizeof(app.status),"%s",s);app.window.dirty=1; }
static void parent_path(const char *path,char *parent) { (void)desk_path_parent(parent,DESK_SEARCH_PATH,path); }
static struct desk_search_entry *selected(void) { int i=app.table.body.selected;return i>=0 && (unsigned)i<app.header.count?&app.entries[app.order[i]]:NULL; }
static void layout(void) {
    int w=app.window.width,h=app.window.height,busy=app.job.busy,have=selected()!=NULL,small=h<320,button_y=small?96:138,table_y=small?136:178;
    if(app.table.column_count!=(w<480?2:4)) {
        int sort=app.table.sort_column,descending=app.table.descending;desk_table_columns(&app.table,w<480?compact_columns:columns,w<480?2:4);desk_table_sort_indicator(&app.table,sort<app.table.column_count?sort:0,descending);rows();
    }
    desk_edit_configure(&app.query,(struct desk_rect){64,12,w-76,30},&desk_font_16,1);
    desk_edit_configure(&app.root,(struct desk_rect){64,54,w-76,30},&desk_font_16,1);
    desk_tabs_configure(&app.tabs,small?(struct desk_rect){0}:(struct desk_rect){12,96,w-24,30},!busy && !small);
    desk_button_configure(&app.search,(struct desk_rect){12,button_y,80,28},!busy && !app.job.retiring_pid);
    desk_button_configure(&app.stop,(struct desk_rect){100,button_y,64,28},busy);
    desk_button_configure(&app.open,(struct desk_rect){172,button_y,80,28},have);
    desk_button_configure(&app.reveal,w>=420?(struct desk_rect){260,button_y,140,28}:(struct desk_rect){0},have && w>=420);
    desk_button_configure(&app.copy,w>=532?(struct desk_rect){408,button_y,112,28}:(struct desk_rect){0},have && w>=532 && !app.clipboard.mode);
    desk_table_configure(&app.table,(struct desk_rect){12,table_y,w-24,h-table_y-36},!busy);
    desk_popup_cancel(&app.context);desk_ui_layout_changed(&app.ui);desk_ui_window_focus(&app.ui,app.window.focused);app.window.dirty=1;
}
static int compare(unsigned ia,unsigned ib) {
    const struct desk_search_entry *a=&app.entries[ia],*b=&app.entries[ib];int col=app.table.sort_column,order=0;
    if(a->directory!=b->directory) return a->directory?-1:1;
    if(col==2) order=a->size>b->size?1:a->size<b->size?-1:0;
    if(col==3) order=strcasecmp(app.folders[ia],app.folders[ib]);
    if(!order) order=strcasecmp(strrchr(a->path,'/')+1,strrchr(b->path,'/')+1);
    if(!order) order=strcmp(a->path,b->path);
    return app.table.descending?-order:order;
}
static void rows(void) {
    for(unsigned i=0;i<app.header.count;i++) {
        parent_path(app.entries[i].path,app.folders[i]);
        if(app.entries[i].directory) strcpy(app.sizes[i],"—");
        else snprintf(app.sizes[i],sizeof(app.sizes[i]),"%llu B",(unsigned long long)app.entries[i].size);
    }
    for(unsigned i=1;i<app.header.count;i++) { unsigned value=app.order[i],j=i;while(j && compare(app.order[j-1],value)>0) { app.order[j]=app.order[j-1];j--; }app.order[j]=value; }
    for(unsigned i=0;i<app.header.count;i++) {
        unsigned index=app.order[i];const struct desk_search_entry *e=&app.entries[index];
        app.rows[i]=(struct desk_table_row){.id=index+1,.cells={strrchr(e->path,'/')+1,e->directory?"Folder":"File",app.sizes[index],app.folders[index]}};
    }
    desk_table_rows(&app.table,app.rows,app.header.count);app.window.dirty=1;
}
static void run(void) {
    if(app.job.busy || app.job.retiring_pid) return;
    if(!desk_file_search_query_valid(app.query_text.data)) { status("Enter part of a file name. Matching is literal; ASCII letters ignore case.");desk_ui_focus(&app.ui,FIND_QUERY);return; }
    char root[DESK_SEARCH_PATH];
    if(desk_path_normalize(root,sizeof(root),app.root_text.data)) { status("Use an absolute folder path within 159 bytes.");desk_ui_focus(&app.ui,FIND_ROOT);return; }
    char kind[2]={(char)('0'+desk_tabs_selected(&app.tabs)-FIND_ALL),0};
    char *args[]={"deskfind-worker",root,app.query_text.data,kind,NULL};
    if(desk_command_job_start(&app.job,"/fat/bin/deskfind-worker",args,app.now,60000000)) { status("The search could not start. Try again.");return; }
    desk_text_buffer_set(&app.root_text,root,strlen(root));strcpy(app.run_root,root);strcpy(app.run_query,app.query_text.data);app.run_kind=kind[0]-'0';
    memset(&app.header,0,sizeof(app.header));desk_table_rows(&app.table,NULL,0);status("Searching names… You can keep working in other windows.");layout();
    desk_log("deskfind: started root=%s query=%s kind=%u\n",app.run_root,app.run_query,app.run_kind);
}
static void stop(void) {
    if(!app.job.busy) return;
    desk_command_job_cancel(&app.job);status("Search stopped. Run again when ready.");layout();desk_log("deskfind: canceled\n");
}
static void poll_job(void) {
    uint64_t retiring=app.job.retiring_pid;int result=desk_command_job_poll(&app.job,app.now);
    if(result) {
        int count=-1;
        if(result>0 && !app.job.exit_status && !app.job.truncated) count=desk_file_search_decode(&app.header,app.entries,app.incoming,app.job.length,app.run_root,app.run_query,app.run_kind);
        if(count<0) status(app.job.timed_out?"Search timed out after 60 seconds. Try a smaller location.":"Search did not finish. Check that the folder exists, then try again.");
        else {
            for(int i=0;i<count;i++) app.order[i]=i;
            rows();if(count) desk_list_select(&app.table.body,0);
            snprintf(app.status,sizeof(app.status),"%u results for “%s”%s%s",app.header.count,app.run_query,app.header.flags&DESK_SEARCH_TRUNCATED?" · First 256 matches":"",app.header.skipped?" · Some paths could not be shown":"");
        }
        layout();if(count>=0) desk_ui_focus(&app.ui,FIND_TABLE);desk_log("deskfind: finished result=%d exit=%llu count=%d matched=%llu skipped=%llu\n",result,(unsigned long long)app.job.exit_status,count,(unsigned long long)app.header.matched,(unsigned long long)app.header.skipped);
    }
    if(retiring && !app.job.retiring_pid) layout();
}
static void open_result(int reveal) {
    struct desk_search_entry *e=selected();if(!e) return;
    char path[DESK_SEARCH_PATH];strcpy(path,e->path);if(reveal) parent_path(e->path,path);
    unsigned application=reveal || e->directory?DESK_APP_FILES:desk_application_for_path(path);
    if(desk_app_launch(&app.window,application,path)) status("The result could not be opened. Try again.");
    else desk_log("deskfind: open application=%u path=%s\n",application,path);
}
static void copy_path(void) {
    struct desk_search_entry *e=selected();if(!e || app.clipboard.mode) return;
    if(desk_clipboard_copy(&app.clipboard,app.window.server,e->path,strlen(e->path),srv_ticks())) { status("The path could not be copied. Try again.");return; }
    app.clip_action=FIND_COPY;status("Copying path…");desk_log("deskfind: copy path=%s\n",e->path);
}
static struct desk_edit *editor(unsigned field) { return field==FIND_QUERY?&app.query:field==FIND_ROOT?&app.root:NULL; }
static void clipboard_action(unsigned field,unsigned flags) {
    unsigned a=flags&(DESK_EDIT_COPY|DESK_EDIT_CUT|DESK_EDIT_PASTE);struct desk_edit *edit=editor(field);
    if(!a || !edit || app.clipboard.mode) return;
    struct desk_text_buffer *t=edit->text;size_t lo=t->anchor<t->caret?t->anchor:t->caret,hi=t->anchor>t->caret?t->anchor:t->caret;
    if(a!=DESK_EDIT_PASTE && lo==hi) return;
    int failed=a==DESK_EDIT_PASTE?desk_clipboard_paste(&app.clipboard,app.window.server,srv_ticks()):desk_clipboard_copy(&app.clipboard,app.window.server,t->data+lo,hi-lo,srv_ticks());
    if(failed) { status("The clipboard is unavailable. Try again.");return; }
    app.clip_action=FIND_EDIT+a;app.clip_field=field;app.clip_revision=t->revision;app.clip_caret=t->caret;app.clip_anchor=t->anchor;
}
static void clipboard_finish(int result) {
    unsigned a=app.clip_action;app.clip_action=0;
    if(result<0) { status("Clipboard transfer did not finish. Try again.");return; }
    if(a==FIND_COPY) { status("Path copied.");return; }
    if(a<FIND_EDIT) return;
    a-=FIND_EDIT;if(a==DESK_EDIT_COPY) return;
    struct desk_edit *edit=editor(app.clip_field);if(!edit) return;struct desk_text_buffer *t=edit->text;
    if(!edit->focused || t->revision!=app.clip_revision || t->caret!=app.clip_caret || t->anchor!=app.clip_anchor) { status("The field changed during transfer. Repeat Cut or Paste.");return; }
    if(a==DESK_EDIT_PASTE && (memchr(app.clipboard.buffer,'\n',app.clipboard.length) || memchr(app.clipboard.buffer,'\r',app.clipboard.length))) { status("This field accepts one line.");return; }
    if(desk_text_buffer_insert(t,a==DESK_EDIT_CUT?"":(char *)app.clipboard.buffer,a==DESK_EDIT_CUT?0:app.clipboard.length)<0) status("That text is too long or invalid. The field was kept.");
    else { desk_edit_reveal(edit);app.window.dirty=1; }
}
static void action(struct desk_action a) {
    if(a.flags&DESK_WIDGET_REDRAW) app.window.dirty=1;
    if(a.id==FIND_QUERY || a.id==FIND_ROOT) { clipboard_action(a.id,a.flags);if(a.flags&DESK_EDIT_SUBMIT) run(); }
    if(a.id==FIND_TABLE) { if(a.flags&DESK_TABLE_SORT) rows();if(a.flags&DESK_WIDGET_CHANGED) { int have=selected()!=NULL;desk_button_configure(&app.open,app.open.frame,have);desk_button_configure(&app.reveal,app.reveal.frame,have && app.window.width>=420);desk_button_configure(&app.copy,app.copy.frame,have && app.window.width>=532 && !app.clipboard.mode); } }
    if(!(a.flags&DESK_WIDGET_ACTIVATED)) return;
    if(a.id>=FIND_EDIT+DESK_EDIT_SELECT_ALL && a.id<=FIND_EDIT+DESK_EDIT_REDO) { struct desk_edit *edit=editor(app.context_field);if(edit) { unsigned flags=desk_edit_command(edit,a.id-FIND_EDIT);clipboard_action(app.context_field,flags);if(flags&DESK_WIDGET_REDRAW) app.window.dirty=1; }return; }
    switch(a.id) {
    case FIND_RUN:run();break;case FIND_STOP:stop();break;case FIND_TABLE:case FIND_OPEN:open_result(0);break;
    case FIND_REVEAL:open_result(1);break;case FIND_COPY:copy_path();break;case FIND_CLOSE:app.quit=1;break;
    case FIND_ALL:case FIND_FILES:case FIND_FOLDERS:if(!app.job.busy) { desk_tabs_select(&app.tabs,a.id);layout(); }break;
    }
}
static void context(int x,int y,int keyboard) {
    unsigned field=keyboard?(app.query.focused?FIND_QUERY:app.root.focused?FIND_ROOT:0):desk_widget_contains(app.query.frame,x,y)?FIND_QUERY:desk_widget_contains(app.root.frame,x,y)?FIND_ROOT:0;
    struct desk_menu_item items[6];unsigned count=0;app.context_field=field;
    if(field) {
        desk_ui_focus(&app.ui,field);struct desk_edit *edit=editor(field);if(keyboard) { x=edit->frame.x+8;y=edit->frame.y+24; }
        unsigned selected=edit->text->anchor!=edit->text->caret?0:DESK_MENU_DISABLED,busy=app.clipboard.mode?DESK_MENU_DISABLED:0;
        items[count++]=(struct desk_menu_item){FIND_EDIT+DESK_EDIT_UNDO,desk_text_buffer_can_undo(edit->text,0)?0:DESK_MENU_DISABLED,"Undo","Ctrl-Z"};
        items[count++]=(struct desk_menu_item){FIND_EDIT+DESK_EDIT_COMMAND_CUT,selected|busy,"Cut","Ctrl-X"};
        items[count++]=(struct desk_menu_item){FIND_EDIT+DESK_EDIT_COMMAND_COPY,selected|busy,"Copy","Ctrl-C"};
        items[count++]=(struct desk_menu_item){FIND_EDIT+DESK_EDIT_COMMAND_PASTE,busy,"Paste","Ctrl-V"};
        items[count++]=(struct desk_menu_item){FIND_EDIT+DESK_EDIT_SELECT_ALL,0,"Select all","Ctrl-A"};
    } else {
        if(!keyboard) { desk_ui_focus(&app.ui,FIND_TABLE);desk_table_event(&app.table,(struct desk_widget_event){.type=DESK_WIDGET_POINTER,.x=x,.y=y,.down=1,.pressed=1},app.now);desk_table_event(&app.table,(struct desk_widget_event){.type=DESK_WIDGET_CANCEL},app.now); }
        unsigned disabled=selected()?0:DESK_MENU_DISABLED;
        items[count++]=(struct desk_menu_item){FIND_OPEN,disabled,"Open","Enter"};items[count++]=(struct desk_menu_item){FIND_REVEAL,disabled,"Show in Files","Ctrl-R"};
        items[count++]=(struct desk_menu_item){FIND_COPY,disabled|(app.clipboard.mode?DESK_MENU_DISABLED:0),"Copy path","Ctrl-C"};
    }
    desk_popup_open(&app.context,items,count,x,y,(struct desk_rect){0,0,app.window.width,app.window.height},&desk_font_16,keyboard);app.window.dirty=1;
}
static void popup_action(struct desk_menu_result r) { if(r.redraw) app.window.dirty=1;if(r.action) action((struct desk_action){r.action,DESK_WIDGET_ACTIVATED}); }
static void key(const struct srv_key_event *k) {
    if(desk_popup_active(&app.context)) { popup_action(desk_popup_key(&app.context,k));return; }
    if((k->flags&SRV_KEY_DOWN_EVENT) && !(k->flags&SRV_KEY_REPEAT)) {
        if(k->usage==SRV_KEY_ESCAPE && app.job.busy) { stop();return; }
        if(k->modifiers&(SRV_KEY_CTRL|SRV_KEY_META)) {
            unsigned id=k->usage==SRV_KEY_ENTER?FIND_RUN:k->usage==26?FIND_CLOSE:k->usage==21?FIND_REVEAL:k->usage==6 && app.table.body.focused?FIND_COPY:0;
            unsigned field=k->usage==9?FIND_QUERY:k->usage==15?FIND_ROOT:0;
            if(field) { desk_ui_focus(&app.ui,field);struct desk_edit *edit=editor(field);desk_text_buffer_select(edit->text,0,edit->text->length);desk_edit_reveal(edit);app.window.dirty=1;return; }
            if(k->usage>=30 && k->usage<=32) id=FIND_ALL+k->usage-30;
            if(id) { action((struct desk_action){id,DESK_WIDGET_ACTIVATED});return; }
        }
    }
    action(desk_ui_key(&app.ui,k));
}
static void menus(void) {
    unsigned disabled=selected()?0:DESK_MENU_DISABLED,busy=app.job.busy?DESK_MENU_DISABLED:0;
    struct desk_menu_item items[]={
        {0,DESK_MENU_HEADING,"Search",""},{FIND_RUN,busy|(app.job.retiring_pid?DESK_MENU_DISABLED:0),"Search","Ctrl-Enter"},
        {FIND_STOP,app.job.busy?0:DESK_MENU_DISABLED,"Stop","Esc"},{FIND_OPEN,disabled,"Open result","Enter"},
        {FIND_REVEAL,disabled,"Show in Files","Ctrl-R"},{FIND_COPY,disabled|(app.clipboard.mode?DESK_MENU_DISABLED:0),"Copy path","Ctrl-C"},
        {FIND_CLOSE,0,"Close window","Ctrl-W"},{0,DESK_MENU_HEADING,"Kind",""},
        {FIND_ALL,busy|(desk_tabs_selected(&app.tabs)==FIND_ALL?DESK_MENU_CHECKED:0),"All items","Ctrl-1"},
        {FIND_FILES,busy|(desk_tabs_selected(&app.tabs)==FIND_FILES?DESK_MENU_CHECKED:0),"Files","Ctrl-2"},
        {FIND_FOLDERS,busy|(desk_tabs_selected(&app.tabs)==FIND_FOLDERS?DESK_MENU_CHECKED:0),"Folders","Ctrl-3"}
    };
    desk_app_menus(&app.window,items,sizeof(items)/sizeof(*items));desk_app_menu_tick(&app.window);
}
static void draw(struct desk_canvas *canvas) {
    const struct desk_theme *theme=&desk_theme_quiet;desk_fill(canvas,(struct desk_rect){0,0,canvas->width,canvas->height},theme->surface);desk_ui_draw(canvas,&app.ui,theme);
    desk_label_draw(canvas,(struct desk_rect){12,16,48,24},&desk_font_16,"Name",theme->muted);desk_label_draw(canvas,(struct desk_rect){12,58,48,24},&desk_font_16,"In",theme->muted);
    desk_label_ellipsis(canvas,(struct desk_rect){12,canvas->height-26,canvas->width-24,22},&desk_font_16,app.status,theme->muted);desk_popup_draw(canvas,&app.context,theme);
}
static void initialize(const char *root) {
    desk_ui_init(&app.ui);desk_popup_init(&app.context);desk_tabs_init(&app.tabs);const struct desk_tab tabs[]={{FIND_ALL,"All items",0},{FIND_FILES,"Files",0},{FIND_FOLDERS,"Folders",0}};desk_tabs_items(&app.tabs,tabs,3);
    desk_text_buffer_init(&app.query_text,app.query_storage[0],app.query_storage[1],sizeof(app.query_storage[0]));desk_edit_init(&app.query,&app.query_text,0);
    desk_text_buffer_init(&app.root_text,app.root_storage[0],app.root_storage[1],sizeof(app.root_storage[0]));desk_edit_init(&app.root,&app.root_text,0);
    char normalized_root[DESK_SEARCH_PATH];
    if(desk_path_normalize(normalized_root,sizeof(normalized_root),root)) root=desk_home();else root=normalized_root;
    desk_text_buffer_set(&app.root_text,root,strlen(root));desk_table_init(&app.table);desk_table_columns(&app.table,columns,4);desk_table_sort_indicator(&app.table,0,0);
    desk_command_job_init(&app.job,(char *)app.incoming,sizeof(app.incoming));desk_clipboard_init(&app.clipboard,app.clipboard_data);
    desk_ui_add(&app.ui,FIND_QUERY,DESK_CONTROL_EDIT,&app.query,NULL);desk_ui_add(&app.ui,FIND_ROOT,DESK_CONTROL_EDIT,&app.root,NULL);desk_ui_add(&app.ui,FIND_TABS,DESK_CONTROL_TABS,&app.tabs,NULL);
    desk_ui_add(&app.ui,FIND_RUN,DESK_CONTROL_BUTTON,&app.search,"Search");desk_ui_add(&app.ui,FIND_STOP,DESK_CONTROL_BUTTON,&app.stop,"Stop");
    desk_ui_add(&app.ui,FIND_OPEN,DESK_CONTROL_BUTTON,&app.open,"Open");desk_ui_add(&app.ui,FIND_REVEAL,DESK_CONTROL_BUTTON,&app.reveal,"Show in Files");desk_ui_add(&app.ui,FIND_COPY,DESK_CONTROL_BUTTON,&app.copy,"Copy path");
    desk_ui_add(&app.ui,FIND_TABLE,DESK_CONTROL_TABLE,&app.table,NULL);
    desk_button_centered(&app.search,1);desk_button_centered(&app.stop,1);desk_button_centered(&app.open,1);desk_button_centered(&app.reveal,1);desk_button_centered(&app.copy,1);
    status("Find names in a folder and its subfolders. File contents are not searched.");
}
int main(int argc,char **argv) {
    initialize(argc>2?argv[2]:NULL);if(desk_app_open(&app.window,"Find Files",760,500)) return 1;desk_log("deskfind: starting\n");
    while(!app.quit && !app.window.failed && !app.window.closed) {
        long now=srv_monotonic_us();app.now=now<0?(uint64_t)srv_ticks()*10000:(uint64_t)now;poll_job();
        if(desk_clipboard_tick(&app.clipboard,srv_ticks())<0) clipboard_finish(-1);
        for(unsigned batch=0;batch<32;batch++) {
            struct desk_packet p;int result=desk_app_poll(&app.window,&p);if(result<=0) break;if(result==2) continue;
            if(p.type==DESK_CONFIGURE) layout();
            else if(p.type==DESK_FOCUS) { desk_ui_window_focus(&app.ui,p.value!=0);desk_popup_cancel(&app.context);app.window.dirty=1; }
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
    desk_command_job_close(&app.job);desk_app_close(&app.window);desk_log("deskfind: exited failure=%d\n",app.window.failed);return app.window.failed?1:0;
}
