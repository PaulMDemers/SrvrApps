#define _POSIX_C_SOURCE 200809L
#include <srvros/desk_paths.h>
#include "app.h"
#include <srvros/desktop_apps.h>
#include <srvros/desk_listing.h>
#include <srvros/desk_path.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <srvros/desk_log.h>

static const struct desk_table_column file_columns[]={
    {"Name",0,0,1},{"Kind",80,0,1},{"Size",108,1,1},{"Modified UTC",176,0,1}
};
static int file_compare(const struct desk_file_entry *a,const struct desk_file_entry *b,int col,int descending) {
    if(a->directory!=b->directory) return a->directory?-1:1;
    int order=0;
    if(col==2) order=a->size>b->size?1:a->size<b->size?-1:0;
    else if(col==3) order=a->modified>b->modified?1:a->modified<b->modified?-1:0;
    if(!order) order=strcasecmp(a->name,b->name);
    if(!order) order=strcmp(a->name,b->name);
    return descending?-order:order;
}
static void file_rows(struct tool *t) {
    for(int i=1;i<t->file_count;i++) {
        struct desk_file_entry value=t->entries[i];int j=i;
        while(j>0 && file_compare(&t->entries[j-1],&value,t->table.sort_column,t->table.descending)>0) { t->entries[j]=t->entries[j-1];j--; }
        t->entries[j]=value;
    }
    for(int i=0;i<t->file_count;i++) {
        struct desk_file_entry *e=&t->entries[i];
        if(e->directory) strcpy(t->details[i],"—");
        else if(e->size<1024) snprintf(t->details[i],32,"%llu B",(unsigned long long)e->size);
        else if(e->size<1048576) snprintf(t->details[i],32,"%llu KiB",(unsigned long long)((e->size+1023)/1024));
        else snprintf(t->details[i],32,"%llu MiB",(unsigned long long)(e->size/1048576));
        time_t stamp=(time_t)e->modified;struct tm utc;
        if(stamp>0 && stamp<=253402300799LL && gmtime_r(&stamp,&utc) && utc.tm_year>=70 && utc.tm_year<=8099)
            { if(!strftime(t->file_dates[i],24,"%Y-%m-%d %H:%M",&utc)) strcpy(t->file_dates[i],"—"); }
        else strcpy(t->file_dates[i],"—");
        t->table_rows[i]=(struct desk_table_row){.id=e->inode,.cells={e->name,e->directory?"Folder":"File",t->details[i],t->file_dates[i]}};
    }
    desk_table_rows(&t->table,t->table_rows,t->file_count);t->window.dirty=1;
}
/* Keep navigation state separate from in-flight folder requests. A failed request
 * never consumes history or changes the authoritative displayed folder. */
