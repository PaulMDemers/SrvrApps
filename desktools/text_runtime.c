#include "runtime.h"
#include "timing.h"
#include <srvros/desk_window_set.h>
#include <srvros/desk_log.h>
#include <stdlib.h>
#include <string.h>
/* Creator-only opt-in; the helper's TLS starts with tracing disabled. */
static _Thread_local int pthread_trace_role;
static _Thread_local uint64_t pthread_trace_at;
static _Thread_local struct tool_phase_trace pthread_trace_phase;
void __posix_pthread_create_trace(const char *stage,int begin) {
    if(!pthread_trace_role)return;
    if(begin) {
        pthread_trace_at=tool_timing_begin(pthread_trace_role,0,stage);
        tool_phase_begin(&pthread_trace_phase);
    } else {
        tool_phase_end(&pthread_trace_phase,pthread_trace_role,0,stage);
        tool_timing_end(pthread_trace_role,0,stage,pthread_trace_at);
    }
}
static int surface_trace_role;
static _Thread_local uint64_t surface_trace_at;
static _Thread_local struct tool_phase_trace surface_trace_phase;
void __desk_app_surface_trace(struct desk_app *window,const char *stage,int begin) {
    if(!surface_trace_role)return;
    if(begin) {
        surface_trace_at=tool_timing_begin(surface_trace_role,window->window,stage);
        tool_phase_begin(&surface_trace_phase);
    } else {
        tool_phase_end(&surface_trace_phase,surface_trace_role,window->window,stage);
        tool_timing_end(surface_trace_role,window->window,stage,surface_trace_at);
    }
}
#include "file_service.h"

