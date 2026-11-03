#include "runtime.h"
#include <string.h>
#include <srvros/desk_log.h>
#include <srvros/desk_commands.h>
enum { MENU_CLOSE=100,MENU_LOCATION,MENU_NEW_WINDOW,MENU_NEW_FOLDER,MENU_STANDARD,MENU_LARGER,MENU_SAVE_AS,MENU_QUIT,MENU_EDIT=200 };
static unsigned text_commands(const struct tool *t,struct desk_command commands[7]) {
    unsigned blocked=(t->modal || t->picker_mode || t->prompt_mode || t->info_mode)?DESK_MENU_DISABLED:0;
    unsigned busy=blocked|(t->job.busy?DESK_MENU_DISABLED:0);
    const struct desk_command values[]={
        {TOOL_NEW,busy,0x11,SRV_KEY_CTRL,"New document","Ctrl-N"},
        {TOOL_OPEN,busy,0x12,SRV_KEY_CTRL,"Open…","Ctrl-O"},
        {TOOL_SAVE,busy,0x16,SRV_KEY_CTRL,"Save","Ctrl-S"},
        {MENU_SAVE_AS,busy,0x16,SRV_KEY_CTRL|SRV_KEY_SHIFT,"Save as…","Ctrl-Shift-S"},
        {MENU_NEW_WINDOW,blocked|(!t->window.connection?DESK_MENU_DISABLED:0),0x11,SRV_KEY_CTRL|SRV_KEY_SHIFT,"New window","Ctrl-Shift-N"},
        {MENU_CLOSE,blocked,0x1a,SRV_KEY_CTRL,"Close window","Ctrl-W"},
        {MENU_QUIT,0,0x14,SRV_KEY_CTRL,"Quit Text","Ctrl-Q"}};
    memcpy(commands,values,sizeof(values));return 7;
}
static unsigned files_commands(const struct tool *t,struct desk_command commands[7]) {
    unsigned blocked=(t->modal || t->picker_mode || t->prompt_mode || t->info_mode)?DESK_MENU_DISABLED:0;
    const struct desk_command values[]={
        {MENU_NEW_WINDOW,blocked|(!t->window.connection?DESK_MENU_DISABLED:0),0x11,SRV_KEY_CTRL|SRV_KEY_SHIFT,"New window","Ctrl-Shift-N"},
        {MENU_CLOSE,blocked,0x1a,SRV_KEY_CTRL,"Close window","Ctrl-W"},
        {MENU_QUIT,0,0x14,SRV_KEY_CTRL,"Quit Files","Ctrl-Q"}};
    memcpy(commands,values,sizeof(values));return 3;
}
static unsigned standard_commands(const struct tool *t,struct desk_command commands[7]) {
    return t->role==DESK_APP_TEXT?text_commands(t,commands):files_commands(t,commands);
}
static void text_command(void *context,unsigned id) {
    struct tool *t=context;
    if(id==MENU_QUIT) {
        if(t->role==DESK_APP_FILES)tool_runtime_cancel_modal(t);
        else if(t->picker_mode)tool_picker_cancel(t);else if(t->modal)tool_text_dialog(t,1003);
        t->quit_application=1;
    } else if(id==MENU_CLOSE) {
        if(t->role==DESK_APP_FILES)tool_runtime_close_request(t);else tool_text_request(t,TOOL_PENDING_CLOSE);
    }
    else if(id==MENU_NEW_WINDOW)tool_new_window(t);
    else if(id==MENU_SAVE_AS)tool_text_save(t,1);
    else tool_text_action(t,(struct desk_action){id,DESK_WIDGET_ACTIVATED});
}
int tool_command_action(struct tool *t,unsigned id) {
    if(t->role!=DESK_APP_TEXT && t->role!=DESK_APP_FILES)return 0;
    struct desk_command commands[7];unsigned count=standard_commands(t,commands);
    for(unsigned i=0;i<count;i++)if(commands[i].id==id) {
        desk_commands_invoke(commands,count,id,text_command,t);return 1;
    }
    return 0;
}
int tool_command_key(struct tool *t,const struct srv_key_event *key) {
    if(t->role!=DESK_APP_TEXT && t->role!=DESK_APP_FILES)return 0;
    struct desk_command commands[7];unsigned count=standard_commands(t,commands);
    unsigned id=desk_commands_key(commands,count,key);
    return id?desk_commands_invoke(commands,count,id,text_command,t):0;
}
static struct desk_edit *edit_target(struct tool *t,unsigned *id) {
    if(t->modal || t->picker_mode || t->prompt_mode || t->info_mode || t->ui.focus<0 || t->ui.focus>=(int)t->ui.count) return NULL;
    if(t->search_open && t->search_focus) {
        if(t->search.ui.focus<0 || t->search.ui.focus>=(int)t->search.ui.count) return NULL;
        struct desk_control *control=&t->search.ui.controls[t->search.ui.focus];
        if(control->kind!=DESK_CONTROL_EDIT || !((struct desk_edit *)control->state)->enabled) return NULL;
        *id=control->id==DESK_SEARCH_REPLACEMENT_FIELD?TOOL_REPLACEMENT:TOOL_FIND_QUERY;return control->state;
    }
    struct desk_control *c=&t->ui.controls[t->ui.focus];
    if(c->kind!=DESK_CONTROL_EDIT || !((struct desk_edit *)c->state)->enabled) return NULL;
    *id=c->id;return c->state;
}
static void append(struct desk_menu_item *items,unsigned *count,unsigned id,const char *label,const char *shortcut,unsigned flags) {
    struct desk_menu_item *m=&items[(*count)++];m->id=id;m->flags=flags;
    strcpy(m->label,label);if(shortcut) strcpy(m->shortcut,shortcut);
}
void tool_menu_refresh(struct tool *t) {
    struct desk_menu_item items[DESK_APP_MENU_ITEMS]={0};unsigned count=0;
    unsigned blocked=(t->modal || t->picker_mode || t->prompt_mode || t->info_mode)?DESK_MENU_DISABLED:0,busy=blocked|(t->job.busy?DESK_MENU_DISABLED:0);
    append(items,&count,0,"File",NULL,DESK_MENU_HEADING);
    if(t->role==DESK_APP_TEXT) {
        struct desk_command commands[7];text_commands(t,commands);
        for(unsigned i=0;i<(t->window.connection?5u:4u);i++)desk_command_menu(&commands[i],&items[count++]);
    } else {
        if(t->role==DESK_APP_FILES) {
            struct desk_command commands[7];files_commands(t,commands);desk_command_menu(&commands[0],&items[count++]);
        } else append(items,&count,MENU_NEW_WINDOW,"New window",NULL,blocked);
        if(t->role==DESK_APP_FILES) {
            append(items,&count,TOOL_OPEN,"Open selected item","Enter",busy|(t->table.body.selected<0?DESK_MENU_DISABLED:0));
            append(items,&count,MENU_NEW_FOLDER,"New folder…",NULL,busy);
            unsigned selected=busy|(t->table.body.selected<0?DESK_MENU_DISABLED:0);
            append(items,&count,TOOL_INFO,"Information…","Ctrl-I",selected);
            append(items,&count,TOOL_COPY_PATH,"Copy path",NULL,selected|(t->clip.mode?DESK_MENU_DISABLED:0));
            append(items,&count,TOOL_RENAME,"Rename…","F2",selected);
            append(items,&count,TOOL_DUPLICATE,"Duplicate…",NULL,selected);
            append(items,&count,TOOL_DELETE,"Delete…","Delete",selected);
        }
    }
    append(items,&count,0,"",NULL,DESK_MENU_SEPARATOR);
    if(t->role==DESK_APP_TEXT) {
        struct desk_command commands[7];text_commands(t,commands);
        desk_command_menu(&commands[5],&items[count++]);desk_command_menu(&commands[6],&items[count++]);
    } else if(t->role==DESK_APP_FILES) {
        struct desk_command commands[7];files_commands(t,commands);
        desk_command_menu(&commands[1],&items[count++]);desk_command_menu(&commands[2],&items[count++]);
    } else append(items,&count,MENU_CLOSE,"Close window","Ctrl-W",blocked);
    if(t->role==DESK_APP_TEXT || t->role==DESK_APP_FILES || t->role==DESK_APP_CALCULATOR || t->role==DESK_APP_SETTINGS) {
        unsigned id=0;struct desk_edit *e=edit_target(t,&id);struct desk_text_buffer *text=e?e->text:NULL;
        unsigned disabled=blocked|(!e?DESK_MENU_DISABLED:0),selection=disabled|(!text || text->anchor==text->caret?DESK_MENU_DISABLED:0);
        append(items,&count,0,"Edit",NULL,DESK_MENU_HEADING);
        append(items,&count,MENU_EDIT+DESK_EDIT_UNDO,"Undo","Ctrl-Z",disabled|(!desk_text_buffer_can_undo(text,0)?DESK_MENU_DISABLED:0));
        append(items,&count,MENU_EDIT+DESK_EDIT_REDO,"Redo","Ctrl-Shift-Z",disabled|(!desk_text_buffer_can_undo(text,1)?DESK_MENU_DISABLED:0));
        append(items,&count,0,"",NULL,DESK_MENU_SEPARATOR);
        append(items,&count,MENU_EDIT+DESK_EDIT_COMMAND_CUT,"Cut","Ctrl-X",selection|(t->clip.mode?DESK_MENU_DISABLED:0));
        append(items,&count,MENU_EDIT+DESK_EDIT_COMMAND_COPY,"Copy","Ctrl-C",selection|(t->clip.mode?DESK_MENU_DISABLED:0));
        append(items,&count,MENU_EDIT+DESK_EDIT_COMMAND_PASTE,"Paste","Ctrl-V",disabled|(t->clip.mode?DESK_MENU_DISABLED:0));
        append(items,&count,0,"",NULL,DESK_MENU_SEPARATOR);
        append(items,&count,MENU_EDIT+DESK_EDIT_SELECT_ALL,"Select all","Ctrl-A",disabled);
    }
    if(t->role==DESK_APP_TEXT) {
        append(items,&count,0,"Search",NULL,DESK_MENU_HEADING);
        append(items,&count,TOOL_FIND,"Find…","Ctrl-F",busy);
        append(items,&count,TOOL_REPLACE,"Find and replace…","Ctrl-H",busy);
        append(items,&count,TOOL_FIND_NEXT,"Find next","Ctrl-G",busy);
        append(items,&count,TOOL_FIND_PREVIOUS,"Find previous","Ctrl-Shift-G",busy);
        append(items,&count,TOOL_FIND_CLOSE,"Close search","Esc",blocked|(!t->search_open?DESK_MENU_DISABLED:0));
    } else if(t->role==DESK_APP_FILES) {
        append(items,&count,0,"Go",NULL,DESK_MENU_HEADING);
        append(items,&count,TOOL_BACK,"Back","Alt-Left",busy|(t->location_index<=0?DESK_MENU_DISABLED:0));
        append(items,&count,TOOL_FORWARD,"Forward","Alt-Right",busy|(t->location_index+1>=t->location_count?DESK_MENU_DISABLED:0));
        append(items,&count,TOOL_HOME,"Home","Alt-Home",busy);
        append(items,&count,TOOL_VOLUME,"Startup disk",NULL,busy);
        append(items,&count,TOOL_ROOT,"System root",NULL,busy);
        append(items,&count,TOOL_UP,"Parent folder","Alt-Up",busy);
        append(items,&count,MENU_LOCATION,"Location…","Ctrl-L",busy);
        append(items,&count,TOOL_REFRESH,"Refresh","Ctrl-R",busy);
        append(items,&count,TOOL_FIND,"Find in this folder…","Ctrl-F",busy);
    } else if(t->role==DESK_APP_CALCULATOR) {
        append(items,&count,0,"Calculate",NULL,DESK_MENU_HEADING);
        append(items,&count,CALC_EVALUATE,"Evaluate","Enter",blocked);append(items,&count,CALC_CLEAR,"Clear",NULL,blocked);
        append(items,&count,CALC_USE,"Use result",NULL,blocked|(!t->calc_valid?DESK_MENU_DISABLED:0));
        append(items,&count,CALC_COPY,"Copy result",NULL,blocked|(!t->calc_valid || t->clip.mode?DESK_MENU_DISABLED:0));
    } else if(t->role==DESK_APP_SYSTEM) {
        append(items,&count,0,"View",NULL,DESK_MENU_HEADING);
        append(items,&count,SYSTEM_PAUSE,t->system_paused?"Resume updates":"Pause updates",NULL,blocked);
        append(items,&count,SYSTEM_REFRESH,"Refresh now","F5",blocked);
        append(items,&count,SYSTEM_DIAGNOSTICS,"Network Utility…",NULL,blocked);
        append(items,&count,0,"",NULL,DESK_MENU_SEPARATOR);
        append(items,&count,SYSTEM_PROCESSES,"Processes",NULL,blocked|(desk_tabs_selected(&t->system_tabs)==SYSTEM_PROCESSES?DESK_MENU_CHECKED:0));
        append(items,&count,SYSTEM_NETWORK,"Network",NULL,blocked|(desk_tabs_selected(&t->system_tabs)==SYSTEM_NETWORK?DESK_MENU_CHECKED:0));
        append(items,&count,SYSTEM_DEVICES,"Devices",NULL,blocked|(desk_tabs_selected(&t->system_tabs)==SYSTEM_DEVICES?DESK_MENU_CHECKED:0));
        append(items,&count,SYSTEM_SENSORS,"Sensors",NULL,blocked|(desk_tabs_selected(&t->system_tabs)==SYSTEM_SENSORS?DESK_MENU_CHECKED:0));
    } else if(t->role==DESK_APP_SETTINGS) {
        append(items,&count,0,"View",NULL,DESK_MENU_HEADING);
        append(items,&count,MENU_STANDARD,"Standard text",NULL,blocked|(t->body_size==16?DESK_MENU_CHECKED:0));
        append(items,&count,MENU_LARGER,"Larger text",NULL,blocked|(t->body_size==20?DESK_MENU_CHECKED:0));
    }
    desk_app_menus(&t->window,items,count);
}
void tool_menu_action(struct tool *t,unsigned id) {
    if(tool_command_action(t,id))return;
    if(t->modal || t->picker_mode || t->prompt_mode || t->info_mode) return;
    if(id>=MENU_EDIT && id<=MENU_EDIT+DESK_EDIT_REDO) {
        unsigned target=0;struct desk_edit *e=edit_target(t,&target);
        if(e) tool_dispatch(t,(struct desk_action){target,desk_edit_command(e,id-MENU_EDIT)});
    } else if(id==MENU_QUIT && t->role==DESK_APP_TEXT) {t->quit_application=1;
    } else if(id==MENU_CLOSE) {
        if(t->role==DESK_APP_TEXT) tool_text_request(t,TOOL_PENDING_CLOSE);else t->closing=1;
    } else if(id==MENU_SAVE_AS) tool_text_save(t,1);
    else if(id==MENU_NEW_WINDOW) tool_new_window(t);
    else if(id==MENU_LOCATION || id==MENU_NEW_FOLDER) {
        if(t->job.busy) return;
        unsigned target=id==MENU_LOCATION?TOOL_PATH:TOOL_NAME;
        struct desk_text_buffer *text=id==MENU_LOCATION?&t->path_text:&t->name_text;
        desk_ui_focus(&t->ui,target);desk_text_buffer_select(text,0,text->length);t->window.dirty=1;
        if(t->role==DESK_APP_TEXT) tool_status(t,"Enter a file path, then press Enter to open it.");
    } else if(id==MENU_STANDARD || id==MENU_LARGER) tool_dispatch(t,(struct desk_action){id==MENU_STANDARD?1:2,DESK_WIDGET_ACTIVATED});
    else tool_dispatch(t,(struct desk_action){id,DESK_WIDGET_ACTIVATED});
}


