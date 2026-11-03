#ifndef DESKTOOLS_FILE_SERVICE_H
#define DESKTOOLS_FILE_SERVICE_H
#include <pthread.h>
#include <sys/mman.h>
/* UI owns pool/jobs/GUI lists. Only immutable requests and completed value
 * records cross this boundary. A full queue retains the caller's mapping. */
#define FILE_SERVICE_STARTS 2
#define FILE_SERVICE_RETIRES 16
#define FILE_SERVICE_IDLE 0u
#define FILE_SERVICE_QUEUED 1u
#define FILE_SERVICE_DONE 2u
#define FILE_SERVICE_FAILED 3u
struct file_service_start {
    unsigned state,cancel;int result;
    struct desk_file_transport transport;
    uint64_t elapsed;
};
struct file_service_retire {
    unsigned state;int result,role,canvas;uint64_t window,elapsed;
    void *address;size_t length;
};
struct file_service {
    pthread_t thread;unsigned event,stop,started,quiesced;
    struct desk_file_pool *pool;
    struct file_service_start starts[FILE_SERVICE_STARTS];
    struct file_service_retire retires[FILE_SERVICE_RETIRES];
    uint64_t requested,opened,open_failed,cancelled,abandoned,jobs_cancelled,retired,retire_failed,max_start_us,max_retire_us;
    int role;
    _Alignas(16) unsigned char stack[64*1024];
};
static unsigned file_service_state(const unsigned *p) {return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
static void file_service_publish(unsigned *p,unsigned v) {__atomic_store_n(p,v,__ATOMIC_RELEASE);}
static void file_service_notify(struct file_service *s) {
    if(!__atomic_exchange_n(&s->event,1,__ATOMIC_RELEASE))srv_futex_wake((uint32_t *)&s->event,1);
}
static void file_service_work(struct file_service *s) {
    for(unsigned i=0;i<FILE_SERVICE_STARTS;i++) {
        struct file_service_start *r=&s->starts[i];
        if(file_service_state(&r->state)!=FILE_SERVICE_QUEUED)continue;
        uint64_t at=(uint64_t)srv_monotonic_us();
        r->result=desk_file_transport_open_cancelable(&r->transport,&r->cancel);
        r->elapsed=(uint64_t)srv_monotonic_us()-at;
        file_service_publish(&r->state,FILE_SERVICE_DONE);
    }
    for(unsigned i=0;i<FILE_SERVICE_RETIRES;i++) {
        struct file_service_retire *r=&s->retires[i];
        if(file_service_state(&r->state)!=FILE_SERVICE_QUEUED)continue;
        const char *stage=r->canvas?"canvas-unmap":"document-unmap";
        uint64_t at=tool_timing_begin(r->role,r->window,stage);
        struct tool_phase_trace phase;tool_phase_begin(&phase);
        r->result=munmap(r->address,r->length);
        tool_phase_end(&phase,r->role,r->window,stage);
        tool_timing_end(r->role,r->window,stage,at);
        r->elapsed=(uint64_t)srv_monotonic_us()-at;
        file_service_publish(&r->state,FILE_SERVICE_DONE);
    }
}
static void *file_service_thread(void *arg) {
    struct file_service *s=arg;
    tool_timing_lane=1;
    for(;;) {
        __atomic_exchange_n(&s->event,0,__ATOMIC_ACQUIRE);
        file_service_work(s);
        if(file_service_state(&s->stop)) {
            /* The sole dispatcher published all final records before stop.
             * Acquire stop, then rescan: it may have queued into a slot that
             * this iteration had already passed. No admission follows stop. */
            file_service_work(s);break;
        }
        unsigned completed=0;
        for(unsigned i=0;i<FILE_SERVICE_STARTS;i++)completed|=file_service_state(&s->starts[i].state)==FILE_SERVICE_DONE;
        for(unsigned i=0;i<FILE_SERVICE_RETIRES;i++)completed|=file_service_state(&s->retires[i].state)==FILE_SERVICE_DONE;
        /* While the dispatcher consumes a completion, keep stop responsive
         * through the audited sleep path instead of entering another gated
         * futex continuation. Fully idle services retain event-driven waits. */
        if(completed)srv_sleep_ticks(1);
        else if(srv_futex_wait((uint32_t *)&s->event,0,0)<0 && !file_service_state(&s->event))srv_sleep_ticks(1);
    }
    /* An unconsumed successful startup still belongs to this service. Publish
     * EOF and reap naturally; never orphan a child or kill accepted work. */
    for(unsigned i=0;i<FILE_SERVICE_STARTS;i++) {
        struct file_service_start *r=&s->starts[i];
        if(file_service_state(&r->state)==FILE_SERVICE_DONE && !r->result) {
            srv_close(r->transport.writer);srv_close(r->transport.reader);
            uint64_t status=0;long result;
            do {result=srv_wait(r->transport.pid,&status,0);if(result==SRV_ERR_INTR || !result)srv_sleep_ticks(1);}while(result==SRV_ERR_INTR || !result);
            if(result!=(long)r->transport.pid || status)r->result=-1;
            else {r->transport=(struct desk_file_transport){.reader=-1,.writer=-1};s->abandoned++;}
        }
    }
    file_service_publish(&s->quiesced,1);
    return NULL;
}
static int file_service_open(void *context,struct desk_file_channel *channel) {
    struct file_service *s=context;
    if(file_service_state(&s->stop))return -1;
    unsigned i=0;while(i<s->pool->count && &s->pool->channels[i]!=channel)i++;
    if(i>=FILE_SERVICE_STARTS || i==s->pool->count)return -1;
    struct file_service_start *r=&s->starts[i];unsigned state=file_service_state(&r->state);
    if(state==FILE_SERVICE_IDLE) {
        r->transport=(struct desk_file_transport){.reader=-1,.writer=-1};r->result=0;r->elapsed=0;file_service_publish(&r->cancel,0);s->requested++;
        file_service_publish(&r->state,FILE_SERVICE_QUEUED);file_service_notify(s);return 0;
    }
    if(state!=FILE_SERVICE_DONE)return 0;
    int result=r->result;
    if(r->elapsed>s->max_start_us)s->max_start_us=r->elapsed;
    if(result==DESK_FILE_TRANSPORT_CANCELLED)s->cancelled++;
    else if(!result) {
        channel->pid=r->transport.pid;channel->reader=r->transport.reader;channel->writer=r->transport.writer;s->opened++;
    } else s->open_failed++;
    file_service_publish(&r->state,FILE_SERVICE_IDLE);return result==DESK_FILE_TRANSPORT_CANCELLED?0:result?-1:1;
}
static void file_service_read_cancelled(struct file_service *s) {
    s->jobs_cancelled++;
    if(s->pool->head)return;
    for(unsigned i=0;i<FILE_SERVICE_STARTS;i++) {
        struct file_service_start *r=&s->starts[i];
        if(file_service_state(&r->state)!=FILE_SERVICE_IDLE)file_service_publish(&r->cancel,1);
    }
}
static int file_service_poll(struct file_service *s) {
    int pending=0;
    for(unsigned i=0;i<FILE_SERVICE_RETIRES;i++) {
        struct file_service_retire *r=&s->retires[i];unsigned state=file_service_state(&r->state);
        if(state==FILE_SERVICE_DONE) {
            if(r->elapsed>s->max_retire_us)s->max_retire_us=r->elapsed;
            if(r->result) {s->retire_failed++;file_service_publish(&r->state,FILE_SERVICE_FAILED);}
            else {s->retired++;file_service_publish(&r->state,FILE_SERVICE_IDLE);}
        } else if(state==FILE_SERVICE_QUEUED)pending=1;
    }
    return pending;
}
struct file_service_release {struct file_service *service;uint64_t window;int canvas;};
static int file_service_capacity(const struct file_service *s) {
    if(file_service_state(&s->stop) || s->retire_failed)return 0;
    for(unsigned i=0;i<FILE_SERVICE_RETIRES;i++)if(file_service_state(&s->retires[i].state)==FILE_SERVICE_IDLE)return 1;
    return 0;
}
static int file_service_retire(void *context,void *address,size_t length) {
    struct file_service_release *release=context;struct file_service *s=release->service;
    if(file_service_state(&s->stop) || s->retire_failed)return -1;
    for(unsigned i=0;i<FILE_SERVICE_RETIRES;i++) {
        struct file_service_retire *r=&s->retires[i];
        if(file_service_state(&r->state)!=FILE_SERVICE_IDLE)continue;
        r->address=address;r->length=length;r->role=s->role;r->window=release->window;r->canvas=release->canvas;r->result=0;r->elapsed=0;
        file_service_publish(&r->state,FILE_SERVICE_QUEUED);file_service_notify(s);return 0;
    }
    return -1;
}
static int file_service_join(struct file_service *s) {
    int failed=0,result;
    while((result=pthread_join(s->thread,NULL))) {
        if(!failed)desk_log("file-service: join error=%d; ownership retained\n",result);
        failed=1;srv_sleep_ticks(1);
    }
    return failed?-1:0;
}
static int file_service_start(struct file_service *s,struct desk_file_pool *pool,int role) {
    if(!s || s->started || !pool || !pool->channels || !pool->count || pool->count>FILE_SERVICE_STARTS || pool->bound || pool->live || pool->head || pool->closing || pool->opener)return -1;
    *s=(struct file_service){.pool=pool,.role=role};
    uint64_t at=tool_timing_begin(role,0,"service-attributes");
    pthread_attr_t attr;int initialized=pthread_attr_init(&attr);int result=initialized;
    if(!result)result=pthread_attr_setstack(&attr,s->stack,sizeof(s->stack));
    tool_timing_end(role,0,"service-attributes",at);
    if(result){if(!initialized)pthread_attr_destroy(&attr);return -1;}
    if(!result)result=pthread_create(&s->thread,&attr,file_service_thread,s);
    pthread_attr_destroy(&attr);
    if(result)return -1;
    s->started=1;
    if(desk_file_pool_opener(pool,file_service_open,s)) {
        file_service_publish(&s->stop,1);file_service_notify(s);file_service_join(s);s->started=0;return -1;
    }
    return 0;
}
static int file_service_complete(struct file_service *s,unsigned process_owned,int join_error) {
    file_service_poll(s);
    unsigned pending=0;
    for(unsigned i=0;i<FILE_SERVICE_STARTS;i++) {
        struct file_service_start *r=&s->starts[i];
        if(file_service_state(&r->state)==FILE_SERVICE_DONE) {
            if(r->elapsed>s->max_start_us)s->max_start_us=r->elapsed;
            if(r->result==DESK_FILE_TRANSPORT_CANCELLED) {s->cancelled++;file_service_publish(&r->state,FILE_SERVICE_IDLE);}
            else if(r->result) {s->open_failed++;file_service_publish(&r->state,FILE_SERVICE_FAILED);}
            else file_service_publish(&r->state,FILE_SERVICE_IDLE);
        }
        pending+=file_service_state(&r->state)!=FILE_SERVICE_IDLE;
    }
    for(unsigned i=0;i<FILE_SERVICE_RETIRES;i++)pending+=file_service_state(&s->retires[i].state)==FILE_SERVICE_QUEUED;
    desk_log("file-service: stopped version=3 pid=%llu role=%d requested=%llu opened=%llu failures=%llu cancelled=%llu abandoned=%llu jobs_cancelled=%llu retired=%llu retire_failed=%llu pending=%u join_error=%u max_start_us=%llu max_retire_us=%llu async=yes quiesced=yes process_owned=%s joined=%s\n",(unsigned long long)srv_getpid(),s->role,
        (unsigned long long)s->requested,(unsigned long long)s->opened,(unsigned long long)s->open_failed,(unsigned long long)s->cancelled,(unsigned long long)s->abandoned,(unsigned long long)s->jobs_cancelled,(unsigned long long)s->retired,(unsigned long long)s->retire_failed,pending,
        join_error!=0,
        (unsigned long long)s->max_start_us,(unsigned long long)s->max_retire_us,process_owned?"yes":"no",process_owned?"no":"yes");
    return pending || s->retire_failed || join_error?-1:0;
}
/* Admission stop is separate from completion. The final GUI/model owner can
 * request it while queued unmaps and idle backend retirement overlap. State,
 * stack and transport ownership remain pinned until finish/quiesce below. */
static void file_service_request_stop(struct file_service *s) {
    s->pool->closing=1;
    if(!__atomic_exchange_n(&s->stop,1,__ATOMIC_RELEASE))file_service_notify(s);
}
/* Reusable service: no caller-owned storage may disappear before join. */
static inline int file_service_finish(struct file_service *s) {
    if(!s->started)return 0;
    file_service_request_stop(s);
    int join_error=file_service_join(s);s->started=0;
    return file_service_complete(s,0,join_error);
}
/* Only a process-owned static context may use this path. The helper body has
 * released all work ownership, but its pthread/TLS/stack may still be live.
 * Keep all service storage pinned/nonreusable until normal kernel process exit
 * and the parent's verified wait; never call this on temporary/heap storage. */
static int file_service_quiesce(struct file_service *s) {
    if(!s->started)return 0;
    file_service_request_stop(s);
    while(!file_service_state(&s->quiesced))srv_sleep_ticks(1);
    return file_service_complete(s,1,0);
}
#endif
