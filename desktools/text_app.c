#include <srvros/desk_paths.h>
#include "app.h"
#include "timing.h"
#include <srvros/desk_log.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int tool_modified(const struct tool *t) {
    return t->body_text.length!=t->saved_length || memcmp(t->body_text.data,t->saved_data,t->saved_length);
}
static void cancel_pending(struct tool *t) {
    if(t->pending==TOOL_PENDING_CLOSE || t->closing)t->close_canceled=1;
    t->pending=t->closing=0;desk_app_cancel_close(&t->window);
}
static void load(struct tool *t,const char *path) {
    if(!t->worker_ready || desk_file_job_start(&t->job,DESK_FILE_READ,path,NULL,0,0,0)) { tool_status(t,"The document could not be opened.");cancel_pending(t);return; }
    tool_status(t,"Opening document…");tool_layout(t);
}
static void save(struct tool *t,int overwrite) {
    const char *path=overwrite?t->job.request.path:t->path_text.data;
    char destination[DESK_FILE_PATH];snprintf(destination,sizeof(destination),"%s",path);
    unsigned flags=overwrite || (t->saved_path[0] && !strcmp(destination,t->saved_path))?DESK_FILE_EXPECT_EXISTING:0;
    uint32_t crc=overwrite?t->overwrite_crc:t->saved_crc;
    if(!t->worker_ready || desk_file_job_start(&t->job,DESK_FILE_WRITE,destination,t->body_text.data,t->body_text.length,flags,crc)) {
        tool_status(t,"Enter an absolute file path, then try Save again.");cancel_pending(t);return;
    }
    tool_status(t,"Saving document…");tool_layout(t);
}
void tool_text_save(struct tool *t,int choose_path) {
    if(t->job.busy) return;
    if(choose_path || !t->saved_path[0] || strcmp(t->path_text.data,t->saved_path)) tool_picker_open(t,DESK_PICKER_SAVE);
    else save(t,0);
}
void tool_text_save_path(struct tool *t,const char *path) {
    if(desk_text_buffer_set(&t->path_text,path,strlen(path))<0) { tool_status(t,"This path is too long.");cancel_pending(t);return; }
    save(t,0);
}
static void continue_action(struct tool *t) {
    int pending=t->pending;t->pending=0;
    if(pending==TOOL_PENDING_CLOSE) { t->quit=1;return; }
    if(pending==TOOL_PENDING_OPEN) { load(t,t->pending_path);return; }
    if(pending==TOOL_PENDING_NEW) {
        tool_text_search_close(t);
        desk_text_buffer_set(&t->body_text,"",0);
        char initial[DESK_FILE_PATH];snprintf(initial,sizeof(initial),"%s/Untitled.txt",desk_home());
        desk_text_buffer_set(&t->path_text,initial,strlen(initial));
        t->saved_path[0]=t->saved_data[0]=0;t->saved_length=0;t->saved_crc=0;
        t->body.scroll_x=t->body.scroll_y=0;t->title_dirty=1;
        tool_status(t,"New document. Choose a path and save when ready.");desk_ui_focus(&t->ui,TOOL_BODY);
    }
}
/* Reserved storage belongs to the enclosing mapped document, never the heap.
 * Standalone callers retain the existing allocation/fallback behavior. */
