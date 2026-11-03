/* Automated GUI+worker fixture; its host harness owns a disposable volume. */
#include <srvros/desk_connection.h>
#include <srvros/desk_runtime.h>
#include <srvros/desk_file_job.h>
#include <srvros/desk_log.h>
#include <srvros/perf_sample.h>
#include <srvros/key_event.h>
#include <srvros/sys.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#define JOBS 40
struct phase_sample { struct perf_sample dispatch;char text[129];unsigned length; };
static struct phase_sample samples[2];
static unsigned char payload[DESK_FILE_BYTES];
static unsigned char pattern(unsigned job,unsigned byte) {return (unsigned char)(job*17+byte*7);}
struct capture_events {
    struct desk_app *app;
    int *ready,*started,*finished;
    unsigned *keys,*complete;
};
static int capture_event(void *context,struct desk_app *target,struct desk_packet packet) {
    struct capture_events *events=context;
    if(target!=events->app)return -1;
    if(packet.type==DESK_OPEN_REPLY && packet.value)return -1;
    if(packet.type==DESK_WINDOW_FAILED || packet.type==DESK_CLOSE_REQUEST)return -1;
    if(packet.type==DESK_CLOSE_ACK) {if(desk_window_dispose(events->app))return -1;return 1;}
    if(packet.type==DESK_KEY) {
        struct srv_key_event key;memcpy(&key,packet.text,sizeof(key));
        if(key.flags&SRV_KEY_CANCEL)return -1;
        if(!(key.flags&SRV_KEY_DOWN_EVENT) || key.codepoint<32 || key.codepoint>=127)return 0;
        unsigned index=*events->started?1:0;struct phase_sample *sample=&samples[index];
        if(!*events->ready || *events->finished || sample->length==128)return -1;
        uint64_t now=srv_monotonic_us();if(!key.time_us || now<key.time_us)return -1;
        perf_record(&sample->dispatch,now-key.time_us);
        sample->text[sample->length++]=key.codepoint;sample->text[sample->length]=0;
        (*events->keys)++;events->app->dirty=1;
        desk_log("wait-load: input phase=%s count=%u complete=%u text=%s\n",index?"load":"idle",sample->length,*events->complete,sample->text);
    }
    return 0;
}
static void start_due(void *context,uint64_t now) {(void)now;*(int *)context=1;}
int tool_wait_capture(void) {
    struct desk_connection connection;desk_connection_init(&connection);
    struct desk_runtime runtime;desk_runtime_init(&runtime,&connection);
    struct desk_runtime_timer phase_timer={0};int idle_due=0;
    struct desk_app app;struct desk_file_pool pool;struct desk_file_channel channels[2];
    struct desk_file_job *jobs=mmap(NULL,JOBS*sizeof(*jobs),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(jobs==MAP_FAILED || desk_file_pool_init(&pool,channels,2) || desk_window_open(&connection,&app,"GUI and worker wait",640,400))goto failure;
    for(unsigned i=0;i<JOBS;i++)if(desk_file_job_bind(&jobs[i],&pool))goto failure;
    unsigned phase=0,complete=0,keys=0,frame_keys=0,presented=0;
    uint64_t idle_end=0,start=srv_monotonic_us(),waits=0,wake_gui=0,wake_fd=0,wake_deadline=0;
    int ready=0,started=0,finished=0,closing=0;
    struct capture_events events={&app,&ready,&started,&finished,&keys,&complete};
    while(connection.windows && (uint64_t)srv_monotonic_us()-start<240000000) {
        if(desk_runtime_dispatch(&runtime,capture_event,&events)<0)goto failure;
        if(!app.connection)break;
        if(!app.pending && frame_keys>presented)presented=frame_keys;
        struct desk_canvas canvas;
        if(desk_app_canvas(&app,&canvas)==1) {
            desk_fill(&canvas,canvas.clip,0xfff6f4ef);
            desk_text(&canvas,&desk_font_20,20,24,started?"File workers active":"Idle event wait",0xff293b31);
            desk_text(&canvas,&desk_font_16,20,80,samples[0].text,0xff293b31);
            desk_text(&canvas,&desk_font_16,20,120,samples[1].text,0xff293b31);
            int result=desk_app_present(&app);if(result<0)goto failure;if(result==1)frame_keys=keys;
        }
        uint64_t now=srv_monotonic_us();
        if(!ready && app.serial && !app.dirty && !app.pending) {
            ready=1;idle_end=now+3000000;
            if(desk_runtime_timer_arm(&runtime,&phase_timer,idle_end,start_due,&idle_due))goto failure;
            srv_puts("wait-load: idle ready\n");
        }
        if(ready && !started && idle_due) {
            if(samples[0].length<16)goto failure;
            started=1;
            for(unsigned i=0;i<JOBS;i++) {
                char path[64];snprintf(path,sizeof(path),"/fat/wait-%u.bin",i);
                for(unsigned b=0;b<sizeof(payload);b++)payload[b]=pattern(i,b);
                if(desk_file_job_start(&jobs[i],DESK_FILE_WRITE,path,payload,sizeof(payload),0,0))goto failure;
            }
            app.dirty=1;srv_puts("wait-load: writes active\n");
        }
        if(started && !finished) {
            for(unsigned n=JOBS;n;n--) {
                unsigned i=n-1;if(!jobs[i].busy)continue;
                int result=desk_file_job_poll(&jobs[i]);
                if(result<0 || (result && jobs[i].response.status))goto failure;
                if(result) {
                    if(phase) {
                        if(jobs[i].response.length!=DESK_FILE_BYTES)goto failure;
                        for(unsigned b=0;b<DESK_FILE_BYTES;b++)if(jobs[i].incoming[b]!=pattern(i,b))goto failure;
                    }
                    complete++;
                }
            }
            if(complete==JOBS) {
                desk_log("wait-load: phase=%s complete=%u keys=%u\n",phase?"read":"write",complete,samples[1].length);
                if(!phase) {
                    if(samples[1].length<32)goto failure;
                    phase=1;complete=0;
                    for(unsigned i=0;i<JOBS;i++)if(desk_file_job_start(&jobs[i],DESK_FILE_READ,jobs[i].request.path,NULL,0,0,0))goto failure;
                } else finished=1;
            }
        }
        if(finished && !closing && !app.dirty && !app.pending && presented==keys) {
            if(pool.spawned!=2 || pool.peak!=2)goto failure;
            if(jobs[0].pool)for(unsigned i=0;i<JOBS;i++)if(desk_file_job_detach(&jobs[i]))goto failure;
            int result=desk_file_pool_shutdown(&pool);if(result<0)goto failure;
            if(result==1) {closing=1;desk_window_close(&app);}
        }
        if(!finished)desk_runtime_file_pool(&runtime,&pool);
        int result=desk_runtime_idle(&runtime,finished);
        if(result>=0) {waits++;wake_gui+=(result&SRV_GUI_READY)!=0;wake_fd+=(result&SRV_GUI_FD_READY)!=0;wake_deadline+=(result&SRV_GUI_DEADLINE)!=0;}
    }
    if(connection.windows || !finished || pool.live || pool.reaped!=2 || presented!=keys || !wake_fd || !wake_gui)goto failure;
    for(unsigned i=0;i<2;i++) {
        struct perf_sample *p=&samples[i].dispatch;
        desk_log("wait-load: latency phase=%s count=%llu mean_us=%llu p95_bound_us=%llu p99_bound_us=%llu max_us=%llu\n",i?"load":"idle",
            (unsigned long long)p->count,(unsigned long long)(p->count?p->total/p->count:0),
            (unsigned long long)perf_percentile(p,95),(unsigned long long)perf_percentile(p,99),(unsigned long long)p->max);
    }
    desk_log("wait-load: waits=%llu gui=%llu fd=%llu deadline=%llu keys=%u acknowledged=%u workers=%llu/%llu\n",
        (unsigned long long)waits,(unsigned long long)wake_gui,(unsigned long long)wake_fd,(unsigned long long)wake_deadline,keys,presented,
        (unsigned long long)pool.reaped,(unsigned long long)pool.spawned);
    desk_runtime_timers_clear(&runtime);
    munmap(jobs,JOBS*sizeof(*jobs));srv_puts("wait-load: PASS GUI dispatch during 40 worker writes/readbacks; all keys acknowledged and workers reaped\n");return 0;
failure:desk_runtime_timers_clear(&runtime);srv_puts("wait-load: FAIL\n");return 1;
}
