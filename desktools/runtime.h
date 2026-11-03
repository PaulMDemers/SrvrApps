#ifndef SRVROS_TOOL_RUNTIME_H
#define SRVROS_TOOL_RUNTIME_H
#include "app.h"
/* Read-only Files work keeps its model/binding after visual Close. Accepted
 * mutations and every Text save retain their existing completion gate. */
static inline int tool_runtime_close_ready(const struct tool *tool) {
    unsigned operation=tool->job.request.operation;
    return !tool->job.busy || (tool->role==DESK_APP_FILES &&
        (operation==DESK_FILE_READ || operation==DESK_FILE_LIST || operation==DESK_FILE_STAT));
}
/* Lazy transport creation may wait for ordinary kernel admission. Publish and
 * receive the initial Files surface release before starting queued I/O. */
static inline int tool_runtime_file_ready(const struct tool *tool) {
    return tool->role!=DESK_APP_FILES || (tool->window.serial &&
        (tool->window.serial>1 || !tool->window.pending));
}
void tool_runtime_init(struct tool *tool,int role);
void tool_runtime_event(struct tool *tool,struct desk_packet event);
int tool_runtime_file_poll(struct tool *tool,uint64_t *logged_job);
int tool_file_pool_retire(struct desk_file_pool *pool,int role);
void tool_runtime_draw(struct tool *tool);
int tool_text_runtime(const char *path);
int tool_window_runtime(int role,const char *path);
void tool_runtime_cancel_modal(struct tool *tool);
void tool_runtime_close_request(struct tool *tool);
#endif
