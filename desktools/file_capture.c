/* Opt-in fixture only: exercise the real Text and file-worker paths. */
#include "app.h"
#include <srvros/desk_battery.h>
#include <srvros/desk_log.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static struct {
    unsigned phase,cycles;
    uint64_t serial;
    char destination[DESK_FILE_PATH];
} capture;
static const char *root="/battery-test/fixture";
static const char *storage="image";
static char draft[DESK_FILE_PATH];
static struct {
    int enabled,cancel;
    unsigned requests,waiting,submit;
    uint64_t epoch,serial,deadline;
} controls;
static char capacity_root[DESK_FILE_PATH]="/work/srvros-work/capacity-XXXXXX";
static int capacity_mode,capacity_picker;
int tool_chooser_capture_at(const char *,const char *);
int tool_file_capture_capacity_setup(void) {
    if(!mkdtemp(capacity_root))return -1;
    root=capacity_root;storage="capacity";capacity_mode=1;
    snprintf(draft,sizeof(draft),"%s/text-draft.txt",root);
    desk_log("document-capacity: directory=%s fresh=yes\n",root);
    return 0;
}
static void capture_write(struct tool *t);
void tool_file_capture_controls_enable(int cancel) { controls.enabled=1;controls.cancel=cancel; }
static int control_request(struct tool *t,unsigned op) {
    struct srv_battery_status status={0};struct srv_battery_control control={0};
    if(controls.waiting || srv_battery_status(&status) || srv_battery_control(SRV_BATTERY_QUERY,0,&control))goto failed;
    struct desk_battery_controls view=desk_battery_controls(&status,&control);
    if(!(op==SRV_BATTERY_START?view.start:view.stop))goto failed;
    if(srv_battery_control(op,status.epoch,&control) || control.pending!=op)goto failed;
    controls.requests++;controls.waiting=op;controls.epoch=status.epoch;
    controls.serial=control.serial;controls.deadline=srv_ticks()+1500;
    desk_log("text-battery: request=%u serial=%llu epoch=%llu cycle=%u pending_file=%u file_op=%u submitted=%u\n",
        op,(unsigned long long)control.serial,(unsigned long long)status.epoch,capture.cycles,
        t->job.busy?1u:0u,t->job.busy?t->job.request.operation:0u,
        t->job.busy && t->job.sent==sizeof(t->job.request)+t->job.request.length?1u:0u);
    return 1;
failed:t->window.failed=1;return 0;
}
int tool_file_capture_controls_tick(struct tool *t) {
    if(!controls.enabled)return 1;
    if(t->window.failed)return 0;
    if(controls.submit) {
        int sent=desk_file_job_submit(&t->job);
        if(sent<0 || (uint64_t)srv_ticks()>controls.deadline)goto failed;
        if(!sent)return 0;
        unsigned op=controls.submit;controls.submit=0;
        if(!control_request(t,op))return 0;
        if(controls.cancel) {
            desk_log("text-battery: cancel serial=%llu pending_file=1 submitted=1\n",(unsigned long long)controls.serial);
            t->quit=1; /* Worker close lets an already accepted mutation finish. */
        }
        return 0;
    }
    struct srv_battery_status status={0};struct srv_battery_control control={0};
    if(srv_battery_status(&status) || srv_battery_control(SRV_BATTERY_QUERY,0,&control))goto failed;
    if(!controls.requests) {
        if(!control.available)return 1; /* The session owner binds after spawn. */
        (void)control_request(t,SRV_BATTERY_START);return 1;
    }
    if(!controls.waiting) {
        if(capture.phase==5)capture_write(t);
        return !controls.submit;
    }
    if((uint64_t)srv_ticks()>controls.deadline || control.result<0 || status.state==SRV_BATTERY_UNAVAILABLE || status.state==SRV_BATTERY_FAULTED)goto failed;
    if(control.pending)return 1;
    if(controls.waiting==SRV_BATTERY_START) {
        if(status.state!=SRV_BATTERY_ACTIVE || !(status.flags&SRV_BATTERY_FRESH) || status.generation<2)return 1;
        if(status.epoch!=controls.epoch+1)goto failed;
    } else {
        if(status.state!=SRV_BATTERY_STOPPED)return 1;
        if(status.epoch!=controls.epoch || (status.flags&(SRV_BATTERY_FRESH|SRV_BATTERY_SAMPLING)))goto failed;
    }
    desk_log("text-battery: observed=%u serial=%llu epoch=%llu state=%u generation=%llu pending_file=%u\n",
        controls.waiting,(unsigned long long)controls.serial,(unsigned long long)status.epoch,status.state,
        (unsigned long long)status.generation,t->job.busy?1u:0u);
    controls.waiting=0;
    if(capture.phase==5)capture_write(t);
    return !controls.submit;
failed:desk_log("text-battery: FAILED requests=%u waiting=%u\n",controls.requests,controls.waiting);t->window.failed=1;return 0;
}
void tool_file_capture_setup(int workspace) {
    if(workspace) { root="/work/battery-study";storage="workspace"; }
    snprintf(draft,sizeof(draft),"%s/text-draft.txt",root);
}
int tool_file_capture_busy(void) { return capture.phase!=0; }
unsigned tool_file_capture_cycles(void) { return capture.cycles; }
void tool_file_capture_begin(struct tool *t,uint64_t serial) {
    if(capture.phase || !t->worker_ready || t->job.busy || capture.cycles>=5 ||
       serial!=(capture.cycles+1)*12 || t->body_text.length!=2 ||
       memcmp(t->body_text.data,"AA",2) || t->clip.mode) { t->window.failed=1;return; }
    capture.serial=serial;capture.phase=5;
    /* Keep the checkpoint unacknowledged while Start becomes fresh. No file
     * request is created until the previous control is observed complete. */
    t->window.input_serial=serial-1;
    if(controls.enabled && (!controls.requests || controls.waiting || controls.submit)) {
        desk_log("text-files: deferred serial=%llu waiting=%u; no write submitted\n",
            (unsigned long long)serial,controls.waiting);
        return;
    }
    capture_write(t);
}
static void capture_write(struct tool *t) {
    if(capture.phase!=5 || t->window.failed || t->job.busy ||
       (controls.enabled && (!controls.requests || controls.waiting || controls.submit))) {
        t->window.failed=1;return;
    }
    capture.phase=1;
    snprintf(capture.destination,sizeof(capture.destination),"%s/text-%u.txt",root,capture.cycles+1);
    desk_log("text-files: write-ready serial=%llu\n",(unsigned long long)capture.serial);
    tool_text_save_path(t,draft);
    if(!t->job.busy)t->window.failed=1;
    if(controls.enabled && !t->window.failed && capture.cycles<4) {
        controls.submit=capture.cycles%2?SRV_BATTERY_START:SRV_BATTERY_STOP;
        controls.deadline=srv_ticks()+1500;
    }
}

