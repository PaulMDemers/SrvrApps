/* Actual-protocol consumer for the connection/lifetime migration. RAM only;
 * independent documents, menus and clipboard state in one process. */
#include <srvros/desk_connection.h>
#include <srvros/desk_clipboard.h>
#include <srvros/desk_log.h>
#include <srvros/sys.h>
#include <srvros/key_event.h>
#include <srvros/gui.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
struct capture_window {
    struct desk_app app;
    struct desk_clipboard_client clipboard;
    unsigned char transfer[DESK_CLIPBOARD_BYTES+1];
    char text[128];
    unsigned length;
    int pasting;
};
static struct capture_window windows[3];
static struct desk_connection connection;
static int opened,quitting;
/* Hold more surfaces than the former kernel table while the actual connection
 * exercises rendering and retirement. These are not extra desktop windows. */
static uint64_t pool[128];
static int surface_pool(void) {
    uint64_t before;
    if(gui_surface_create(1,1,0,&before) || gui_surface_destroy(before))return -1;
    struct srv_gui_surface_create *request=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(request==MAP_FAILED)return -1;
    *request=(struct srv_gui_surface_create){.abi_version=SRV_ABI_VERSION,.struct_size=sizeof(*request),.width=512,.height=512};
    if(mprotect(request,4096,PROT_READ))return -1;
    /* Output faults must not allocate unreachable 1 MiB surfaces. Repeating
     * past the byte budget detects a leak before the real surface pool opens. */
    for(unsigned i=0;i<300;i++)if(srv_syscall1(SYS_GUI_SURFACE_CREATE,(long)request)!=-1)return -1;
    if(munmap(request,4096))return -1;
    uint32_t pixels[4]={0xff010203,0xff040506,0xff070809,0xff101112},copy[4];
    for(unsigned i=0;i<128;i++)if(gui_surface_create(2,2,0,&pool[i]))return -1;
    if(pool[0]!=before+1)return -1;
    uint64_t high=pool[127];
    for(unsigned i=0;i<128;i++) {
        if(gui_surface_blit(pool[i],0,0,2,2,pixels,2) ||
           gui_surface_copy(pool[i],0,0,2,2,copy,2) || memcmp(pixels,copy,sizeof(copy)))return -1;
        if(!(i&1)) {
            uint64_t old=pool[i];
            if(gui_surface_destroy(old) || gui_surface_create(2,2,0,&pool[i]) || pool[i]<=high ||
               gui_surface_copy(old,0,0,2,2,copy,2)!=-1)return -1;
        }
    }
    srv_puts("multi-window: surface-pool 128 live, readback/reuse/stale IDs verified\n");return 0;
}
static int open_next(void) {
    if(opened==3 || connection.opening)return -1;
    struct capture_window *w=&windows[opened];char title[64];
    snprintf(title,sizeof(title),"Connection document %d",opened+1);
    if(desk_window_open(&connection,&w->app,title,420,280))return -1;
    desk_clipboard_init(&w->clipboard,w->transfer);w->clipboard.window=w->app.window;
    struct desk_menu_item menu[]={{0,DESK_MENU_HEADING,"Document",""},
        {1,0,"New window","Ctrl-N"},{2,0,"Close window","Ctrl-W"}};
    desk_app_menus(&w->app,menu,3);opened++;return 0;
}
static void close_all(void) {
    quitting=1;
    for(int i=0;i<opened;i++)desk_window_close(&windows[i].app);
}
static void paste(struct capture_window *w,int result) {
    if(!result)return;
    if(result<0) { w->pasting=0;srv_puts("multi-window: FAIL clipboard\n");return; }
    if(!w->pasting) { desk_log("multi-window: copied window=%llu\n",(unsigned long long)w->app.window);return; }
    w->pasting=0;
    unsigned n=w->clipboard.length;if(n>sizeof(w->text)-1-w->length)n=sizeof(w->text)-1-w->length;
    memcpy(w->text+w->length,w->transfer,n);w->length+=n;w->text[w->length]=0;w->app.dirty=1;
    desk_log("multi-window: paste window=%llu text=%s\n",(unsigned long long)w->app.window,w->text);
}
int tool_multi_capture(void) {
    if(surface_pool()) { srv_puts("multi-window: FAIL surface-pool\n");return 1; }
    desk_connection_init(&connection);if(open_next())return 1;
    uint64_t start=srv_ticks();int ready=0;
    while(connection.windows && (uint64_t)srv_ticks()-start<18000) {
        for(unsigned batch=0;batch<32;batch++) {
            struct desk_app *a;struct desk_packet p;
            int r=desk_connection_poll(&connection,&a,&p);
            if(r<0) { srv_puts("multi-window: FAIL connection\n");return 1; }
            if(!r)break;if(r==2)continue;
            struct capture_window *w=NULL;
            for(int i=0;i<opened;i++)if(&windows[i].app==a)w=&windows[i];
            if(!w) { srv_puts("multi-window: FAIL routing\n");return 1; }
            if(p.type==DESK_OPEN_REPLY) {
                if(p.value) { desk_log("multi-window: FAIL admission=%u\n",p.value);return 1; }
                desk_log("multi-window: admitted window=%llu\n",(unsigned long long)a->window);
            } else if(p.type==DESK_CONFIGURE) {
                desk_log("multi-window: configured window=%llu generation=%llu size=%ux%u\n",(unsigned long long)a->window,(unsigned long long)a->generation,a->width,a->height);
            } else if(p.type==DESK_CLOSE_ACK) {
                desk_log("multi-window: retired window=%llu text=%s\n",(unsigned long long)a->window,w->text);
                if(desk_window_dispose(a)) { srv_puts("multi-window: FAIL dispose\n");return 1; }
            } else if(p.type==DESK_CLOSE_REQUEST)desk_window_close(a);
            else if(p.type==DESK_CLIPBOARD_ACK || p.type==DESK_CLIPBOARD_DATA)paste(w,desk_clipboard_receive(&w->clipboard,&p));
            else if(p.type==DESK_MENU_ACTION) {
                if(p.value==1)open_next();else if(p.value==2)desk_window_close(a);
            } else if(p.type==DESK_KEY) {
                struct srv_key_event k;memcpy(&k,p.text,sizeof(k));
                if(!(k.flags&SRV_KEY_DOWN_EVENT))continue;
                if(k.modifiers&SRV_KEY_CTRL) {
                    if(k.usage==0x11)open_next();
                    else if(k.usage==0x1a)desk_window_close(a);
                    else if(k.usage==0x14)close_all();
                    else if(k.usage==0x06)desk_clipboard_copy(&w->clipboard,a->server,w->text,w->length,srv_ticks());
                    else if(k.usage==0x19 && !desk_clipboard_paste(&w->clipboard,a->server,srv_ticks()))w->pasting=1;
                    continue;
                }
                if(k.codepoint>=32 && k.codepoint<127 && w->length+1<sizeof(w->text))w->text[w->length++]=k.codepoint;
                else if(k.usage==SRV_KEY_BACKSPACE && w->length)w->length--;
                w->text[w->length]=0;a->dirty=1;
                desk_log("multi-window: input window=%llu text=%s\n",(unsigned long long)a->window,w->text);
            }
        }
        for(int i=0;i<opened;i++) {
            struct capture_window *w=&windows[i];struct desk_app *a=&w->app;
            if(!a->connection || a->closed)continue;
            if(!a->closing) { paste(w,desk_clipboard_tick(&w->clipboard,srv_ticks()));desk_app_menu_tick(a); }
            struct desk_canvas canvas;
            if(desk_app_canvas(a,&canvas)==1) {
                desk_fill(&canvas,canvas.clip,0xfff6f4ef);
                desk_text(&canvas,&desk_font_20,20,20,"Independent document",0xff26382c);
                desk_text(&canvas,&desk_font_16,20,64,w->text,0xff202420);
                desk_text(&canvas,&desk_font_16,20,116,"Ctrl-N: new window  Ctrl-W: close",0xff607064);
                if(desk_app_present(a)<0) { srv_puts("multi-window: FAIL present\n");return 1; }
            }
        }
        if(opened==1 && !connection.opening && windows[0].app.serial && !windows[0].app.pending)open_next();
        if(!ready && opened==2 && windows[1].app.serial && !windows[1].app.pending) {ready=1;srv_puts("multi-window: ready two windows one process\n");}
        if(quitting || desk_connection_wait(&connection,10)<0)srv_sleep_ticks(1);
    }
    if(connection.windows) { srv_puts("multi-window: FAIL deadline\n");return 1; }
    for(unsigned i=0;i<128;i++)if(gui_surface_destroy(pool[i])) { srv_puts("multi-window: FAIL surface-pool retirement\n");return 1; }
    srv_puts("multi-window: surface-pool retired\n");
    srv_puts("multi-window: PASS all windows retired; process exiting\n");return 0;
}