static int normalize_path(const char *path,char target[DESK_FILE_PATH]) {
    return desk_path_normalize(target,DESK_FILE_PATH,path);
}
static void remember_selection(struct tool *t) {
    if(t->location_index<0 || t->location_index>=t->location_count) return;
    struct tool_location *location=&t->locations[t->location_index];
    int selected=t->table.body.selected;
    location->selected=selected>=0 && selected<t->file_count?t->entries[selected].inode:0;
    location->scroll=t->table.body.scroll.offset;
}
static int list_directory(struct tool *t,const char *path) {
    char target[DESK_FILE_PATH];
    if(normalize_path(path,target) || !t->worker_ready || desk_file_job_start(&t->job,DESK_FILE_LIST,target,NULL,0,0,0)) {
        tool_status(t,"Enter an absolute folder path and try again.");return -1;
    }
    remember_selection(t);t->location_pending=-1;
    tool_status(t,"Opening folder…");tool_layout(t);return 0;
}
static void location_commit(struct tool *t) {
    int index=t->location_pending;t->location_pending=-1;
    if(index>=0 && index<t->location_count && !strcmp(t->locations[index].path,t->job.request.path)) {
        t->location_index=index;return;
    }
    if(t->location_index>=0 && !strcmp(t->locations[t->location_index].path,t->job.request.path)) return;
    t->location_count=t->location_index+1;
    if(t->location_count==TOOL_HISTORY) {
        memmove(t->locations,t->locations+1,(TOOL_HISTORY-1)*sizeof(*t->locations));t->location_count--;
    }
    t->location_index=t->location_count++;
    struct tool_location *location=&t->locations[t->location_index];*location=(struct tool_location){0};strcpy(location->path,t->job.request.path);
}
void tool_files_layout(struct tool *t) {
    int w=t->window.width,h=t->window.height,busy=t->job.busy,sidebar=w>=640,left=sidebar?164:16;
    desk_button_configure(&t->buttons[7],(struct desk_rect){16,16,64,28},!busy && t->location_index>0);
    desk_button_configure(&t->buttons[8],(struct desk_rect){88,16,80,28},!busy && t->location_index+1<t->location_count);
    desk_button_configure(&t->buttons[0],(struct desk_rect){176,16,52,28},!busy && t->saved_path[0] && strcmp(t->saved_path,"/"));
    desk_button_configure(&t->buttons[1],w>=324?(struct desk_rect){236,16,72,28}:(struct desk_rect){0},!busy && w>=324);
    desk_button_configure(&t->buttons[2],w>=420?(struct desk_rect){316,16,88,28}:(struct desk_rect){0},!busy && w>=420);
    for(int i=0;i<3;i++) { int x[]={412,500,612},width[]={80,104,80};
        desk_button_configure(&t->buttons[4+i],w>=708?(struct desk_rect){x[i],16,width[i],28}:(struct desk_rect){0},!busy && w>=708 && t->table.body.selected>=0); }
    for(int i=0;i<3;i++) desk_button_configure(&t->buttons[9+i],sidebar?(struct desk_rect){24,134+i*40,116,30}:(struct desk_rect){0},!busy && sidebar);
    desk_edit_configure(&t->path,(struct desk_rect){16,56,w-32,34},&desk_font_16,!busy);
    desk_table_configure(&t->table,(struct desk_rect){left,102,w-left-16,h-190},!busy);
    desk_edit_configure(&t->name,(struct desk_rect){left,h-76,w-left-140,34},&desk_font_16,!busy);
    desk_button_configure(&t->buttons[3],(struct desk_rect){w-132,h-73,116,28},!busy);
}
void tool_files_draw(struct tool *t,struct desk_canvas *c) {
    if(t->window.width>=640) {
        desk_fill(c,(struct desk_rect){16,102,132,t->window.height-190},desk_theme_quiet.track);
        desk_label_draw(c,(struct desk_rect){24,106,116,24},desk_theme_quiet.body,"Places",desk_theme_quiet.muted);
    }
    desk_ui_draw(c,&t->ui,&desk_theme_quiet);
}
void tool_files_init(struct tool *t,const char *path) {
    t->location_index=t->location_pending=-1;
    desk_table_init(&t->table);desk_table_columns(&t->table,file_columns,4);desk_table_sort_indicator(&t->table,0,0);
    desk_ui_add(&t->ui,TOOL_BACK,DESK_CONTROL_BUTTON,&t->buttons[7],"Back");
    desk_ui_add(&t->ui,TOOL_FORWARD,DESK_CONTROL_BUTTON,&t->buttons[8],"Forward");
    desk_ui_add(&t->ui,TOOL_UP,DESK_CONTROL_BUTTON,&t->buttons[0],"Up");
    desk_ui_add(&t->ui,TOOL_OPEN,DESK_CONTROL_BUTTON,&t->buttons[1],"Open");
    desk_ui_add(&t->ui,TOOL_REFRESH,DESK_CONTROL_BUTTON,&t->buttons[2],"Refresh");
    desk_ui_add(&t->ui,TOOL_RENAME,DESK_CONTROL_BUTTON,&t->buttons[4],"Rename");
    desk_ui_add(&t->ui,TOOL_DUPLICATE,DESK_CONTROL_BUTTON,&t->buttons[5],"Duplicate");
    desk_ui_add(&t->ui,TOOL_DELETE,DESK_CONTROL_BUTTON,&t->buttons[6],"Delete");
    desk_ui_add(&t->ui,TOOL_HOME,DESK_CONTROL_BUTTON,&t->buttons[9],"Home");
    desk_ui_add(&t->ui,TOOL_VOLUME,DESK_CONTROL_BUTTON,&t->buttons[10],"Startup disk");
    desk_ui_add(&t->ui,TOOL_ROOT,DESK_CONTROL_BUTTON,&t->buttons[11],"System root");
    desk_ui_add(&t->ui,TOOL_PATH,DESK_CONTROL_EDIT,&t->path,NULL);
    desk_ui_add(&t->ui,TOOL_LIST,DESK_CONTROL_TABLE,&t->table,NULL);
    desk_ui_add(&t->ui,TOOL_NAME,DESK_CONTROL_EDIT,&t->name,NULL);
    desk_ui_add(&t->ui,TOOL_MKDIR,DESK_CONTROL_BUTTON,&t->buttons[3],"New folder");
    desk_text_buffer_set(&t->path_text,path && path[0]?path:desk_home(),strlen(path && path[0]?path:desk_home()));
    desk_text_buffer_set(&t->name_text,"New folder",10);
    list_directory(t,t->path_text.data);
}
static int child_path(struct tool *t,const char *name,char path[DESK_FILE_PATH]) {
    return desk_path_join(path,DESK_FILE_PATH,t->saved_path,name);
}
static int basename_valid(const char *name) {
    size_t n=strlen(name);if(!n || n>=96 || !strcmp(name,".") || !strcmp(name,"..")) return 0;
    for(const unsigned char *p=(const unsigned char *)name;*p;p++)
        if(*p<32 || *p>=127 || strchr("/\\:*?\"<>|",*p)) return 0;
    return 1;
}
static void close_change(struct tool *t) {
    t->prompt_mode=t->modal=0;desk_prompt_focus(&t->prompt,0);desk_dialog_focus(&t->dialog,0);
    desk_ui_window_focus(&t->ui,t->window.focused);desk_ui_focus(&t->ui,TOOL_LIST);t->window.dirty=1;
}
static int start_change(struct tool *t,unsigned operation,const char *destination) {
    struct desk_file_change change={.inode=t->file_selected.inode,.size=t->file_selected.size,
        .modified=t->file_selected.modified,.directory=t->file_selected.directory};
    if(destination) snprintf(change.destination,sizeof(change.destination),"%s",destination);
    if(!t->worker_ready || desk_file_job_start(&t->job,operation,t->file_source,&change,sizeof(change),0,0)) {
        tool_status(t,"The file worker is unavailable. Close and reopen Files to retry.");return -1;
    }
    t->file_select_name[0]=0;
    if(destination) snprintf(t->file_select_name,sizeof(t->file_select_name),"%s",strrchr(destination,'/')+1);
    close_change(t);tool_status(t,operation==DESK_FILE_DELETE?"Deleting selected item…":operation==DESK_FILE_COPY?"Duplicating file…":"Renaming selected item…");
    tool_layout(t);return 0;
}
static void open_change(struct tool *t,unsigned action) {
    int index=t->table.body.selected;if(index<0 || index>=t->file_count) { tool_status(t,"Select a file or folder first.");return; }
    t->file_selected=t->entries[index];
    if(child_path(t,t->file_selected.name,t->file_source)) { tool_status(t,"This path is too long.");return; }
    if(action==TOOL_DELETE) {
        char message[384];snprintf(message,sizeof(message),"Permanently delete “%s”? There is no Trash or undo yet. Folders must be empty.",t->file_selected.name);
        struct desk_dialog_choice choices[]={{1003,"Cancel",1},{1004,"Delete",1}};
        if(desk_dialog_open(&t->dialog,"Delete selected item?",message,choices,2,1003,1003)) return;
        t->modal=3;desk_ui_window_focus(&t->ui,0);tool_layout(t);desk_dialog_focus(&t->dialog,t->window.focused);
        desk_log("desktools: files-delete-confirm name=%s\n",t->file_selected.name);return;
    }
    if(action==TOOL_DUPLICATE && (t->file_selected.directory || t->file_selected.size>DESK_FILE_COPY_BYTES)) {
        tool_status(t,"Duplicate currently supports individual files up to 16 MiB.");return;
    }
    char name[128];snprintf(name,sizeof(name),"%s",t->file_selected.name);
    if(action==TOOL_DUPLICATE) {
        const char *dot=strrchr(t->file_selected.name,'.');
        if(dot && dot!=t->file_selected.name) snprintf(name,sizeof(name),"%.*s copy%s",(int)(dot-t->file_selected.name),t->file_selected.name,dot);
        else snprintf(name,sizeof(name),"%s copy",t->file_selected.name);
    }
    if(desk_prompt_open(&t->prompt,action==TOOL_DUPLICATE?"Duplicate file":"Rename selected item",
        action==TOOL_DUPLICATE?"Choose a name for the new copy. Existing items will not be replaced.":
        "Choose a new name. Existing items will not be replaced; folders must be empty.",name,action==TOOL_DUPLICATE?"Duplicate":"Rename")) return;
    t->prompt_mode=action==TOOL_DUPLICATE?DESK_FILE_COPY:DESK_FILE_RENAME;
    desk_ui_window_focus(&t->ui,0);tool_layout(t);desk_prompt_focus(&t->prompt,t->window.focused);
    desk_log("desktools: files-prompt operation=%d name=%s\n",t->prompt_mode,t->file_selected.name);
}
void tool_files_prompt(struct tool *t,struct desk_action a) {
    if(a.flags&DESK_WIDGET_REDRAW) t->window.dirty=1;
    if(a.id==DESK_PROMPT_FIELD) tool_clipboard_action(t,(struct desk_action){TOOL_PROMPT,a.flags});
    if(!(a.flags&DESK_WIDGET_ACTIVATED)) return;
    if(a.id==DESK_PROMPT_CANCEL) { close_change(t);tool_status(t,"File action canceled.");return; }
    if(a.id!=DESK_PROMPT_ACCEPT || !t->prompt_mode) return;
    char path[DESK_FILE_PATH];
    if(!basename_valid(t->prompt.text.data) || child_path(t,t->prompt.text.data,path)) {
        tool_status(t,"Use a name under 96 ASCII characters, without slashes or reserved punctuation.");return;
    }
    if(!strcmp(path,t->file_source)) { close_change(t);tool_status(t,"The name was unchanged.");return; }
    start_change(t,t->prompt_mode,path);
}
void tool_files_dialog(struct tool *t,unsigned id) {
    if(t->modal!=3) return;
    if(id==1003) { close_change(t);tool_status(t,"Delete canceled. The item was kept.");desk_log("desktools: files-delete-canceled\n"); }
    else if(id==1004) start_change(t,DESK_FILE_DELETE,NULL);
}
void tool_files_action(struct tool *t,struct desk_action a) {
    if(t->job.busy || t->prompt_mode || t->modal || t->info_mode) return;
    if(a.id==TOOL_LIST && (a.flags&DESK_WIDGET_CHANGED)) {
        for(int i=4;i<=6;i++) desk_button_configure(&t->buttons[i],t->buttons[i].frame,t->window.width>=708 && t->table.body.selected>=0);
    }
    if(a.id==TOOL_LIST && (a.flags&DESK_TABLE_SORT)) {
        file_rows(t);desk_log("desktools: files-sort column=%d descending=%d\n",t->table.sort_column,t->table.descending);return;
    }
    if(a.id==TOOL_PATH && (a.flags&DESK_EDIT_SUBMIT)) { list_directory(t,t->path_text.data);return; }
    if(!(a.flags&DESK_WIDGET_ACTIVATED) && !(a.id==TOOL_NAME && (a.flags&DESK_EDIT_SUBMIT))) return;
    if(a.id==TOOL_FIND) { tool_launch(t,DESK_APP_FIND,t->saved_path);return; }
    if(a.id==TOOL_INFO) { tool_files_info_open(t);return; }
    if(a.id==TOOL_COPY_PATH) { tool_files_copy_path(t);return; }
    if(a.id==TOOL_BACK || a.id==TOOL_FORWARD) {
        int index=t->location_index+(a.id==TOOL_BACK?-1:1);
        if(index>=0 && index<t->location_count && !list_directory(t,t->locations[index].path)) t->location_pending=index;
        return;
    }
    if(a.id==TOOL_HOME || a.id==TOOL_VOLUME || a.id==TOOL_ROOT) {
        list_directory(t,a.id==TOOL_HOME?desk_home():a.id==TOOL_VOLUME?"/fat":"/");return;
    }
    if(a.id==TOOL_RENAME || a.id==TOOL_DUPLICATE || a.id==TOOL_DELETE) { open_change(t,a.id);return; }
    if(a.id==TOOL_UP) {
        char path[DESK_FILE_PATH];
        if(!desk_path_parent(path,sizeof(path),t->saved_path[0]?t->saved_path:"/")) list_directory(t,path);
    } else if(a.id==TOOL_REFRESH) list_directory(t,t->saved_path[0]?t->saved_path:t->path_text.data);
    else if(a.id==TOOL_OPEN || a.id==TOOL_LIST) {
        if(t->table.body.selected<0) { tool_status(t,"Select a file or folder first.");return; }
        struct desk_file_entry *entry=&t->entries[t->table.body.selected];char path[DESK_FILE_PATH];
        if(child_path(t,entry->name,path)) { tool_status(t,"This path is too long to open.");return; }
        if(entry->directory) list_directory(t,path);else tool_launch(t,desk_application_for_path(path),path);
    } else if(a.id==TOOL_MKDIR || a.id==TOOL_NAME) {
        char path[DESK_FILE_PATH];
        if(!basename_valid(t->name_text.data) || child_path(t,t->name_text.data,path) || desk_file_job_start(&t->job,DESK_FILE_MKDIR,path,NULL,0,0,0)) {
            tool_status(t,"Choose an ASCII folder name under 96 characters, without reserved punctuation.");return;
        }
        tool_status(t,"Creating folder…");tool_layout(t);
    }
}
int tool_listing_decode(struct tool *t) {
    return desk_listing_decode(t->entries,t->items,t->details,t->job.incoming,t->job.response.length,t->job.response.count);
}

