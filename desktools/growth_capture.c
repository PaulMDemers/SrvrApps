/* RAM-only workload for real dynamic compositor admission and enumeration. */
#include <srvros/desk_connection.h>
#include <srvros/desk_log.h>
#include <srvros/sys.h>
#include <srvros/key_event.h>
#include <stdio.h>
#include <string.h>
#define GROWTH_WINDOWS 128
struct growth_window { struct desk_app app;char text[32];unsigned length; };
static struct growth_window windows[GROWTH_WINDOWS];
static struct desk_connection connection;
static unsigned opened,retired;
static int open_window(void) {
    if(connection.opening)return -1;
    for(unsigned i=0;i<GROWTH_WINDOWS;i++)if(!windows[i].app.connection) {
        struct growth_window *w=&windows[i];memset(w,0,sizeof(*w));
        char title[64];snprintf(title,sizeof(title),"Growth %03u",opened+1);
        if(desk_window_open(&connection,&w->app,title,280,240))return -1;
        opened++;return 0;
    }
    return -1;
}
static int all_presented(void) {
    for(unsigned i=0;i<GROWTH_WINDOWS;i++)if(windows[i].app.connection &&
        (!windows[i].app.serial || windows[i].app.pending || windows[i].app.dirty))return 0;
    return !connection.opening;
}
int tool_growth_capture(void) {
    desk_connection_init(&connection);if(open_window())return 1;
    uint64_t start=srv_ticks();unsigned checkpoint=0;int ready=0,quitting=0;
    while(connection.windows && (uint64_t)srv_ticks()-start<30000) {
        for(unsigned batch=0;batch<64;batch++) {
            struct desk_app *a;struct desk_packet p;
            int r=desk_connection_poll(&connection,&a,&p);
            if(r<0)goto failure;if(!r)break;if(r==2)continue;
            struct growth_window *w=NULL;
            for(unsigned i=0;i<GROWTH_WINDOWS;i++)if(&windows[i].app==a)w=&windows[i];
            if(!w)goto failure;
            if(p.type==DESK_OPEN_REPLY && p.value)goto failure;
            if(p.type==DESK_WINDOW_FAILED)goto failure;
            if(p.type==DESK_CONFIGURE)desk_log("window-growth: configured id=%llu generation=%llu\n",(unsigned long long)a->window,(unsigned long long)a->generation);
            if(p.type==DESK_CLOSE_ACK) {
                desk_log("window-growth: retired id=%llu text=%s\n",(unsigned long long)a->window,w->text);
                if(desk_window_dispose(a))goto failure;retired++;
            } else if(p.type==DESK_CLOSE_REQUEST)desk_window_close(a);
            else if(p.type==DESK_KEY) {
                struct srv_key_event k;memcpy(&k,p.text,sizeof(k));if(!(k.flags&SRV_KEY_DOWN_EVENT))continue;
                if(k.modifiers&SRV_KEY_CTRL) {
                    if(k.usage==0x1a)desk_window_close(a);
                    else if(k.usage==0x11) { if(open_window())goto failure; }
                    else if(k.usage==0x14) {
                        quitting=1;
                        for(unsigned i=0;i<GROWTH_WINDOWS;i++)if(windows[i].app.connection)desk_window_close(&windows[i].app);
                    }
                } else if(k.codepoint>=32 && k.codepoint<127 && w->length+1<sizeof(w->text)) {
                    w->text[w->length++]=k.codepoint;w->text[w->length]=0;a->dirty=1;
                    desk_log("window-growth: input id=%llu text=%s\n",(unsigned long long)a->window,w->text);
                }
            }
        }
        for(unsigned i=0;i<GROWTH_WINDOWS;i++) {
            struct growth_window *w=&windows[i];if(!w->app.connection || w->app.closed)continue;
            struct desk_canvas canvas;
            if(desk_app_canvas(&w->app,&canvas)==1) {
                char caption[64];snprintf(caption,sizeof(caption),"Window %llu",(unsigned long long)w->app.window);
                desk_fill(&canvas,canvas.clip,0xfff6f4ef);desk_text(&canvas,&desk_font_20,16,18,caption,0xff26382c);
                desk_text(&canvas,&desk_font_16,16,62,w->text,0xff202420);
                desk_text(&canvas,&desk_font_16,16,110,"128 independent windows",0xff607064);
                if(desk_app_present(&w->app)<0)goto failure;
            }
        }
        if(!quitting && all_presented()) {
            if(opened<=GROWTH_WINDOWS && opened%32==0 && checkpoint!=opened) {
                checkpoint=opened;desk_log("window-growth: presented=%u\n",opened);
            }
            if(opened<GROWTH_WINDOWS) { if(open_window())goto failure; }
            else if(!ready) { ready=1;srv_puts("window-growth: ready 128 windows one process\n"); }
            else if(opened==129 && ready==1) { ready=2;srv_puts("window-growth: replacement129 presented\n"); }
        }
        srv_sleep_ticks(1);
    }
    if(connection.windows || !quitting || opened!=129 || retired!=129)goto failure;
    srv_puts("window-growth: PASS 129 lifetimes, all windows retired\n");return 0;
failure:
    desk_log("window-growth: FAIL opened=%u retired=%u\n",opened,retired);return 1;
}
