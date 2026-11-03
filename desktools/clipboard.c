#include "app.h"
#include <srvros/sys.h>
#include <srvros/desk_log.h>
#include <string.h>
#include <stdio.h>
static struct desk_edit *target(struct tool *t,unsigned id) {
    if(t->info_mode) return NULL;
    if(id==TOOL_PROMPT) return t->prompt_mode?&t->prompt.edit:NULL;
    if(t->prompt_mode) return NULL;
    if(id==TOOL_PICK_LOCATION || id==TOOL_PICK_NAME) return t->picker_mode?(id==TOOL_PICK_LOCATION?&t->picker.location:&t->picker.name):NULL;
    if(t->picker_mode) return NULL;
    if(id==TOOL_REPLACEMENT) return t->search_open && t->search_focus && t->search.replacing?&t->search.with:NULL;
    if(id==TOOL_FIND_QUERY) return t->search_open && t->search_focus?&t->search.query:NULL;
    if(t->search_open && t->search_focus) return NULL;
    return id==TOOL_BODY?&t->body:id==TOOL_PATH?&t->path:id==TOOL_NAME?&t->name:NULL;
}
void tool_clipboard_action(struct tool *t,struct desk_action a) {
    unsigned operation=a.flags&(DESK_EDIT_COPY|DESK_EDIT_CUT|DESK_EDIT_PASTE);
    if(!operation) return;
    struct desk_edit *edit=target(t,a.id);if(!edit) return;
    if(t->clip.mode) { tool_status(t,"The clipboard is still transferring. Try again in a moment.");return; }
    struct desk_text_buffer *text=edit->text;
    size_t lo=text->caret<text->anchor?text->caret:text->anchor,hi=text->caret>text->anchor?text->caret:text->anchor;
    if(operation!=DESK_EDIT_PASTE && hi==lo) return;
    int result=operation==DESK_EDIT_PASTE?desk_clipboard_paste(&t->clip,t->window.server,srv_ticks()):
        desk_clipboard_copy(&t->clip,t->window.server,text->data+lo,hi-lo,srv_ticks());
    if(result) { tool_status(t,"The clipboard is unavailable. Your text was kept.");return; }
    t->clip_action=operation;t->clip_target=a.id;t->clip_revision=text->revision;
    t->clip_caret=text->caret;t->clip_anchor=text->anchor;
}
void tool_clipboard_result(struct tool *t,int result) {
    unsigned operation=t->clip_action;t->clip_action=0;
    if(operation==DESK_EDIT_COPY && t->clip_target==TOOL_INFO) {
        snprintf(t->info_notice,sizeof(t->info_notice),"%s",result>0?"Copied.":"Copy did not finish. Try again.");
        tool_files_info_layout(t);return;
    }
    if(operation==DESK_EDIT_COPY && t->clip_target==TOOL_COPY_PATH) { tool_status(t,result>0?"Path copied.":"Copy did not finish. Try again.");return; }
    if(result<0) { tool_status(t,"Clipboard transfer did not finish. Your text was kept; try again.");return; }
    desk_log("desktools: clipboard role=%d action=%u bytes=%u\n",t->role,operation,t->clip.length);
    if(operation==DESK_EDIT_COPY) { if(t->role==DESK_APP_CALCULATOR && t->clip_target==CALC_COPY) tool_status(t,"Result copied.");return; }
    struct desk_edit *edit=target(t,t->clip_target);if(!edit) return;
    struct desk_text_buffer *text=edit->text;
    if(!edit->enabled || t->modal || text->revision!=t->clip_revision || text->caret!=t->clip_caret || text->anchor!=t->clip_anchor) {
        tool_status(t,"The selection changed during transfer. Repeat Cut or Paste when ready.");return;
    }
    if(operation==DESK_EDIT_PASTE && !edit->multiline &&
       (memchr(t->clip.buffer,'\n',t->clip.length) || memchr(t->clip.buffer,'\r',t->clip.length))) {
        tool_status(t,"This field accepts one line of text.");return;
    }
    if(operation==DESK_EDIT_PASTE && !t->clip.length) return;
    const char *data=operation==DESK_EDIT_CUT?"":(const char *)t->clip.buffer;
    size_t length=operation==DESK_EDIT_CUT?0:t->clip.length;
    if(desk_text_buffer_insert(text,data,length)<0) {
        tool_status(t,"That text is invalid or too large for this field. Your text was kept.");return;
    }
    desk_edit_reveal(edit);t->window.dirty=1;
    if(t->role==DESK_APP_CALCULATOR) tool_calculator_action(t,(struct desk_action){t->clip_target,DESK_WIDGET_CHANGED|DESK_WIDGET_REDRAW});
    if(t->role==DESK_APP_TEXT) tool_text_action(t,(struct desk_action){t->clip_target,DESK_WIDGET_CHANGED|DESK_WIDGET_REDRAW});
}