void tool_file_capture_finish(struct tool *t,int result) {
    if(result<0 || t->job.response.status) { t->window.failed=1;return; }
    if(capture.phase==1) {
        tool_text_finish(t,result);
        if(tool_modified(t) || strcmp(t->saved_path,draft)) goto failed;
        struct desk_file_probe probe={0};capture.phase=2;
        if(desk_file_job_start(&t->job,DESK_FILE_STAT,draft,&probe,sizeof(probe),0,0)) goto failed;
    } else if(capture.phase==2) {
        struct desk_file_info info;memcpy(&info,t->job.incoming,sizeof(info));
        if(info.kind!=DESK_FILE_KIND_FILE || info.size!=2 || !info.inode) goto failed;
        struct desk_file_change change={.inode=info.inode,.size=info.size,.modified=info.modified};
        strcpy(change.destination,capture.destination);capture.phase=3;
        if(desk_file_job_start(&t->job,DESK_FILE_RENAME,draft,&change,sizeof(change),0,0)) goto failed;
    } else if(capture.phase==3) {
        /* Clear the current document before opening so retained RAM cannot
         * stand in for a successful disk read. */
        tool_text_request(t,TOOL_PENDING_NEW);
        if(t->body_text.length || t->modal || t->job.busy) goto failed;
        capture.phase=4;tool_text_open_path(t,capture.destination);
        if(!t->job.busy) goto failed;
    } else if(capture.phase==4) {
        tool_text_finish(t,result);
        if(tool_modified(t) || strcmp(t->saved_path,capture.destination) ||
           t->body_text.length!=2 || memcmp(t->body_text.data,"AA",2) ||
           t->body_text.caret!=0 || t->body_text.anchor!=0) goto failed;
        capture.cycles++;capture.phase=0;
        if(capacity_mode && capture.cycles==5) {
            if(tool_chooser_capture_at(root,capture.destination))goto failed;
            capacity_picker=1;
            desk_log("document-capacity: verified directory=%s files=5 bytes_each=2 chooser=PASS\n",root);
        }
        t->window.input_serial=capture.serial;t->window.dirty=1;
    } else goto failed;
    return;
failed:t->window.failed=1;
}
int tool_file_capture_report(void) {
    if(capacity_mode && (!capacity_picker || capture.cycles!=5 || capture.phase))return 0;
    desk_log("text-files: cycles=%u pending=%u storage=%s\n",capture.cycles,capture.phase?1u:0u,storage);
    if(controls.enabled) {
        struct srv_battery_status status={0};
        int ok=!srv_battery_status(&status) && controls.requests==5 && !controls.waiting &&
            status.state==SRV_BATTERY_ACTIVE && (status.flags&SRV_BATTERY_FRESH) &&
            capture.cycles==5 && !capture.phase;
        desk_log("text-battery: completed=%s requests=%u waiting=%u epoch=%llu state=%u; expected_owner=active\n",
            ok?"yes":"NO",controls.requests,controls.waiting,(unsigned long long)status.epoch,status.state);
        return ok;
    }
    return 1;
}
