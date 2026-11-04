#include <srvros/desk_paths.h>
#include <srvros/desk_path.h>
#include "app.h"
#include <srvros/desk_log.h>
#include <srvros/sys.h>
#include <stdio.h>
#include <string.h>
static void browse(struct tool *t) {
    (void)desk_chooser_browse(&t->chooser);
    desk_picker_focus(&t->picker,t->window.focused);t->window.dirty=1;
}
static void open_failed(struct tool *t,const char *message) {
    if(t->pending==TOOL_PENDING_CLOSE || t->closing)t->close_canceled=1;
    t->pending=t->closing=0;desk_app_cancel_close(&t->window);tool_status(t,message);
}
void tool_picker_open(struct tool *t,int mode) {
    if(t->job.busy || t->modal || t->picker_mode) return;
    if(!t->worker_ready) { open_failed(t,mode==DESK_PICKER_FOLDER?"The file worker is unavailable. Close and reopen Files to retry.":"The file worker is unavailable. Your document was kept.");return; }
    if(mode!=DESK_PICKER_FOLDER) tool_text_search_close(t);
    char folder[DESK_FILE_PATH],name[DESK_PICKER_NAME];
    const char *candidate=t->path_text.data;
    if(mode==DESK_PICKER_FOLDER) {
        snprintf(folder,sizeof(folder),"%s",t->saved_path);name[0]=0;
        goto open;
    }
    if(candidate[0]=='/') snprintf(folder,sizeof(folder),"%s",candidate);
    else snprintf(folder,sizeof(folder),"%s/Untitled.txt",desk_home());
    char *slash=strrchr(folder,'/');
    const char *leaf=slash?slash+1:"Untitled.txt";
    if(!leaf[0] || strlen(leaf)>=sizeof(name)) leaf="Untitled.txt";
    strcpy(name,mode==DESK_PICKER_SAVE?leaf:"");
    if(slash && slash!=folder) *slash=0;else strcpy(folder,"/");
open:
    if(desk_chooser_open(&t->chooser,mode,folder,name)) { open_failed(t,"The file picker could not open this path.");return; }
    t->picker_mode=mode;desk_ui_window_focus(&t->ui,0);
    desk_picker_configure(&t->picker,(struct desk_rect){0,0,t->window.width,t->window.height});
    browse(t);tool_layout(t);desk_log("desktools: picker mode=%d opened\n",mode);
}
void tool_picker_dismiss(struct tool *t) {
    t->picker_mode=0;desk_chooser_cancel(&t->chooser);
    desk_ui_window_focus(&t->ui,t->window.focused);tool_layout(t);t->window.dirty=1;
}
void tool_picker_cancel(struct tool *t) {
    if(t->pending==TOOL_PENDING_CLOSE || t->closing)t->close_canceled=1;
    t->pending=0;t->closing=0;tool_picker_dismiss(t);
    desk_app_cancel_close(&t->window);desk_ui_focus(&t->ui,t->role==DESK_APP_FILES?TOOL_LIST:TOOL_BODY);
    if(t->role==DESK_APP_FILES) { t->transfer_action=0;t->transfer_folder[0]=0;tool_status(t,"File action canceled."); }
    else tool_status(t,"File selection canceled. Your document was kept.");
}
void tool_picker_result(struct tool *t,struct desk_picker_result a) {
    if(a.flags&DESK_WIDGET_REDRAW) t->window.dirty=1;
    unsigned id=a.field==DESK_PICKER_LOCATION?TOOL_PICK_LOCATION:a.field==DESK_PICKER_FILENAME?TOOL_PICK_NAME:0;
    if(id) tool_clipboard_action(t,(struct desk_action){id,a.flags});
    if(t->picker_mode==DESK_PICKER_FOLDER && a.operation==DESK_PICKER_ACCEPT) {
        char destination[DESK_FILE_PATH];
        if(desk_path_join(destination,sizeof(destination),t->picker.directory,t->file_selected.name)) {
            desk_picker_message(&t->picker,"The destination path is too long.");t->window.dirty=1;return;
        }
        if(!strcmp(destination,t->file_source)) {
            desk_picker_message(&t->picker,"Choose a different destination folder.");t->window.dirty=1;return;
        }
    }
    struct desk_chooser_selection selected;
    int outcome=desk_chooser_action(&t->chooser,a,&selected);
    if(outcome==DESK_CHOOSER_CANCELED) { tool_picker_cancel(t);return; }
    if(a.operation==DESK_PICKER_BROWSE) {
        desk_picker_focus(&t->picker,t->window.focused);t->window.dirty=1;
    }
    if(outcome==DESK_CHOOSER_ACCEPTED) {
        t->picker_mode=0;desk_ui_window_focus(&t->ui,t->window.focused);
        if(selected.mode==DESK_PICKER_FOLDER) tool_layout(t);
        if(selected.mode==DESK_PICKER_FOLDER) tool_files_transfer_accept(t,selected.path);
        else if(selected.mode==DESK_PICKER_SAVE) tool_text_save_path(t,selected.path);else tool_text_open_path(t,selected.path);
    }
}

void tool_picker_finish(struct tool *t,int result) {
    int outcome=desk_chooser_finish(&t->chooser,t->chooser.pending_id,result);
    if(outcome==DESK_CHOOSER_STALE || t->chooser.pending)return;
    if(!t->picker_mode) {
        if(t->role==DESK_APP_FILES) tool_layout(t);
        if(t->closing && t->role!=DESK_APP_FILES) { t->closing=0;tool_text_request(t,TOOL_PENDING_CLOSE); }
        return;
    }
    if(outcome==DESK_CHOOSER_READY)
        desk_log("desktools: picker ready mode=%d items=%d path=%s\n",t->picker_mode,t->chooser.count,t->picker.directory);
    else if(outcome==DESK_CHOOSER_ERROR && t->picker_mode==DESK_PICKER_FOLDER)
        desk_log("desktools: folder picker error path=%s\n",t->job.request.path);
    desk_picker_focus(&t->picker,t->window.focused);t->window.dirty=1;
}