void tool_context_result(struct tool *t,struct desk_menu_result result) {
    if(result.redraw) t->window.dirty=1;
    if(result.action) { desk_log("desktools: context action=%u role=%d\n",result.action,t->role);tool_menu_action(t,result.action); }
}
void tool_context_open(struct tool *t,int x,int y,int keyboard) {
    if(t->modal || t->picker_mode || t->prompt_mode || t->info_mode || t->closing) return;
    int files=0;unsigned target=0;
    if(keyboard) {
        if(t->role==DESK_APP_FILES && t->ui.focus>=0 && t->ui.controls[t->ui.focus].id==TOOL_LIST) files=1;
        else edit_target(t,&target);
        if(!target && !files && t->role==DESK_APP_FILES) files=1;
        if(files) {
            struct desk_rect row=desk_list_row_rect(&t->table.body,t->table.body.selected);
            x=t->table.body.frame.x+24;y=row.h>0?row.y+row.h:t->table.body.frame.y+16;
        }
        else if(target) {
            struct desk_edit *e=edit_target(t,&target);x=e->frame.x+16;y=e->frame.y+24;
        }
    } else {
        if(t->role==DESK_APP_FILES && desk_widget_contains(t->table.body.frame,x,y)) {
            if(t->job.busy || !t->table.body.enabled) return;
            int index=desk_list_index_at(&t->table.body,x,y);
            if(desk_widget_contains(t->table.body.scroll.track,x,y)) return;
            desk_list_select(&t->table.body,index);t->table.header_focus=0;desk_ui_focus(&t->ui,TOOL_LIST);files=1;
        } else if(t->search_open && desk_widget_contains(t->search.query.frame,x,y) && t->search.query.enabled) {
            t->search_focus=1;tool_content_focus(t,t->window.focused);target=TOOL_FIND_QUERY;desk_ui_focus(&t->search.ui,DESK_SEARCH_QUERY_FIELD);
        } else if(t->search_open && t->search.replacing && desk_widget_contains(t->search.with.frame,x,y) && t->search.with.enabled) {
            t->search_focus=1;tool_content_focus(t,t->window.focused);target=TOOL_REPLACEMENT;desk_ui_focus(&t->search.ui,DESK_SEARCH_REPLACEMENT_FIELD);
        } else for(unsigned i=0;i<t->ui.count;i++) if(t->ui.controls[i].kind==DESK_CONTROL_EDIT) {
            struct desk_edit *e=t->ui.controls[i].state;
            if(e->enabled && desk_widget_contains(e->frame,x,y)) { t->search_focus=0;tool_content_focus(t,t->window.focused);target=t->ui.controls[i].id;desk_ui_focus(&t->ui,target);break; }
        }
    }
    if(!target && !files) return;
    struct desk_menu_item items[DESK_POPUP_ITEMS]={0};unsigned count=0;
    if(files) {
        if(t->job.busy) return;
        int index=t->table.body.selected,selected=index>=0 && index<t->file_count;
        unsigned disabled=selected?0:DESK_MENU_DISABLED;
        append(items,&count,TOOL_OPEN,"Open","Enter",disabled);
        append(items,&count,TOOL_INFO,"Information…","Ctrl-I",disabled);
        append(items,&count,TOOL_COPY_PATH,"Copy path",NULL,disabled|(t->clip.mode?DESK_MENU_DISABLED:0));
        append(items,&count,TOOL_RENAME,"Rename…","F2",disabled);
        append(items,&count,TOOL_DUPLICATE,"Duplicate…",NULL,disabled|(selected && t->entries[index].directory?DESK_MENU_DISABLED:0));
        append(items,&count,TOOL_DELETE,"Delete…","Delete",disabled);
        append(items,&count,0,"",NULL,DESK_MENU_SEPARATOR);
        append(items,&count,MENU_NEW_FOLDER,"New folder…",NULL,0);append(items,&count,TOOL_REFRESH,"Refresh","Ctrl-R",0);
    } else {
        struct desk_edit *e=edit_target(t,&target);if(!e) return;
        struct desk_text_buffer *text=e->text;unsigned selection=text->caret==text->anchor?DESK_MENU_DISABLED:0,busy=t->clip.mode?DESK_MENU_DISABLED:0;
        append(items,&count,MENU_EDIT+DESK_EDIT_UNDO,"Undo","Ctrl-Z",desk_text_buffer_can_undo(text,0)?0:DESK_MENU_DISABLED);
        append(items,&count,MENU_EDIT+DESK_EDIT_REDO,"Redo","Ctrl-Shift-Z",desk_text_buffer_can_undo(text,1)?0:DESK_MENU_DISABLED);
        append(items,&count,0,"",NULL,DESK_MENU_SEPARATOR);
        append(items,&count,MENU_EDIT+DESK_EDIT_COMMAND_CUT,"Cut","Ctrl-X",selection|busy);
        append(items,&count,MENU_EDIT+DESK_EDIT_COMMAND_COPY,"Copy","Ctrl-C",selection|busy);
        append(items,&count,MENU_EDIT+DESK_EDIT_COMMAND_PASTE,"Paste","Ctrl-V",busy);
        append(items,&count,0,"",NULL,DESK_MENU_SEPARATOR);
        append(items,&count,MENU_EDIT+DESK_EDIT_SELECT_ALL,"Select all","Ctrl-A",0);
    }
    desk_ui_pointer(&t->ui,(struct desk_widget_event){.type=DESK_WIDGET_CANCEL},0);
    if(!desk_popup_open(&t->context,items,count,x,y,(struct desk_rect){0,0,t->window.width,t->window.height},&desk_font_16,keyboard)) {
        t->window.dirty=1;desk_log("desktools: context opened role=%d target=%u selected=%d\n",t->role,files?TOOL_LIST:target,t->table.body.selected);
    }
}
