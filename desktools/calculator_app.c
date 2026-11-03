#include "app.h"
#include "calculator.h"
#include <srvros/desk_log.h>
#include <stdio.h>
#include <string.h>
static const char *keys[]={"(",")","Back","Ans","7","8","9","÷","4","5","6","×","1","2","3","−","0",".","±","+"};
static const char *tokens[]={"(",")",NULL,NULL,"7","8","9","/","4","5","6","*","1","2","3","-","0",".",NULL,"+"};
static void rows(struct tool *t) {
    for(int i=0;i<t->calc_count;i++) t->calc_rows[i]=(struct desk_table_row){.id=t->calc_ids[i],.cells={t->calc_expressions[i],t->calc_results[i]}};
    desk_table_rows(&t->calc_history,t->calc_rows,t->calc_count);
}
static void insert(struct tool *t,const char *text,int replace) {
    size_t anchor=t->body_text.anchor,caret=t->body_text.caret;
    if(replace) desk_text_buffer_select(&t->body_text,0,t->body_text.length);
    if(desk_text_buffer_insert(&t->body_text,text,strlen(text))<0) {
        desk_text_buffer_select(&t->body_text,anchor,caret);tool_status(t,"The expression is too long.");return;
    }
    t->calc_just_evaluated=0;desk_edit_reveal(&t->body);t->window.dirty=1;
    tool_status(t,"Edited. Enter evaluates.");
}
static void evaluate(struct tool *t) {
    double result;const char *error=desk_calculate(t->body_text.data,&result);
    if(error) { tool_status(t,error);t->calc_just_evaluated=0;desk_log("desktools: calculator error=%s\n",error);return; }
    snprintf(t->calc_result,sizeof(t->calc_result),"%.12g",result);t->calc_valid=t->calc_just_evaluated=1;
    int duplicate=t->calc_count && !strcmp(t->calc_expressions[0],t->body_text.data) && !strcmp(t->calc_results[0],t->calc_result);
    if(!duplicate) {
        int count=t->calc_count<CALC_HISTORY_LIMIT?t->calc_count:CALC_HISTORY_LIMIT-1;
        memmove(t->calc_expressions+1,t->calc_expressions,(size_t)count*sizeof(t->calc_expressions[0]));
        memmove(t->calc_results+1,t->calc_results,(size_t)count*sizeof(t->calc_results[0]));
        memmove(t->calc_ids+1,t->calc_ids,(size_t)count*sizeof(t->calc_ids[0]));
        snprintf(t->calc_expressions[0],sizeof(t->calc_expressions[0]),"%s",t->body_text.data);
        strcpy(t->calc_results[0],t->calc_result);t->calc_ids[0]=++t->calc_next_id;t->calc_count=count+1;rows(t);
    }
    desk_text_buffer_select(&t->body_text,0,t->body_text.length);desk_edit_reveal(&t->body);
    tool_calculator_layout(t);desk_ui_focus(&t->ui,TOOL_BODY);
    tool_status(t,"Result ready. Type to start again.");
    desk_log("desktools: calculator result=%s history=%d\n",t->calc_result,t->calc_count);
}
static void token(struct tool *t,const char *text) {
    int selected=t->body_text.caret!=t->body_text.anchor &&
        (t->body_text.caret==0 || t->body_text.anchor==0) &&
        (t->body_text.caret==t->body_text.length || t->body_text.anchor==t->body_text.length);
    if(t->calc_just_evaluated && selected && text[0] && !text[1] && strchr("+-*/",text[0])) {
        char expression[80];snprintf(expression,sizeof(expression),"(%s)%s",t->calc_result,text);insert(t,expression,1);
    } else insert(t,text,0);
}
void tool_calculator_init(struct tool *t) {
    desk_table_init(&t->calc_history);
    static const struct desk_table_column columns[]={{"Expression",0,0,0},{"Result",0,1,0}};
    desk_table_columns(&t->calc_history,columns,2);
    desk_ui_add(&t->ui,TOOL_BODY,DESK_CONTROL_EDIT,&t->body,NULL);
    const unsigned actions[]={CALC_EVALUATE,CALC_CLEAR,CALC_USE,CALC_COPY};
    const char *labels[]={"Calculate","Clear","Use result","Copy"};
    for(unsigned i=0;i<4;i++) { desk_ui_add(&t->ui,actions[i],DESK_CONTROL_BUTTON,&t->buttons[i],labels[i]);desk_button_centered(&t->buttons[i],1); }
    for(unsigned i=0;i<20;i++) { desk_ui_add(&t->ui,CALC_KEY_FIRST+i,DESK_CONTROL_BUTTON,&t->buttons[i+4],keys[i]);desk_button_centered(&t->buttons[i+4],1); }
    desk_ui_add(&t->ui,CALC_HISTORY,DESK_CONTROL_TABLE,&t->calc_history,NULL);
    tool_status(t,"Enter an expression, or use the keypad. Enter evaluates.");
}
void tool_calculator_layout(struct tool *t) {
    int w=t->window.width,h=t->window.height;
    desk_edit_configure(&t->body,(struct desk_rect){16,56,w-32,36},&desk_font_20,1);
    desk_button_configure(&t->buttons[0],(struct desk_rect){w-204,18,100,28},1);
    desk_button_configure(&t->buttons[1],(struct desk_rect){w-96,18,80,28},1);
    desk_button_configure(&t->buttons[2],w>=440?(struct desk_rect){w-204,104,96,28}:(struct desk_rect){0},w>=440 && t->calc_valid);
    desk_button_configure(&t->buttons[3],(struct desk_rect){w-100,104,84,28},t->calc_valid);
    int visible=h>=366,history=visible && w>=560,width=history?w/2-24:w-32;
    int row=(h-194-32)/5;if(row>48) row=48;if(row<28) row=28;
    t->calc_keys=visible?(struct desk_rect){16,150,width,5*row+32}:(struct desk_rect){0};
    for(unsigned i=0;i<20;i++) {
        int col=i%4,y=i/4,left=(width+8)*col/4,right=(width+8)*(col+1)/4-8;
        struct desk_rect r={16+left,150+y*(row+8),right-left,row};
        desk_button_configure(&t->buttons[i+4],visible?r:(struct desk_rect){0},visible && (i!=3 || t->calc_valid));
    }
    desk_table_configure(&t->calc_history,history?(struct desk_rect){32+width,150,w-width-48,h-194}:(struct desk_rect){0},history);
    desk_ui_layout_changed(&t->ui);t->window.dirty=1;
}
void tool_calculator_action(struct tool *t,struct desk_action a) {
    if(a.id==TOOL_BODY && (a.flags&DESK_WIDGET_CHANGED)) { t->calc_just_evaluated=0;tool_status(t,"Enter evaluates the expression."); }
    if(a.flags&DESK_EDIT_FULL) tool_status(t,"The expression has reached its 255-byte limit.");
    if(a.id==TOOL_BODY && (a.flags&DESK_EDIT_SUBMIT)) { evaluate(t);return; }
    if(!(a.flags&DESK_WIDGET_ACTIVATED)) return;
    if(a.id==CALC_EVALUATE) evaluate(t);
    else if(a.id==CALC_CLEAR) {
        insert(t,"",1);t->calc_valid=0;t->calc_result[0]=0;tool_calculator_layout(t);desk_ui_focus(&t->ui,TOOL_BODY);tool_status(t,"Ready.");
    } else if(a.id==CALC_USE && t->calc_valid) {
        insert(t,t->calc_result,1);desk_ui_focus(&t->ui,TOOL_BODY);
    } else if(a.id==CALC_COPY && t->calc_valid) {
        if(t->clip.mode) { tool_status(t,"The clipboard is still transferring.");return; }
        if(desk_clipboard_copy(&t->clip,t->window.server,t->calc_result,strlen(t->calc_result),srv_ticks())) tool_status(t,"The clipboard is unavailable.");
        else { t->clip_action=DESK_EDIT_COPY;t->clip_target=CALC_COPY;tool_status(t,"Copying result…"); }
    } else if(a.id==CALC_HISTORY) {
        int i=t->calc_history.body.selected;if(i<0 || i>=t->calc_count) return;
        insert(t,t->calc_expressions[i],1);strcpy(t->calc_result,t->calc_results[i]);t->calc_valid=1;
        tool_calculator_layout(t);desk_ui_focus(&t->ui,TOOL_BODY);
        tool_status(t,"Recalled expression. Edit it or press Enter to evaluate.");
        desk_log("desktools: calculator recalled=%llu\n",(unsigned long long)t->calc_ids[i]);
    } else if(a.id>=CALC_KEY_FIRST && a.id<CALC_KEY_FIRST+20) {
        unsigned i=a.id-CALC_KEY_FIRST;
        if(tokens[i]) token(t,tokens[i]);
        else if(i==2) { desk_text_buffer_erase(&t->body_text,1);t->calc_just_evaluated=0;desk_edit_reveal(&t->body);t->window.dirty=1; }
        else if(i==3 && t->calc_valid) token(t,t->calc_result);
        else if(i==18) {
            char expression[CALC_EXPRESSION_BYTES];
            if(t->body_text.length+3>=sizeof(expression)) tool_status(t,"The expression is too long to negate.");
            else if(t->body_text.length) { snprintf(expression,sizeof(expression),"-(%s)",t->body_text.data);insert(t,expression,1); }
            else token(t,"-");
        }
        desk_ui_focus(&t->ui,TOOL_BODY);
    }
}
int tool_calculator_key(struct tool *t,const struct srv_key_event *k) {
    if(!t->ui.active || !t->body.enabled || !(k->flags&SRV_KEY_DOWN_EVENT) || (k->modifiers&(SRV_KEY_CTRL|SRV_KEY_META|SRV_KEY_ALT)) ||
       t->ui.focus<0 || t->ui.controls[t->ui.focus].id!=TOOL_BODY) return 0;
    if(k->codepoint && k->codepoint<128 && strchr("+-*/",(char)k->codepoint) && t->calc_just_evaluated) {
        char text[2]={(char)k->codepoint,0};token(t,text);return 1;
    }
    if(k->usage==SRV_KEY_LEFT || k->usage==SRV_KEY_RIGHT || k->usage==SRV_KEY_HOME || k->usage==SRV_KEY_END) t->calc_just_evaluated=0;
    return 0;
}
void tool_calculator_draw(struct tool *t,struct desk_canvas *c) {
    const struct desk_theme *theme=&desk_theme_quiet;
    desk_label_draw(c,(struct desk_rect){16,18,c->width-232,28},theme->heading,c->width<400?"Calc":"Calculator",theme->text);
    char label[80];snprintf(label,sizeof(label),"%s%s",t->calc_valid?(t->calc_just_evaluated?"= ":"Last: "):"",t->calc_valid?t->calc_result:"Result");
    desk_label_draw(c,(struct desk_rect){16,104,c->width-(c->width>=440?232:128),28},theme->heading,label,t->calc_valid?theme->text:theme->muted);
    if(!t->calc_keys.h) desk_text_block_draw(c,(struct desk_rect){16,154,c->width-32,c->height-194},theme->body,
        "Enter evaluates.\nEnlarge for the keypad.",theme->muted);
}