int tool_text_history_prepare(struct tool *t,struct desk_text_history *reserved) {
    if(t->body_history)return -1;
    t->body_history=reserved?reserved:malloc(sizeof(*t->body_history)+TOOL_TEXT_HISTORY_BYTES);
    t->body_history_owned=!reserved && t->body_history!=NULL;
    if(!t->body_history)return -1;
    if(desk_text_buffer_history(&t->body_text,t->body_history,
        (char *)(t->body_history+1),TOOL_TEXT_HISTORY_BYTES)) {
        tool_text_history_release(t);return -1;
    }
    return 0;
}
void tool_text_history_release(struct tool *t) {
    if(t->body_history_owned)free(t->body_history);
    t->body_history=NULL;t->body_history_owned=0;t->body_text.journal=NULL;
    t->body_text.history=0;
}
void tool_text_init(struct tool *t,const char *path) {
    desk_searchbar_init(&t->search);desk_edit_scrollbars(&t->body,1);
    uint64_t timing=tool_timing_begin(t->role,t->window.window,"text-history-init");
    struct tool_phase_trace phase;tool_phase_begin(&phase);
    /* A runtime document attaches its reserved journal before opening. */
    if(!t->body_history)(void)tool_text_history_prepare(t,NULL);
    tool_phase_end(&phase,t->role,t->window.window,"text-history-init");
    tool_timing_end(t->role,t->window.window,"text-history-init",timing);
    desk_log("desktools: text history=%s\n",t->body_history?"multi-step":"single-step");
    char initial[DESK_FILE_PATH];snprintf(initial,sizeof(initial),"%s/Untitled.txt",desk_home());
    desk_text_buffer_set(&t->path_text,initial,strlen(initial));
    desk_ui_add(&t->ui,TOOL_NEW,DESK_CONTROL_BUTTON,&t->buttons[0],"New");
    desk_ui_add(&t->ui,TOOL_OPEN,DESK_CONTROL_BUTTON,&t->buttons[1],"Open");
    desk_ui_add(&t->ui,TOOL_SAVE,DESK_CONTROL_BUTTON,&t->buttons[2],"Save");
    desk_ui_add(&t->ui,TOOL_PATH,DESK_CONTROL_EDIT,&t->path,NULL);
    desk_ui_add(&t->ui,TOOL_BODY,DESK_CONTROL_EDIT,&t->body,NULL);
    tool_status(t,"Type a document. Ctrl-S saves; Tab moves between controls.");
    if(path && path[0]) {
        if(desk_text_buffer_set(&t->path_text,path,strlen(path))<0) tool_status(t,"This path is too long.");
        else load(t,path);
    }
}
void tool_text_open_path(struct tool *t,const char *path) {
    if(t->job.busy || strlen(path)>=sizeof(t->pending_path)) return;
    strcpy(t->pending_path,path);t->pending=TOOL_PENDING_OPEN;
    if(tool_modified(t)) tool_dialog(t,1);else continue_action(t);
}
void tool_text_request(struct tool *t,int pending) {
    if(t->job.busy) {
        if(pending==TOOL_PENDING_CLOSE) { t->closing=1;tool_status(t,"Finishing the file operation before closing…"); }
        return;
    }
    t->pending=pending;
    if(pending==TOOL_PENDING_OPEN) snprintf(t->pending_path,sizeof(t->pending_path),"%s",t->path_text.data);
    if(tool_modified(t)) tool_dialog(t,1);else continue_action(t);
}
void tool_text_action(struct tool *t,struct desk_action a) {
    if(a.id==TOOL_FIND_QUERY && (a.flags&DESK_WIDGET_CHANGED)) tool_text_search_step(t,0,1);
    if(a.flags&DESK_WIDGET_ACTIVATED) {
        if(a.id==TOOL_FIND) tool_text_search_open(t);
        if(a.id==TOOL_REPLACE) tool_text_replace_open(t);
        if(a.id==TOOL_FIND_NEXT || a.id==TOOL_FIND_PREVIOUS) tool_text_search_step(t,a.id==TOOL_FIND_PREVIOUS,0);
        if(a.id==TOOL_FIND_CLOSE) tool_text_search_close(t);
    }
    if(a.id==TOOL_BODY && (a.flags&DESK_WIDGET_CHANGED)) {
        t->title_dirty=1;tool_status(t,"Edited. Ctrl-S saves the current document.");
    }
    if(a.flags&DESK_EDIT_FULL) tool_status(t,"The document has reached the 64 KiB editing limit.");
    if(a.id==TOOL_PATH && (a.flags&DESK_EDIT_SUBMIT)) tool_text_request(t,TOOL_PENDING_OPEN);
    if(a.id==TOOL_OPEN && (a.flags&DESK_WIDGET_ACTIVATED)) tool_picker_open(t,DESK_PICKER_OPEN);
    if(a.id==TOOL_NEW && (a.flags&DESK_WIDGET_ACTIVATED)) tool_text_request(t,TOOL_PENDING_NEW);
    if(a.id==TOOL_SAVE && (a.flags&DESK_WIDGET_ACTIVATED) && !t->job.busy) tool_text_save(t,0);
}
void tool_text_dialog(struct tool *t,unsigned id) {
    int modal=t->modal;
    if(id==1003) {
        if(t->pending==TOOL_PENDING_CLOSE || t->closing)t->close_canceled=1;
        t->modal=0;t->pending=0;t->closing=0;
        desk_app_cancel_close(&t->window);desk_ui_window_focus(&t->ui,1);t->window.dirty=1;return;
    }
    if(id==1002 && modal==1) {
        t->modal=0;desk_ui_window_focus(&t->ui,1);continue_action(t);t->window.dirty=1;return;
    }
    if(id==1001) {
        t->modal=0;desk_ui_window_focus(&t->ui,1);if(modal==2) save(t,1);else tool_text_save(t,0);t->window.dirty=1;
    }
}
void tool_text_finish(struct tool *t,int result) {
    if(result<0) { tool_status(t,"The file worker stopped. Your text remains in this window.");cancel_pending(t);tool_layout(t);return; }
    unsigned status=t->job.response.status,operation=t->job.request.operation;
    if(status==DESK_FILE_EXISTS) {
        t->overwrite_crc=t->job.response.existing_crc;tool_dialog(t,2);return;
    }
    if(status) {
        tool_status(t,status==DESK_FILE_CHANGED?"The file changed on disk. Save to a different path to keep your edits.":
            status==DESK_FILE_TOO_LARGE?"This file exceeds the 64 KiB text limit, or is not a regular file.":
            "The file operation failed. Check the path and that the volume is writable.");
        cancel_pending(t);tool_layout(t);return;
    }
    if(operation==DESK_FILE_READ) {
        tool_text_search_close(t);
        if(desk_text_buffer_set(&t->body_text,(const char *)t->job.incoming,t->job.response.length)<0) {
            tool_status(t,"This file is not supported UTF-8 text. The current document was kept.");cancel_pending(t);tool_layout(t);return;
        }
        memcpy(t->saved_data,t->job.incoming,t->job.response.length);t->saved_length=t->job.response.length;
        t->saved_crc=desk_file_crc(t->saved_data,t->saved_length);
        strcpy(t->saved_path,t->job.request.path);desk_text_buffer_set(&t->path_text,t->saved_path,strlen(t->saved_path));
        t->body.scroll_x=t->body.scroll_y=0;tool_status(t,"Document opened.");
    } else if(operation==DESK_FILE_WRITE) {
        memcpy(t->saved_data,t->job.outgoing,t->job.request.length);t->saved_length=t->job.request.length;
        t->saved_crc=desk_file_crc(t->saved_data,t->saved_length);strcpy(t->saved_path,t->job.request.path);
        tool_status(t,tool_modified(t)?"Saved. There are newer edits still in this window.":"Saved and verified on disk.");
    }
    t->saved_data[t->saved_length]=0;t->title_dirty=1;tool_layout(t);
    if(t->closing) { t->closing=0;tool_text_request(t,TOOL_PENDING_CLOSE); }
    else if(t->pending) {
        if(tool_modified(t)) tool_dialog(t,1);else continue_action(t);
    }
    desk_ui_focus(&t->ui,TOOL_BODY);
}
