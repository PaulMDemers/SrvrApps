#ifndef SRVROS_TOOL_TIMING_H
#define SRVROS_TOOL_TIMING_H
#include <srvros/desk_log.h>
/* One dispatcher lane and one file-service lane. Timestamps from independent
 * threads can be captured before their log writes reach the common sink. */
static _Thread_local unsigned tool_timing_lane;

/* Sparse lifecycle checkpoints, never per-frame/poll logging. Times are wall
 * clock (including scheduling/I/O waits), not CPU time. Begin survives a stall;
 * elapsed includes its diagnostic write. No deadline or ownership is changed. */
static inline uint64_t tool_timing(int role,uint64_t window,const char *stage,
                                  const char *edge,uint64_t start) {
    uint64_t now=(uint64_t)srv_monotonic_us();
    desk_log("app-timing: pid=%llu role=%d window=%llu stage=%s edge=%s at_us=%llu elapsed_us=%llu lane=%u\n",
        (unsigned long long)srv_getpid(),role,(unsigned long long)window,stage,edge,
        (unsigned long long)now,(unsigned long long)(start && now>=start?now-start:0),tool_timing_lane);
    return now;
}
static inline uint64_t tool_timing_begin(int role,uint64_t window,const char *stage) {
    return tool_timing(role,window,stage,"begin",0);
}
static inline void tool_timing_end(int role,uint64_t window,const char *stage,uint64_t start) {
    tool_timing(role,window,stage,"end",start);
}
static inline void tool_timing_event(int role,uint64_t window,const char *stage) {
    tool_timing(role,window,stage,"event",0);
}
/* Current-thread attribution for sparse teardown phases. Diagnostic writes are
 * outside BEGIN/END. A failed BEGIN must never END someone else's active trace.
 * Scheduler durations are not CPU time or proof of a particular gate owner. */
struct tool_phase_trace { struct srv_sched_trace scheduler;long begin; };
static inline void tool_phase_begin(struct tool_phase_trace *phase) {
    *phase=(struct tool_phase_trace){0};
    phase->begin=srv_sched_trace(SRV_SCHED_TRACE_BEGIN,&phase->scheduler);
}
static inline void tool_phase_end(struct tool_phase_trace *phase,int role,uint64_t window,const char *stage) {
    long end=phase->begin?phase->begin:srv_sched_trace(SRV_SCHED_TRACE_END,&phase->scheduler);
    if(phase->begin || end) {
        desk_log("app-phase-sched: pid=%llu role=%d window=%llu stage=%s begin=%ld end=%ld available=no\n",
            (unsigned long long)srv_getpid(),role,(unsigned long long)window,stage,phase->begin,end);
        return;
    }
    const struct srv_sched_trace *q=&phase->scheduler;
    char text[768];
    int length=snprintf(text,sizeof(text),"app-phase-sched: pid=%llu role=%d window=%llu stage=%s start_us=%llu end_us=%llu blocked_us=%llu ready_us=%llu blocks=%llu dispatches=%llu exclusions=%llu invalid=%llu max_ready_us=%llu reason=%llu\n",
        (unsigned long long)srv_getpid(),role,(unsigned long long)window,stage,
        (unsigned long long)q->start_us,(unsigned long long)q->end_us,
        (unsigned long long)q->blocked_us,(unsigned long long)q->ready_us,
        (unsigned long long)q->blocks,(unsigned long long)q->dispatches,
        (unsigned long long)q->exclusions,(unsigned long long)q->invalid,
        (unsigned long long)q->max_ready_us,(unsigned long long)q->reason);
    if(length>0 && (unsigned)length<sizeof(text))srv_puts(text);
}
#endif