void tool_files_finish(struct tool *t,int result) {
    if(t->job.request.operation==DESK_FILE_STAT) { tool_files_info_finish(t,result);return; }
    if(result<0 || t->job.response.status) {
        const char *message="Could not open or change this folder. Check the path and volume.";
        if(result<0) message="The file worker stopped. Close and reopen Files to retry.";
        else if(t->job.response.status==DESK_FILE_CHANGED) message="The selected item changed. Refresh the folder before trying again.";
        else if(t->job.response.status==DESK_FILE_EXISTS) message="That name already exists. Choose a different name.";
        else if(t->job.response.status==DESK_FILE_BUSY) message="The item is open in another process. Close it and try again.";
        else if(t->job.response.status==DESK_FILE_TOO_LARGE) message="Duplicate supports individual files up to 16 MiB.";
        else if(t->job.request.operation>=DESK_FILE_RENAME) message="The change could not be verified. Refresh before retrying. Folders must be empty and the volume writable.";
        t->location_pending=-1;tool_status(t,message);
        if(t->saved_path[0]) desk_text_buffer_set(&t->path_text,t->saved_path,strlen(t->saved_path));
        tool_layout(t);return;
    }
    if(t->job.request.operation==DESK_FILE_MKDIR || t->job.request.operation>=DESK_FILE_RENAME) {
        snprintf(t->file_notice,sizeof(t->file_notice),"%s",t->job.request.operation==DESK_FILE_DELETE?"Item deleted":
            t->job.request.operation==DESK_FILE_RENAME?"Item renamed":t->job.request.operation==DESK_FILE_COPY?"Copy verified":"Folder created");
        list_directory(t,t->saved_path);return;
    }
    int count=tool_listing_decode(t);
    if(count<0) {
        t->location_pending=-1;tool_status(t,"The folder listing was invalid.");
        desk_text_buffer_set(&t->path_text,t->saved_path,strlen(t->saved_path));tool_layout(t);return;
    }
    location_commit(t);
    if(strcmp(t->saved_path,t->job.request.path)) desk_table_rows(&t->table,NULL,0);
    strcpy(t->saved_path,t->job.request.path);t->title_dirty=1;desk_text_buffer_set(&t->path_text,t->saved_path,strlen(t->saved_path));
    t->file_count=count;file_rows(t);
    struct tool_location *location=&t->locations[t->location_index];
    for(int i=0;i<count;i++) if(t->entries[i].inode==location->selected) desk_list_select(&t->table.body,i);
    desk_scrollbar_set(&t->table.body.scroll,location->scroll);
    desk_log("desktools: files-location path=%s history=%d/%d\n",t->saved_path,t->location_index+1,t->location_count);
    if(t->file_select_name[0]) {
        for(int i=0;i<count;i++) if(!strcmp(t->entries[i].name,t->file_select_name)) desk_list_select(&t->table.body,i);
        t->file_select_name[0]=0;
    }
    tool_layout(t);desk_ui_focus(&t->ui,TOOL_LIST);
    snprintf(t->status,sizeof(t->status),"%u items%s%s%s",(unsigned)count,t->job.response.truncated?" · Limited listing":"",
        t->file_notice[0]?" · ":"",t->file_notice);t->file_notice[0]=0;
    t->window.dirty=1;
    if(t->closing) t->quit=1;
}
