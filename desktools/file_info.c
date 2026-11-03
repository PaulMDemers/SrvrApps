#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include <srvros/desk_log.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
enum { INFO_VIEW=1,INFO_PATH,INFO_DETAILS,INFO_CLOSE };
static void notice(struct tool *t,const char *message) { snprintf(t->info_notice,sizeof(t->info_notice),"%s",message);t->window.dirty=1; }
static int selected_path(struct tool *t,char path[DESK_FILE_PATH],uint64_t *inode) {
    int index=t->table.body.selected;if(index<0 || index>=t->file_count) return -1;
    int n=snprintf(path,DESK_FILE_PATH,"%s%s%s",t->saved_path,!strcmp(t->saved_path,"/")?"":"/",t->entries[index].name);
    if(n<0 || n>=DESK_FILE_PATH) return -1;
    if(inode) *inode=t->entries[index].inode;
    return 0;
}
static void copy(struct tool *t,int path) {
    if(t->clip.mode) { notice(t,"The clipboard is busy. Try again.");return; }
    const char *data=t->info_path;long length=strlen(data);
    if(!path) { if(!t->info_ready) return;length=desk_properties_text(&t->properties,t->clipboard,sizeof(t->clipboard));data=t->clipboard; }
    if(length<0 || desk_clipboard_copy(&t->clip,t->window.server,data,length,srv_ticks())) { notice(t,"Copy did not start. Try again.");return; }
    t->clip_action=DESK_EDIT_COPY;t->clip_target=TOOL_INFO;notice(t,path?"Copying path…":"Copying details…");
    desk_log("desktools: files-info-copy kind=%s bytes=%ld\n",path?"path":"details",length);
}
void tool_files_copy_path(struct tool *t) {
    char path[DESK_FILE_PATH];if(t->job.busy || selected_path(t,path,NULL)) { tool_status(t,"Select an item first.");return; }
    if(t->clip.mode || desk_clipboard_copy(&t->clip,t->window.server,path,strlen(path),srv_ticks())) { tool_status(t,"The clipboard is busy. Try again.");return; }
    t->clip_action=DESK_EDIT_COPY;t->clip_target=TOOL_COPY_PATH;tool_status(t,"Copying path…");
}
void tool_files_info_layout(struct tool *t) {
    if(!t->info_mode) return;
    int width=t->window.width,height=t->window.height,compact=width<400;
    desk_properties_configure(&t->properties,(struct desk_rect){16,56,width-32,height-124},t->body_size==20?&desk_font_20:&desk_font_16,t->info_mode==2);
    int x=16,y=height-56;
    desk_button_configure(&t->info_buttons[0],(struct desk_rect){x,y,compact?64:104,28},!t->clip.mode);x+=(compact?64:104)+8;
    desk_button_configure(&t->info_buttons[1],(struct desk_rect){x,y,compact?80:112,28},t->info_ready && !t->clip.mode);x+=(compact?80:112)+8;
    desk_button_configure(&t->info_buttons[2],(struct desk_rect){x,y,compact?64:80,28},1);
    t->info_ui.controls[1].label=compact?"Path":"Copy path";t->info_ui.controls[2].label=compact?"Details":"Copy details";
    desk_ui_layout_changed(&t->info_ui);desk_ui_window_focus(&t->info_ui,t->window.focused);t->window.dirty=1;
}
void tool_files_info_open(struct tool *t) {
    if(t->job.busy || t->modal || t->prompt_mode || t->info_mode || t->picker_mode) return;
    uint64_t inode;char path[DESK_FILE_PATH];
    if(selected_path(t,path,&inode)) { tool_status(t,"Select a file or folder first.");return; }
    struct desk_file_probe probe={inode};
    if(!t->worker_ready || desk_file_job_start(&t->job,DESK_FILE_STAT,path,&probe,sizeof(probe),0,0)) { tool_status(t,"The file worker is unavailable. Close and reopen Files to retry.");return; }
    strcpy(t->info_path,path);t->info_mode=1;t->info_ready=0;desk_properties_init(&t->properties);desk_ui_init(&t->info_ui);
    desk_ui_add(&t->info_ui,INFO_VIEW,DESK_CONTROL_PROPERTIES,&t->properties,NULL);
    desk_ui_add(&t->info_ui,INFO_PATH,DESK_CONTROL_BUTTON,&t->info_buttons[0],"Copy path");
    desk_ui_add(&t->info_ui,INFO_DETAILS,DESK_CONTROL_BUTTON,&t->info_buttons[1],"Copy details");
    desk_ui_add(&t->info_ui,INFO_CLOSE,DESK_CONTROL_BUTTON,&t->info_buttons[2],"Close");
    for(unsigned i=0;i<3;i++) desk_button_centered(&t->info_buttons[i],1);
    struct desk_property fields[]={{1,"Location",t->info_path},{2,"Status","Reading current information…"}};desk_properties_set(&t->properties,fields,2);
    desk_ui_window_focus(&t->ui,0);desk_popup_cancel(&t->context);tool_files_info_layout(t);desk_ui_focus(&t->info_ui,INFO_CLOSE);notice(t,"Reading current information…");
    desk_log("desktools: files-info opened path=%s\n",path);
}
void tool_files_info_close(struct tool *t) {
    if(!t->info_mode) return;
    t->info_mode=0;desk_ui_window_focus(&t->info_ui,0);tool_layout(t);desk_ui_window_focus(&t->ui,t->window.focused);desk_ui_focus(&t->ui,TOOL_LIST);
    desk_log("desktools: files-info closed\n");
}
void tool_files_info_finish(struct tool *t,int result) {
    if(!t->info_mode) { tool_layout(t);return; }
    const char *error=NULL;
    if(result<0) error="The file worker stopped. Reopen Files to retry.";
    else if(t->job.response.status==DESK_FILE_CHANGED) error="This path now refers to a different item. Close this view and refresh Files.";
    else if(t->job.response.status==DESK_FILE_NOT_FOUND) error="This item no longer exists at that path.";
    else if(t->job.response.status) error="Could not read the current information. Check the volume.";
    struct desk_file_info info={0};
    if(!error) {
        if(t->job.response.length!=sizeof(info)) error="The file worker returned invalid information.";
        else { memcpy(&info,t->job.incoming,sizeof(info));if(info.kind<DESK_FILE_KIND_FILE || info.kind>DESK_FILE_KIND_OTHER || info.permissions>0777) error="The file worker returned invalid information."; }
    }
    if(error) {
        struct desk_property fields[]={{1,"Location",t->info_path},{2,"Status",error}};desk_properties_set(&t->properties,fields,2);notice(t,error);t->info_ready=0;
    } else {
        char size[64],modified[64];snprintf(size,sizeof(size),"%llu bytes",(unsigned long long)info.size);
        time_t stamp=(time_t)info.modified;struct tm utc;
        if(info.modified<=0 || info.modified>253402300799LL || !gmtime_r(&stamp,&utc) || !strftime(modified,sizeof(modified),"%Y-%m-%d %H:%M:%S UTC",&utc)) strcpy(modified,"Not available");
        const char *name=strrchr(t->info_path,'/');name=name && name[1]?name+1:t->info_path;
        struct desk_property fields[]={
            {1,"Name",name},{2,"Kind",info.kind==DESK_FILE_KIND_FILE?"File":info.kind==DESK_FILE_KIND_FOLDER?"Folder":"Other node"},
            {3,"Size",info.kind==DESK_FILE_KIND_FOLDER?"Folder contents are not totaled":size},{4,"Location",t->info_path},{5,"Modified",modified}
        };
        if(desk_properties_set(&t->properties,fields,5)) { notice(t,"The information is too long to display.");t->info_ready=0; }
        else { t->info_ready=1;notice(t,"Current metadata. Close and reopen to refresh."); }
    }
    t->info_mode=2;tool_files_info_layout(t);
    /* Error details are still scrollable; only Copy details needs a full result. */
    desk_properties_configure(&t->properties,t->properties.frame,t->properties.body,1);desk_ui_focus(&t->info_ui,INFO_VIEW);
    desk_log("desktools: files-info ready=%d status=%u\n",t->info_ready,t->job.response.status);
}
void tool_files_info_action(struct tool *t,struct desk_action action) {
    if(action.flags&DESK_WIDGET_REDRAW) t->window.dirty=1;
    if(!(action.flags&DESK_WIDGET_ACTIVATED)) return;
    if(action.id==INFO_CLOSE) tool_files_info_close(t);else if(action.id==INFO_PATH || action.id==INFO_DETAILS) { copy(t,action.id==INFO_PATH);tool_files_info_layout(t); }
}
void tool_files_info_key(struct tool *t,const struct srv_key_event *key) {
    if((key->flags&SRV_KEY_DOWN_EVENT) && !(key->flags&SRV_KEY_REPEAT)) {
        if(key->usage==SRV_KEY_ESCAPE || ((key->modifiers&(SRV_KEY_CTRL|SRV_KEY_META)) && key->usage==26)) { tool_files_info_close(t);return; }
        if((key->modifiers&(SRV_KEY_CTRL|SRV_KEY_META)) && key->usage==6) { copy(t,(key->modifiers&SRV_KEY_SHIFT)!=0);tool_files_info_layout(t);return; }
    }
    tool_files_info_action(t,desk_ui_key(&t->info_ui,key));
}
void tool_files_info_draw(struct tool *t,struct desk_canvas *canvas) {
    desk_fill(canvas,(struct desk_rect){0,0,canvas->width,canvas->height},desk_theme_quiet.surface);
    desk_label_draw(canvas,(struct desk_rect){16,16,canvas->width-32,28},desk_theme_quiet.heading,"Information",desk_theme_quiet.text);
    desk_ui_draw(canvas,&t->info_ui,&desk_theme_quiet);
    desk_label_ellipsis(canvas,(struct desk_rect){16,canvas->height-24,canvas->width-32,22},desk_theme_quiet.body,t->info_notice,desk_theme_quiet.muted);
}