struct tool_document {
    struct tool tool;
    uint64_t logged_job;
    char initial_path[DESK_FILE_PATH];
    int worker_closed,bad_path;
};
struct tool_windows {
    struct desk_window_set set;
    struct desk_file_pool pool;
    struct desk_file_channel channels[2];
    struct file_service files;
    struct desk_open_document open_request;
    struct desk_window_owner *initial_owner;
    uint32_t *initial_pixels;
    uint64_t service_window;
    unsigned capture_checkpoints;
    int role,capacity;
};
static struct tool_document *document(struct desk_window_owner *owner) {return (void *)owner->model;}
static const char *role_name(int role) {return role==DESK_APP_TEXT?"text":"files";}
void tool_new_window(struct tool *t) {
    if((t->role==DESK_APP_TEXT || t->role==DESK_APP_FILES) && t->window.connection) {
        if(!t->quit && !t->window.closing)t->new_window=1;
    } else tool_launch(t,t->role,t->role==DESK_APP_FILES?t->saved_path:"");
}
static void notice(struct tool_windows *app,uint64_t requester,const char *message) {
    desk_log("%s-runtime: %s\n",role_name(app->role),message);
    struct tool *fallback=NULL;
    for(struct desk_window_owner *o=app->set.windows;o;o=o->next) {
        struct tool *t=&document(o)->tool;
        if(o->ready && !o->retired && !t->quit) {
            if(t->window.window==requester) {tool_status(t,message);return;}
            if(!fallback)fallback=t;
        }
    }
    if(fallback)tool_status(fallback,message);
}
static struct desk_window_owner *allocate_document(struct tool_windows *app) {
    uint64_t timing=tool_timing_begin(app->role,0,"document-map");
    struct tool_phase_trace phase;tool_phase_begin(&phase);
    struct desk_window_owner *owner=desk_window_owner_new(&app->set,sizeof(struct tool_document)+
        (app->role==DESK_APP_TEXT?sizeof(struct desk_text_history)+TOOL_TEXT_HISTORY_BYTES:0));
    tool_phase_end(&phase,app->role,0,"document-map");
    tool_timing_end(app->role,0,"document-map",timing);
    return owner;
}
static int create(struct tool_windows *app,uint64_t requester,const char *path) {
    struct desk_window_owner *owner=app->initial_owner;app->initial_owner=NULL;
    if(!owner)owner=allocate_document(app);
    uint64_t timing;
    if(!owner) {notice(app,requester,"There is not enough memory for another window.");return -1;}
    struct tool_document *d=document(owner);struct tool *t=&d->tool;
    tool_runtime_init(t,app->role);
    if(app->role==DESK_APP_TEXT) {
        /* Aligned model tail shares the document mapping and its retirement.
         * No heap call is permitted after helper startup for this journal. */
        _Static_assert(sizeof(struct tool_document)%_Alignof(struct desk_text_history)==0,"journal alignment");
        uint64_t history_at=tool_timing_begin(app->role,0,"text-history-reserve");
        struct tool_phase_trace phase;tool_phase_begin(&phase);
        int history_ready=tool_text_history_prepare(t,(struct desk_text_history *)(d+1));
        tool_phase_end(&phase,app->role,0,"text-history-reserve");
        tool_timing_end(app->role,0,"text-history-reserve",history_at);
        if(history_ready) {
            desk_window_owner_release(owner,1);notice(app,requester,"The document history could not be prepared.");return -1;
        }
    }
    if(strlen(path)>=sizeof(d->initial_path))d->bad_path=1;else strcpy(d->initial_path,path);
    timing=tool_timing_begin(app->role,0,"window-open");
    int opened=desk_window_owner_open(owner,&t->window,requester,
        app->role==DESK_APP_TEXT?"Text — Untitled":"Files",720,480);
    tool_timing_end(app->role,0,"window-open",timing);
    if(opened) {
        desk_window_owner_release(owner,1);notice(app,requester,"Another window is still opening. Try again shortly.");return -1;
    }
    if(app->initial_pixels) {
        uint64_t adopt_at=tool_timing_begin(app->role,t->window.window,"canvas-adopt");
        if(desk_app_adopt_pixels(&t->window,app->initial_pixels,720,480))
            desk_pixels_free(app->initial_pixels,720,480);
        app->initial_pixels=NULL;
        tool_timing_end(app->role,t->window.window,"canvas-adopt",adopt_at);
    }
    t->clip.window=t->window.window;
    if(app->role==DESK_APP_FILES)desk_log("desktools: window-request role=files pid=%llu at_us=%llu\n",
        (unsigned long long)srv_getpid(),(unsigned long long)srv_monotonic_us());
    return 0;
}
/* Model ownership survives CLOSE_ACK until accepted work releases its binding. */
static int retire_job(struct desk_window_owner *owner) {
    struct tool_document *d=document(owner);struct tool *t=&d->tool;
    if(t->job.busy)return 0;
    if(!d->worker_closed) {
        if(t->worker_ready && desk_file_job_detach(&t->job))return 0;
        d->worker_closed=1;t->worker_ready=0;
        desk_log("%s-runtime: job-detached window=%llu\n",role_name(t->role),(unsigned long long)t->window.window);
    }
    return 1;
}
static void dialog_focus(struct tool *t,int focused) {
    if(t->picker_mode)desk_picker_focus(&t->picker,focused);
    else if(t->prompt_mode)desk_prompt_focus(&t->prompt,focused);
    else if(t->modal)desk_dialog_focus(&t->dialog,focused);
}
static void dialog_event(struct desk_window_owner *owner,struct desk_packet p) {
    struct tool *t=&document(owner)->tool;struct desk_action action={0};
    dialog_focus(t,owner->dialog.window.focused);
    if(p.type==DESK_CLOSE_REQUEST || p.type==DESK_WINDOW_FAILED) {tool_runtime_cancel_modal(t);return;}
    if(p.type==DESK_FOCUS)dialog_focus(t,p.value!=0);
    else if(p.type==DESK_KEY) {
        struct srv_key_event key;memcpy(&key,p.text,sizeof(key));
        if(tool_command_key(t,&key))return;
        if((key.flags&SRV_KEY_DOWN_EVENT) && (key.modifiers&(SRV_KEY_CTRL|SRV_KEY_META)) && key.usage==0x1a) {tool_runtime_cancel_modal(t);return;}
        if(t->picker_mode)tool_picker_result(t,desk_picker_key(&t->picker,&key));
        else if(t->prompt_mode)tool_files_prompt(t,desk_prompt_key(&t->prompt,&key));
        else action=desk_dialog_key(&t->dialog,&key);
    } else if(p.type==DESK_POINTER) {
        struct desk_widget_event e={.type=p.value&DESK_POINTER_CANCEL?DESK_WIDGET_CANCEL:DESK_WIDGET_POINTER,
            .x=p.x,.y=p.y,.down=(p.value&DESK_POINTER_LEFT)!=0,.pressed=(p.value&DESK_POINTER_PRESS)!=0};
        if(t->picker_mode)tool_picker_result(t,desk_picker_pointer(&t->picker,e,srv_monotonic_us()));
        else if(t->prompt_mode)tool_files_prompt(t,desk_prompt_pointer(&t->prompt,e,srv_monotonic_us()));
        else action=desk_dialog_pointer(&t->dialog,e,srv_monotonic_us());
    } else if(p.type==DESK_SCROLL && t->picker_mode)tool_picker_result(t,desk_picker_scroll(&t->picker,p.width,p.height,p.y));
    else if(p.type==DESK_CLIPBOARD_ACK || p.type==DESK_CLIPBOARD_DATA) {
        int done=desk_clipboard_receive(&t->clip,&p);if(done)tool_clipboard_result(t,done);
    }
    if(action.flags&DESK_WIDGET_ACTIVATED) {
        if(t->role==DESK_APP_FILES)tool_files_dialog(t,action.id);else tool_text_dialog(t,action.id);
    }
    owner->dialog.window.dirty=1;
}
static void dialog_tick(struct desk_window_owner *owner) {
    struct tool *t=&document(owner)->tool;
    int wanted=!owner->retired && (t->modal || t->picker_mode || t->prompt_mode);
    const char *title=t->picker_mode?(t->picker_mode==DESK_PICKER_SAVE?"Save document":"Open document"):
        t->prompt_mode?(t->prompt_mode==DESK_FILE_COPY?"Duplicate file":"Rename item"):
        t->role==DESK_APP_FILES?"Delete item":t->modal==2?"Replace file":"Unsaved document";
    int ready=desk_dialog_window_sync(&owner->dialog,&t->window,wanted,title,
        t->picker_mode?720:560,t->picker_mode?480:280);
    int previous_separate=t->dialog_separate;
    t->dialog_separate=wanted && ready>=0;
    if(previous_separate!=t->dialog_separate)t->window.dirty=1;
    struct desk_rect bounds={0,0,t->window.width,t->window.height};
    if(wanted && ready<0) {
        if(t->picker_mode)desk_picker_configure(&t->picker,bounds);
        else if(t->prompt_mode)desk_prompt_configure(&t->prompt,bounds);
        else desk_dialog_configure(&t->dialog,bounds);
        dialog_focus(t,t->window.focused);
    }
    if(!t->clip.mode)t->clip.window=t->dialog_separate && owner->dialog.window.connection?owner->dialog.window.window:t->window.window;
    if(!wanted || ready!=1)return;
    struct desk_app *window=&owner->dialog.window;
    bounds=(struct desk_rect){0,0,window->width,window->height};
    if(t->picker_mode)desk_picker_configure(&t->picker,bounds);
    else if(t->prompt_mode)desk_prompt_configure(&t->prompt,bounds);
    else desk_dialog_configure(&t->dialog,bounds);
    dialog_focus(t,window->focused);
    if(t->window.dirty)window->dirty=1;
    struct desk_canvas canvas;
    if(desk_app_canvas(window,&canvas)==1) {
        desk_fill(&canvas,canvas.clip,desk_theme_quiet.surface);
        if(t->picker_mode)desk_picker_draw(&canvas,&t->picker,&desk_theme_quiet);
        else if(t->prompt_mode)desk_prompt_draw(&canvas,&t->prompt,&desk_theme_quiet);
        else desk_dialog_draw(&canvas,&t->dialog,&desk_theme_quiet);
        desk_app_present(window);
    }
}
int tool_file_capture_capacity_setup(void);
static int window_event(void *context,struct desk_window_owner *owner,struct desk_app *window,struct desk_packet p) {
    struct tool_windows *app=context;struct tool_document *d=document(owner);struct tool *t=&d->tool;
    if(window==&owner->dialog.window) {dialog_event(owner,p);return 0;}
    if(p.type==DESK_OPEN_DOCUMENT) {desk_open_document_receive(&app->open_request,&p);return 0;}
    if(p.type==DESK_OPEN_REPLY) {
        if(p.value)notice(app,owner->requester,p.value==DESK_OPEN_CAPACITY?
            "The desktop is full. Close a window before opening another.":"The new window could not be opened. Your other windows were kept.");
        else {
            t->worker_ready=desk_file_job_bind(&t->job,&app->pool)==0;
            uint64_t timing=tool_timing_begin(app->role,window->window,"role-init");
            if(app->role==DESK_APP_TEXT)tool_text_init(t,d->initial_path);else tool_files_init(t,d->initial_path);
            tool_timing_end(app->role,window->window,"role-init",timing);
            if(d->bad_path)tool_status(t,"This path is too long. Choose another file or folder.");
            else if(!t->worker_ready)tool_status(t,"The file worker is unavailable. Your window was kept.");
            desk_log("%s-runtime: admitted window=%llu worker=%llu\n",role_name(app->role),(unsigned long long)window->window,(unsigned long long)t->job.pid);
        }
    } else if(p.type==DESK_CLOSE_ACK) {
        tool_timing_event(app->role,window->window,"close-ack");t->pending=t->closing=0;
        if(app->service_window==window->window)app->service_window=0;
        desk_log("%s-runtime: closed window=%llu bytes=%u\n",role_name(app->role),(unsigned long long)window->window,(unsigned)t->body_text.length);
    } else if(p.type==DESK_WINDOW_FAILED)notice(app,owner->requester,"A window could not be displayed. Other windows remain open.");
    else if(owner->ready && !owner->retired && !t->quit) {
        tool_runtime_event(t,p);
        if(app->capacity && p.type==DESK_KEY && p.input_serial && p.input_serial<=60 && !(p.input_serial%12))tool_file_capture_begin(t,p.input_serial);
    }
    return 0;
}
int tool_window_runtime(int role,const char *path) {
    /* One runtime per process. Its helper stack/state stay mapped through
     * normal kernel process exit even after the body acknowledges stop. */
    static struct tool_windows app;static int used;
    if(used)return 1;
    used=1;app=(struct tool_windows){.role=role,.capacity=role==DESK_APP_TEXT && !strcmp(path,"--capacity-files")};
    if(app.capacity) {if(tool_file_capture_capacity_setup())return 1;path="";}
    surface_trace_role=role;
    desk_window_set_init(&app.set);
    uint64_t timing=tool_timing_begin(role,0,"pool-init");
    int initialized=desk_file_pool_init(&app.pool,app.channels,2);
    tool_timing_end(role,0,"pool-init",timing);if(initialized)return 1;
    /* Reserve the initial model while still single-threaded. Do not publish
     * a GUI owner or lend it to the helper before startup has succeeded. */
    app.initial_owner=allocate_document(&app);if(!app.initial_owner)return 1;
    timing=tool_timing_begin(role,0,"canvas-reserve");
    struct tool_phase_trace canvas_phase;tool_phase_begin(&canvas_phase);
    app.initial_pixels=desk_pixels_alloc(720,480);
    tool_phase_end(&canvas_phase,role,0,"canvas-reserve");
    tool_timing_end(role,0,"canvas-reserve",timing);
    if(!app.initial_pixels) {desk_window_owner_release(app.initial_owner,1);return 1;}
    timing=tool_timing_begin(role,0,"file-service-init");
    pthread_trace_role=role;
    int service_started=file_service_start(&app.files,&app.pool,role);
    pthread_trace_role=0;
    tool_timing_end(role,0,"file-service-init",timing);
    if(service_started) {
        desk_pixels_free(app.initial_pixels,720,480);app.initial_pixels=NULL;
        (void)desk_window_owner_release(app.initial_owner,1);app.initial_owner=NULL;return 1;
    }
    desk_log("desktools: starting role=%s\n",role_name(role));
    if(create(&app,0,path)) {
        desk_pixels_free(app.initial_pixels,720,480);app.initial_pixels=NULL;
        file_service_quiesce(&app.files);return 1;
    }
    while(app.set.windows || file_service_poll(&app.files)) {
        file_service_poll(&app.files);
        if(app.files.retire_failed)app.set.failed=1;
        if(!app.set.failed)desk_window_set_dispatch(&app.set,window_event,&app);
        desk_open_document_tick(&app.open_request,app.set.connection.server);
        if(!app.set.failed && app.open_request.ready && !app.set.connection.opening) {
            int accepted=!app.set.lifecycle.quitting && !create(&app,0,app.open_request.path);
            desk_log("%s-runtime: open-document accepted=%u\n",role_name(role),accepted);desk_open_document_finish(&app.open_request,accepted);
        }
        int busy=app.set.failed || app.set.connection.opening!=NULL || app.open_request.reply_pending || app.open_request.ready,drained=1;
        struct desk_window_owner *owner=app.set.windows;
        while(owner) {
            struct desk_window_owner *next=owner->next;struct tool_document *d=document(owner);struct tool *t=&d->tool;
            if(app.set.failed) {owner->retired=1;t->pending=t->closing=0;}
            if(role==DESK_APP_FILES && (t->closing || owner->retired) && desk_file_job_cancel_unsubmitted(&t->job)) {
                file_service_read_cancelled(&app.files);
                desk_log("file-job: cancelled-unsubmitted role=files window=%llu op=%u id=%u\n",(unsigned long long)t->window.window,t->job.request.operation,t->job.request.id);
            }
            if(role==DESK_APP_FILES && owner->ready && !owner->retired && !t->quit && !app.set.failed && !t->window.serial)
                tool_runtime_draw(t);
            if(t->job.busy && (owner->retired || app.set.failed || tool_runtime_file_ready(t))) {
                int result=tool_runtime_file_poll(t,&d->logged_job);
                if(result) {
                    if(owner->retired) {
                        desk_chooser_cancel(&t->chooser);
                        if(t->chooser.pending)desk_chooser_finish(&t->chooser,t->chooser.pending_id,result);
                    } else if(app.capacity && tool_file_capture_busy()) {
                        tool_file_capture_finish(t,result);
                        if(!tool_file_capture_busy() && !t->window.failed)app.capture_checkpoints++;
                    } else if(t->chooser.pending)tool_picker_finish(t,result);
                    else if(role==DESK_APP_TEXT)tool_text_finish(t,result);else tool_files_finish(t,result);
                }
            }
            if(app.capacity && t->window.failed)app.set.failed=1;
            if(t->close_canceled) {desk_application_cancel_quit(&app.set.lifecycle,t->window.window);t->close_canceled=0;}
            if(t->quit_application) {t->quit_application=0;desk_application_quit(&app.set.lifecycle);}
            if(!app.set.failed)dialog_tick(owner);
            if(owner->retired) {
                if(!app.set.failed && owner->dialog.window.connection) {busy=1;owner=next;continue;}
                if(!app.set.failed && !owner->disposed) {
                    if(!file_service_capacity(&app.files)) {busy=1;drained=0;owner=next;continue;}
                    timing=tool_timing_begin(role,t->window.window,"window-dispose");
                    struct file_service_release canvas={.service=&app.files,.window=t->window.window,.canvas=1};
                    int disposed=desk_window_owner_dispose_with(owner,file_service_retire,&canvas);tool_timing_end(role,t->window.window,"window-dispose",timing);
                    if(disposed) {app.set.failed=1;owner=next;continue;}
                }
                if(!retire_job(owner)) {busy=1;drained=0;owner=next;continue;}
                /* A dead/failed connection cannot prove GUI resource ownership.
                 * Drain every accepted job, then let process exit reclaim models. */
                if(app.set.failed) {owner=next;continue;}
                if(!file_service_capacity(&app.files)) {busy=1;owner=next;continue;}
                uint64_t window=t->window.window;
                timing=tool_timing_begin(role,window,"history-free");tool_text_history_release(t);
                tool_timing_end(role,window,"history-free",timing);
                struct file_service_release release={.service=&app.files,.window=window};
                int released=desk_window_owner_release_with(owner,1,file_service_retire,&release);
                if(released)app.set.failed=1;
                owner=next;continue;
            }
            drained=0;
            if(owner->ready && !app.set.failed) {
                if(role==DESK_APP_TEXT && !app.capacity && !app.set.lifecycle.quitting && !app.service_window && !t->window.closing) {
                    int first=!t->window.serial;uint64_t service_at=0;struct tool_phase_trace phase;
                    if(first) {service_at=tool_timing_begin(role,t->window.window,"first-service");tool_phase_begin(&phase);}
                    int registered=desk_application_service(&t->window,DESK_SERVICE_OPEN_DOCUMENT);
                    if(first) {tool_phase_end(&phase,role,t->window.window,"first-service");tool_timing_end(role,t->window.window,"first-service",service_at);}
                    if(!registered)app.service_window=t->window.window;
                }
                if(app.set.lifecycle.quitting && app.service_window==t->window.window && !desk_application_service(&t->window,0))app.service_window=0;
                int close_ready=tool_runtime_close_ready(t);
                if(t->closing && close_ready && role==DESK_APP_FILES)t->quit=1;
                if(t->quit && close_ready)desk_window_close(&t->window);
                if(!t->quit && !t->window.closing) {
                    if(desk_clipboard_tick(&t->clip,srv_ticks())<0)tool_clipboard_result(t,-1);
                    tool_runtime_draw(t);
                }
                busy|=t->clip.mode || t->title_dirty || t->quit || t->window.closing || t->new_window;
            }
            owner=next;
        }
        if(app.set.failed && drained)break;
        struct desk_window_owner *closing=desk_window_set_next_close(&app.set);
        if(closing) {tool_runtime_close_request(&document(closing)->tool);busy=1;}
        if(!app.set.failed && !app.set.lifecycle.quitting && !app.set.connection.opening) {
            for(owner=app.set.windows;owner;owner=owner->next) {
                struct tool *t=&document(owner)->tool;
                if(t->new_window && !owner->retired && !t->quit) {
                    t->new_window=0;create(&app,t->window.window,role==DESK_APP_FILES?t->saved_path:"");busy=1;break;
                }
            }
        }
        /* Every model is detached only after its accepted job drained. Start
         * idle-worker EOF/reaping while the helper still owns queued unmaps.
         * Final finish below retains the full child/completion obligation. */
        if(!app.set.windows && !app.set.connection.opening && !app.pool.bound && !app.pool.head) {
            if(desk_file_pool_shutdown(&app.pool)<0)app.set.failed=1;
            file_service_request_stop(&app.files);
        }
        if(app.set.windows) {desk_runtime_file_pool(&app.set.runtime,&app.pool);desk_runtime_idle(&app.set.runtime,busy);}
        else if(file_service_poll(&app.files))srv_sleep_ticks(1);
    }
    desk_runtime_timers_clear(&app.set.runtime);
    timing=tool_timing_begin(role,0,"file-service-stop");
    if(file_service_quiesce(&app.files))app.set.failed=1;
    tool_timing_end(role,0,"file-service-stop",timing);
    desk_log("%s-runtime: documents admitted=%u retired=%u refused=%u\n",role_name(role),(unsigned)app.set.admitted,(unsigned)app.set.retired,(unsigned)app.set.refused);
    if(tool_file_pool_retire(&app.pool,role))app.set.failed=1;
    if(app.capacity) {
        if(app.set.admitted!=1 || app.set.retired!=1 || app.set.refused || app.capture_checkpoints!=5 || !tool_file_capture_report())app.set.failed=1;
        desk_log("document-capacity: result=%s checkpoints=%u pooled=yes retired=%u\n",app.set.failed?"FAIL":"PASS",app.capture_checkpoints,(unsigned)app.set.retired);
    }
    desk_log("desktools: exited role=%s failure=%d\n",role_name(role),app.set.failed);return app.set.failed?1:0;
}
int tool_text_runtime(const char *path) {return tool_window_runtime(DESK_APP_TEXT,path);}
