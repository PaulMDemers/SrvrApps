#include <srvros/desk_chooser.h>
#include <srvros/desktop.h>
#include <srvros/desk_log.h>
#include <srvros/sys.h>
#include <string.h>

/* Opt-in actual worker transport check; the VM supplies a disposable /fat.
 * Submit a LIST before canceling, rather than simulating an unstarted request. */
static struct desk_file_job job;
static struct desk_file_picker picker;
static struct desk_file_entry entries[DESK_FILE_ENTRIES];
static struct desk_list_item items[DESK_FILE_ENTRIES];
static char details[DESK_FILE_ENTRIES][32];
static int wait_job(int submit) {
    for(unsigned ticks=0;ticks<2000;ticks++) {
        int result=submit?desk_file_job_submit(&job):desk_file_job_poll(&job);
        if(result)return result;srv_sleep_ticks(1);
    }
    return -1;
}
int tool_chooser_capture_at(const char *root,const char *target) {
    for(unsigned pooled=0;pooled<2;pooled++) {
        struct desk_file_pool pool;struct desk_file_channel channel;
        memset(&job,0,sizeof(job));desk_picker_init(&picker);
        if(pooled) {
            if(desk_file_pool_init(&pool,&channel,1) || desk_file_job_bind(&job,&pool))goto failed;
        } else if(desk_file_job_init(&job))goto failed;
        struct desk_chooser chooser;
        if(desk_chooser_init(&chooser,&picker,&job,entries,items,details) ||
           desk_chooser_open(&chooser,DESK_PICKER_OPEN,root,"") || desk_chooser_browse(&chooser))goto failed;
        uint64_t old=chooser.pending_id;
        if(wait_job(1)!=1 || !job.busy || !job.pid)goto failed;
        desk_chooser_cancel(&chooser);
        if(!chooser.pending || desk_chooser_open(&chooser,DESK_PICKER_OPEN,root,"")>=0)goto failed;
        int result=wait_job(0);
        if(result!=1 || desk_chooser_finish(&chooser,old,result)!=DESK_CHOOSER_DISCARDED || chooser.pending)goto failed;
        if(desk_chooser_open(&chooser,DESK_PICKER_OPEN,root,"") || desk_chooser_browse(&chooser))goto failed;
        if(desk_chooser_finish(&chooser,old,1)!=DESK_CHOOSER_STALE || !chooser.pending)goto failed;
        result=wait_job(0);
        if(result!=1 || desk_chooser_finish(&chooser,chooser.pending_id,result)!=DESK_CHOOSER_READY || chooser.count<1)goto failed;
        strcpy(picker.target,target);struct desk_chooser_selection selected;
        if(desk_chooser_action(&chooser,(struct desk_picker_result){.operation=DESK_PICKER_ACCEPT},&selected)!=DESK_CHOOSER_ACCEPTED ||
           strcmp(selected.path,target) || selected.mode!=DESK_PICKER_OPEN || chooser.active || chooser.pending)goto failed;
        uint64_t pid=job.pid,status=0;
        if(pooled) {
            if(desk_file_job_detach(&job))goto failed;
            int closed=0;
            for(unsigned ticks=0;ticks<1000 && !closed;ticks++) { closed=desk_file_pool_shutdown(&pool);if(!closed)srv_sleep_ticks(1); }
            if(closed!=1 || pool.live || pool.spawned!=1 || pool.reaped!=1)goto failed;
        } else {
            desk_file_job_close(&job);
            /* close may already have reaped this idle child. Either it is gone,
             * or this exact PID is reaped here; never leave a live worker. */
            int done=0;
            for(unsigned ticks=0;ticks<1000 && !done;ticks++) {
                long result=srv_wait(pid,&status,SRV_WAIT_NOHANG);
                if(result>0) { if(result!=(long)pid || status)goto failed;done=1; }
                else if(result<0 && !desk_process_alive(pid))done=1;
                else srv_sleep_ticks(1);
            }
            if(!done)goto failed;
        }
        desk_log("chooser-capture: mode=%s canceled-submitted=yes reopened=yes stale-rejected=yes worker-retired=yes\n",pooled?"pooled":"legacy");
    }
    srv_puts("chooser-capture: PASS actual legacy/pooled listing cancel/drain/reopen and selected path\n");return 0;
failed:
    srv_puts("chooser-capture: FAIL\n");return 1;
}

int tool_chooser_capture(void) { return tool_chooser_capture_at("/fat","/fat/bin/deskd"); }
