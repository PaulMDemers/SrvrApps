#include <srvros/desk_file_job.h>
#include <srvros/desk_log.h>
#include <srvros/sys.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>

/* Opt-in actual transport fixture. Host harness supplies a disposable image. */
#define POOL_JOBS 40
static unsigned char payload[DESK_FILE_BYTES];
static unsigned char value(unsigned job,unsigned byte) { return (unsigned char)(job*17+byte*7); }
int tool_file_pool_capture(void) {
    struct desk_file_pool pool;struct desk_file_channel channels[2];
    struct desk_file_job *jobs=mmap(NULL,POOL_JOBS*sizeof(*jobs),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(jobs==MAP_FAILED || desk_file_pool_init(&pool,channels,2))return 1;
    for(unsigned i=0;i<POOL_JOBS;i++)if(desk_file_job_bind(&jobs[i],&pool))goto failed;
    for(unsigned pass=0;pass<2;pass++) {
        for(unsigned i=0;i<POOL_JOBS;i++) {
            char path[80];snprintf(path,sizeof(path),"/fat/pool-%u.bin",i);
            for(unsigned b=0;b<sizeof(payload);b++)payload[b]=value(i,b);
            if(desk_file_job_start(&jobs[i],pass?DESK_FILE_READ:DESK_FILE_WRITE,path,pass?NULL:payload,pass?0:sizeof(payload),0,0))goto failed;
        }
        if(!pass && (pool.spawned || desk_file_job_detach(&jobs[0])>=0 || desk_file_pool_shutdown(&pool)>=0))goto failed;
        unsigned complete=0;uint64_t start=srv_ticks();
        while(complete<POOL_JOBS) {
            for(unsigned n=POOL_JOBS;n>0;n--) {
                unsigned i=n-1;if(!jobs[i].busy)continue;
                int result=desk_file_job_poll(&jobs[i]);
                if(result<0 || (result && jobs[i].response.status))goto failed;
                if(result) {
                    if(pass) {
                        if(jobs[i].response.length!=DESK_FILE_BYTES)goto failed;
                        for(unsigned b=0;b<DESK_FILE_BYTES;b++)if(jobs[i].incoming[b]!=value(i,b))goto failed;
                    }
                    complete++;
                }
            }
            if((uint64_t)srv_ticks()-start>20000)goto failed;
            if(complete<POOL_JOBS)srv_sleep_ticks(1);
        }
        desk_log("file-pool-capture: phase=%s jobs=%u spawned=%llu peak=%llu\n",pass?"read":"write",complete,(unsigned long long)pool.spawned,(unsigned long long)pool.peak);
    }
    if(pool.peak!=2 || pool.spawned!=2)goto failed;
    for(unsigned i=0;i<POOL_JOBS;i++)if(desk_file_job_detach(&jobs[i]))goto failed;
    int result;while(!(result=desk_file_pool_shutdown(&pool)))srv_sleep_ticks(1);
    if(result!=1 || pool.live || pool.reaped!=2)goto failed;
    munmap(jobs,POOL_JOBS*sizeof(*jobs));
    srv_puts("file-pool-capture: PASS 40 queued 64KiB writes/readbacks, two workers reaped\n");return 0;
failed:
    srv_puts("file-pool-capture: FAIL\n");return 1;
}
