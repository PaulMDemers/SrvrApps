#include <srvros/desk_theme_preferences.h>
#include <srvros/desk_app.h>
#include <srvros/desk_ui.h>
#include <srvros/desk_chooser.h>
#include <srvros/desk_log.h>
#include <srvros/profile_session.h>
#include <srvros/sys.h>
#include <cJSON.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIMIT 64
#define PATH_CAP DESK_PICKER_PATH
enum { TABS=1, ALIAS, ADD, REMOVE, SOURCE, REVIEW, BACK, CONFIRM, LIST, REPORT, IMPORT, EXPORT, CLOSE };
enum { EDITING, PREVIEWING, REVIEWING, TRANSFERRING, COMPLETE, FAILED };
enum { PICK_DOCUMENT=1, PICK_BUNDLE, PICK_OUTPUT };
struct stream { int fd,eof,truncated; size_t used,cap; char *data; };
struct request {
    int mutate,importing,session_valid; unsigned count;
    struct srv_profile_session session;
    char docs[LIMIT][PATH_CAP],bundle[PATH_CAP],output[PATH_CAP],home[PATH_CAP],alias[96],fingerprint[65],uid[24],gid[24],diagnostic[64];
};
struct app {
    struct desk_app window; struct desk_ui ui; struct desk_tabs tabs;
    struct desk_edit alias; struct desk_text_buffer alias_text; char alias_storage[2][96];
    struct desk_button add,remove,source,review,back,confirm; struct desk_list list;
    struct desk_list_item doc_items[LIMIT]; char docs[LIMIT][PATH_CAP]; unsigned count;
    struct desk_document report; char report_text[6][4096];
    struct desk_file_picker picker; struct desk_chooser chooser; struct desk_file_job file_job;
    struct desk_file_entry entries[DESK_FILE_ENTRIES]; struct desk_list_item items[DESK_FILE_ENTRIES]; char details[DESK_FILE_ENTRIES][32];
    int file_ready,picker_open,pick_kind,state,quit,close_pending;
    char bundle[PATH_CAP],output[PATH_CAP],home[PATH_CAP],fingerprint[65],uid[24],gid[24],status[256];
    char out[16385],err[4097],diagnostic[64]; struct stream stdout_stream,stderr_stream;
    uint64_t pid,exit_status,now; int reaped,transport_error; struct srv_profile_session session; int session_valid;
    pthread_t worker,picker_worker; int worker_active,worker_done,picker_starting,picker_done,picker_result; struct request request;
};
static struct app a;
static int importing(void) { return desk_tabs_selected(&a.tabs)==IMPORT; }
static const char *operation(void) { return importing()?"import":"export"; }
static void status(const char *s) { snprintf(a.status,sizeof(a.status),"%s",s);a.window.dirty=1; }
static void trace(const char *state,const char *result) { desk_log("profile-ui: state=%s operation=%s result=%s\n",state,operation(),result); }
static void rows(void) { for(unsigned i=0;i<a.count;i++) a.doc_items[i]=(struct desk_list_item){.label=a.docs[i],.id=i+1};desk_list_set_items(&a.list,a.doc_items,a.count); }
static void layout(void) {
    int w=a.window.width,h=a.window.height,edit=a.state==EDITING&&!a.picker_starting&&!a.close_pending,view=a.state!=EDITING,busy=a.state==PREVIEWING||a.state==TRANSFERRING,imp=importing();
    desk_tabs_configure(&a.tabs,(struct desk_rect){12,10,w-24,30},edit&&!a.picker_open);
    desk_edit_configure(&a.alias,edit&&imp?(struct desk_rect){112,92,w-124,28}:(struct desk_rect){0},&desk_font_16,edit&&imp);
    desk_button_configure(&a.source,edit?(struct desk_rect){12,50,116,28}:(struct desk_rect){0},edit);
    desk_button_configure(&a.add,edit&&!imp?(struct desk_rect){12,92,80,28}:(struct desk_rect){0},edit&&!imp&&a.count<LIMIT);
    desk_button_configure(&a.remove,edit&&!imp?(struct desk_rect){100,92,80,28}:(struct desk_rect){0},edit&&!imp&&a.count>0);
    desk_list_configure(&a.list,edit&&!imp?(struct desk_rect){12,128,w-24,h-208}:(struct desk_rect){0},edit&&!imp);
    desk_document_configure(&a.report,view?(struct desk_rect){12,50,w-24,h-((a.state==REVIEWING&&!imp)?166:130)}:(struct desk_rect){0},&desk_font_16,view);
    desk_button_configure(&a.review,edit?(struct desk_rect){w-112,h-68,100,28}:(struct desk_rect){0},edit);
    desk_button_configure(&a.back,view&&!busy?(struct desk_rect){12,h-68,80,28}:(struct desk_rect){0},view&&!busy);
    desk_button_configure(&a.confirm,a.state==REVIEWING?(struct desk_rect){w-112,h-68,100,28}:(struct desk_rect){0},a.state==REVIEWING);
    a.ui.controls[4].label=imp?"Choose bundle":"Destination";
    a.ui.controls[7].label=imp?"Import":"Export";
    desk_picker_configure(&a.picker,(struct desk_rect){0,0,w,h});desk_ui_layout_changed(&a.ui);
    desk_ui_window_focus(&a.ui,a.window.focused&&!a.picker_open);desk_picker_focus(&a.picker,a.window.focused&&a.picker_open);a.window.dirty=1;
}
static void report(const char *heading,const char *text) {
    snprintf(a.report_text[0],sizeof(a.report_text[0]),"%s",heading);snprintf(a.report_text[1],sizeof(a.report_text[1]),"%s",text);
    struct desk_document_block b[]={{1,DESK_DOCUMENT_HEADING,a.report_text[0]},{2,DESK_DOCUMENT_PARAGRAPH,a.report_text[1]}};desk_document_set(&a.report,b,2);a.window.dirty=1;
}
static void stream_init(struct stream *s,int fd,char *data,size_t cap) { *s=(struct stream){.fd=fd,.cap=cap,.data=data};data[0]=0; }
static void drain(struct stream *s) {
    for(unsigned i=0;i<4&&!s->eof;i++) { char buf[4096];long n=srv_read(s->fd,buf,sizeof(buf));
        if(n==SRV_ERR_AGAIN||n==SRV_ERR_INTR) break;
        if(n<0 || n>(long)sizeof(buf)) { a.transport_error=1;srv_close(s->fd);s->eof=1;break; }
        if(!n) { srv_close(s->fd);s->eof=1;break; }
        size_t keep=(size_t)n;if(keep>s->cap-1-s->used) { keep=s->cap-1-s->used;s->truncated=1; }
        memcpy(s->data+s->used,buf,keep);s->used+=keep;s->data[s->used]=0;
    }
}
/* The worker owns its immutable snapshot, descriptors and child until EOF and reap. */
static void *command_worker(void *unused) {
    (void)unused;struct request *r=&a.request;struct srv_profile_environment env;struct srv_profile_session session=r->session;
    if(!r->session_valid) { if(!r->importing || !srv_profile_session_init(&session,SRV_PROFILE_LEGACY_RAM_HOME,NULL)) goto failed; }
    if(!srv_profile_environment_init(&env,&session,"/fat/bin")) goto failed;
    char stop[96];if(r->diagnostic[0]) { snprintf(stop,sizeof(stop),"SRVROS_PROFILE_STOP_AFTER=%s",r->diagnostic);env.envp[env.count++]=stop;env.envp[env.count]=NULL; }
    char *argv[LIMIT+10];unsigned n=0;argv[n++]="/fat/bin/profile";
    if(r->importing) { argv[n++]=r->mutate?"import-reviewed":"preview-import";argv[n++]=r->bundle;argv[n++]=r->home;argv[n++]=r->alias;
        if(r->mutate) { argv[n++]=r->uid;argv[n++]=r->gid;argv[n++]=r->fingerprint; }
    } else { argv[n++]=r->mutate?"export-reviewed":"preview-export";argv[n++]=r->session.home;
        if(r->mutate) { argv[n++]=r->output;argv[n++]=r->fingerprint; }
        for(unsigned i=0;i<r->count;i++)argv[n++]=r->docs[i];
    } argv[n]=NULL;
    int in[2],out[2],err[2];if(srv_pipe(in))goto failed;if(srv_pipe(out)) { srv_close(in[0]);srv_close(in[1]);goto failed; }
    if(srv_pipe(err)) { srv_close(in[0]);srv_close(in[1]);srv_close(out[0]);srv_close(out[1]);goto failed; }
    if(srv_fcntl(out[0],SRV_F_SETFL,SRV_FD_NONBLOCK)<0||srv_fcntl(err[0],SRV_F_SETFL,SRV_FD_NONBLOCK)<0) {
        srv_close(in[0]);srv_close(in[1]);srv_close(out[0]);srv_close(out[1]);srv_close(err[0]);srv_close(err[1]);goto failed;
    }
    struct srv_exec_request req={.path=argv[0],.argv=argv,.envp=env.envp,.flags=SRV_EXEC_BACKGROUND,.stdin_fd=in[0],.stdout_fd=out[1],.stderr_fd=err[1],.process_group=UINT64_MAX};
    long pid=srv_exec(&req);srv_close(in[0]);srv_close(in[1]);srv_close(out[1]);srv_close(err[1]);
    if(pid<=0) { srv_close(out[0]);srv_close(err[0]);goto failed; }
    a.pid=pid;stream_init(&a.stdout_stream,out[0],a.out,sizeof(a.out));stream_init(&a.stderr_stream,err[0],a.err,sizeof(a.err));
    int no_child=0;
    while((!a.reaped&&!no_child)||!a.stdout_stream.eof||!a.stderr_stream.eof) {
        drain(&a.stdout_stream);drain(&a.stderr_stream);
        if(!a.reaped&&!no_child) { long result=srv_wait(a.pid,&a.exit_status,SRV_WAIT_NOHANG);if(result==(long)a.pid)a.reaped=1;
            else if(result==-1||result==-ECHILD) { no_child=1;a.transport_error=1; }
            else if(result<0&&result!=SRV_ERR_INTR&&result!=SRV_ERR_AGAIN)a.transport_error=1; }
        if((!a.reaped&&!no_child)||!a.stdout_stream.eof||!a.stderr_stream.eof)srv_sleep_ticks(1);
    }
    __atomic_store_n(&a.worker_done,1,__ATOMIC_RELEASE);return NULL;
failed:
    a.transport_error=1;__atomic_store_n(&a.worker_done,1,__ATOMIC_RELEASE);return NULL;
}
static int start(int mutate) {
    struct request *r=&a.request;memset(r,0,sizeof(*r));r->mutate=mutate;r->importing=importing();r->session_valid=a.session_valid;r->session=a.session;r->count=a.count;
    memcpy(r->docs,a.docs,sizeof(r->docs));snprintf(r->bundle,sizeof(r->bundle),"%s",a.bundle);snprintf(r->output,sizeof(r->output),"%s",a.output);
    snprintf(r->home,sizeof(r->home),"%s",a.home);snprintf(r->alias,sizeof(r->alias),"%s",a.alias_text.data);snprintf(r->fingerprint,sizeof(r->fingerprint),"%s",a.fingerprint);
    snprintf(r->uid,sizeof(r->uid),"%s",a.uid);snprintf(r->gid,sizeof(r->gid),"%s",a.gid);snprintf(r->diagnostic,sizeof(r->diagnostic),"%s",a.diagnostic);
    a.pid=0;a.reaped=a.transport_error=0;a.exit_status=0;a.out[0]=a.err[0]=0;memset(&a.stdout_stream,0,sizeof(a.stdout_stream));memset(&a.stderr_stream,0,sizeof(a.stderr_stream));
    __atomic_store_n(&a.worker_done,0,__ATOMIC_RELAXED);
    if(pthread_create(&a.worker,NULL,command_worker,NULL))return -1;
    a.worker_active=1;a.state=mutate?TRANSFERRING:PREVIEWING;report(mutate?"Transfer in progress":"Preparing review","Please wait. This window remains open until the operation finishes.");
    status("Working…");trace(mutate?"transferring":"previewing","started");layout();return 0;
}
static const char *json_string(cJSON *o,const char *key) { cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsString(v)?v->valuestring:NULL; }
static int integer(cJSON *v,unsigned *out) { if(!cJSON_IsNumber(v)||v->valuedouble<0||v->valuedouble>2147483647||v->valuedouble!=(double)v->valueint) return 0;*out=(unsigned)v->valueint;return 1; }
static int parse_review(void) {
    if(a.stdout_stream.truncated || memchr(a.out,0,a.stdout_stream.used)) return -1;
    const char *end=NULL;cJSON *j=cJSON_ParseWithOpts(a.out,&end,0);if(!j)return -1;while(end&&(*end==' '||*end=='\n'||*end=='\r'||*end=='\t'))end++;
    const char *format=json_string(j,"format"),*op=json_string(j,"operation"),*hash=json_string(j,"sha256");
    cJSON *profile=cJSON_GetObjectItemCaseSensitive(j,"profile"),*prefs=cJSON_GetObjectItemCaseSensitive(j,"preferences"),*docs=cJSON_GetObjectItemCaseSensitive(j,"documents"),*dest=cJSON_GetObjectItemCaseSensitive(j,"destination");
    const char *id=json_string(profile,"id"),*name=json_string(profile,"display_name");unsigned bytes=0;
    if(!end||*end||!format||strcmp(format,"srvros-profile-review-v1")||!op||strcmp(op,operation())||!hash||strlen(hash)!=64||!id||!srv_profile_session_id_valid(id)||!name||!cJSON_IsObject(prefs)||!cJSON_IsArray(docs)||cJSON_GetArraySize(docs)>LIMIT||!integer(cJSON_GetObjectItemCaseSensitive(j,"bytes"),&bytes)) goto bad;
    for(unsigned i=0;i<64;i++) if(!((hash[i]>='0'&&hash[i]<='9')||(hash[i]>='a'&&hash[i]<='f')))goto bad;
    if(importing()) { unsigned uid,gid;const char *home=json_string(dest,"home"),*alias=json_string(dest,"alias");
        if(!home||strcmp(home,a.home)||!alias||strcmp(alias,a.alias_text.data)||!integer(cJSON_GetObjectItemCaseSensitive(dest,"uid"),&uid)||!uid||!integer(cJSON_GetObjectItemCaseSensitive(dest,"gid"),&gid)||!gid) goto bad;
        snprintf(a.uid,sizeof(a.uid),"%u",uid);snprintf(a.gid,sizeof(a.gid),"%u",gid);
    }
    snprintf(a.fingerprint,sizeof(a.fingerprint),"%s",hash);
    snprintf(a.report_text[0],sizeof(a.report_text[0]),"%s\nProfile: %s\nIdentity: %s\n%s: %s\nDestination: %s\nTotal: %u bytes",importing()?"Review import":"Review export",name,id,importing()?"Source bundle":"Source",importing()?a.bundle:a.session.home,importing()?a.home:a.output,bytes);
    static const struct { const char *key,*label;const char *values[4];unsigned max; } settings[]={
        {"theme_style","Style",{"Silver","Aero","Aqua"},2},
        {"theme_accent","Accent",{"Blue","Graphite","Teal"},2},
        {"glass_level","Glass",{"Solid","Subtle","Clear"},2},
        {"wallpaper","Desktop color",{"Blue","Sand","Slate"},2}
    };
    size_t pref_used=(size_t)snprintf(a.report_text[1],sizeof(a.report_text[1]),"Portable preferences:\n");unsigned found=0;
    for(unsigned i=0;i<sizeof(settings)/sizeof(*settings);i++) { cJSON *v=cJSON_GetObjectItemCaseSensitive(prefs,settings[i].key);unsigned value;if(!v)continue;
        if(!integer(v,&value)||value>settings[i].max)goto bad;found++;pref_used+=(size_t)snprintf(a.report_text[1]+pref_used,sizeof(a.report_text[1])-pref_used,"%s: %s\n",settings[i].label,settings[i].values[value]); }
    cJSON *v=cJSON_GetObjectItemCaseSensitive(prefs,"text_size");unsigned value;
    if(v) { if(!integer(v,&value)||(value!=16&&value!=20))goto bad;found++;pref_used+=(size_t)snprintf(a.report_text[1]+pref_used,sizeof(a.report_text[1])-pref_used,"Text size: %s\n",value==16?"Standard":"Larger"); }
    v=cJSON_GetObjectItemCaseSensitive(prefs,"clock_flags");if(v) { if(!integer(v,&value)||value>3)goto bad;found++;pref_used+=(size_t)snprintf(a.report_text[1]+pref_used,sizeof(a.report_text[1])-pref_used,"24-hour clock: %s\nShow date: %s\n",value&1?"On":"Off",value&2?"On":"Off"); }
    if(!found)snprintf(a.report_text[1]+pref_used,sizeof(a.report_text[1])-pref_used,"No portable preferences in this profile.");
    memset(a.report_text[2],0,sizeof(a.report_text[2]));memset(a.report_text[3],0,sizeof(a.report_text[3]));memset(a.report_text[4],0,sizeof(a.report_text[4]));size_t used[3]={0,0,0};unsigned idx=0;
    cJSON *d; cJSON_ArrayForEach(d,docs) { const char *path=json_string(d,"path");unsigned size;if(!path||!integer(cJSON_GetObjectItemCaseSensitive(d,"size"),&size))goto bad;
        int n=snprintf(a.report_text[2+idx]+used[idx],sizeof(a.report_text[2])-used[idx],"%s — %u bytes\n",path,size);
        if(n<0||(size_t)n>=sizeof(a.report_text[2])-used[idx]) { a.report_text[2+idx][used[idx]]=0;if(idx==2)goto bad;idx++;n=snprintf(a.report_text[2+idx],sizeof(a.report_text[2+idx]),"%s — %u bytes\n",path,size);if(n<0||(size_t)n>=sizeof(a.report_text[2+idx]))goto bad; }used[idx]+=(size_t)n;
    }
    snprintf(a.report_text[5],sizeof(a.report_text[5]),"%s",importing()?"Confirm to import. The profile will be available in the next startup chooser.":"Selected document bytes may contain sensitive content. Confirm to create a new bundle; existing files are never overwritten.");
    struct desk_document_block b[]={{1,DESK_DOCUMENT_HEADING,a.report_text[0]},{2,DESK_DOCUMENT_PARAGRAPH,a.report_text[1]},{3,DESK_DOCUMENT_PARAGRAPH,a.report_text[2][0]?a.report_text[2]:"No documents selected."},{4,DESK_DOCUMENT_PARAGRAPH,a.report_text[3]},{5,DESK_DOCUMENT_PARAGRAPH,a.report_text[4]},{6,DESK_DOCUMENT_NOTE,a.report_text[5]}};
    if(desk_document_set(&a.report,b,6))goto bad;cJSON_Delete(j);return 0;
bad:cJSON_Delete(j);return -1;
}
static void poll_child(void) {
    if(!a.worker_active||!__atomic_load_n(&a.worker_done,__ATOMIC_ACQUIRE))return;
    (void)pthread_join(a.worker,NULL);a.worker_active=0;int was=a.state;
    if(a.reaped)desk_log("profile-ui: state=worker operation=%s result=reaped status=%llu stdout_eof=1 stderr_eof=1\n",operation(),(unsigned long long)a.exit_status);
    for(size_t i=0;i<a.stderr_stream.used;i++) { unsigned char c=a.err[i];if((c<32&&c!='\n'&&c!='\t')||c>=127)a.err[i]='?'; }
    if(a.transport_error||a.exit_status || (was==PREVIEWING&&parse_review())) {
        a.state=FAILED;
        snprintf(a.report_text[0],sizeof(a.report_text[0]),"%s",was==TRANSFERRING?"Transfer did not complete":"Review could not be prepared");
        const char *error=a.err[0]?a.err:"The profile operation could not be completed.";size_t length=strlen(error),first=length>4000?4000:length;
        memcpy(a.report_text[1],error,first);a.report_text[1][first]=0;snprintf(a.report_text[2],sizeof(a.report_text[2]),"%s%s",error+first,a.stderr_stream.truncated?"\n[Further error output was truncated.]":"");
        snprintf(a.report_text[3],sizeof(a.report_text[3]),"%s",was!=TRANSFERRING?"Go back to check the inputs and try review again.":importing()?"If an import was interrupted, preserve the original bundle. Restart and use the existing profile chooser recovery before trying again.":"An incomplete bundle may remain at the destination. Keep it and choose a fresh filename before retrying.");
        struct desk_document_block b[]={{1,DESK_DOCUMENT_HEADING,a.report_text[0]},{2,DESK_DOCUMENT_PARAGRAPH,a.report_text[1]},{3,DESK_DOCUMENT_PARAGRAPH,a.report_text[2]},{4,DESK_DOCUMENT_NOTE,a.report_text[3]}};desk_document_set(&a.report,b,4);
        status("Read the details, then go back to your inputs.");trace("editing","error");if(a.diagnostic[0])desk_log("profile-ui: checkpoint=%s result=error\n",a.diagnostic);
    } else if(was==PREVIEWING) { a.state=REVIEWING;status("Review all details, then confirm or go back.");trace("reviewing","ready"); }
    else { a.state=COMPLETE;report("Transfer complete",importing()?"The profile is available in the next startup chooser. The current session has not changed.":"The profile bundle was created at the selected destination.");status("Transfer complete.");trace("complete","success"); }
    layout();if(a.close_pending)a.quit=1;
}
static int alias_valid(const char *s) { size_t n=strlen(s);if(!n||n>32||s[0]<'a'||s[0]>'z')return 0;for(size_t i=0;i<n;i++)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='0'&&s[i]<='9')||s[i]=='_'||s[i]=='-'))return 0;return 1; }
static void begin_review(void) {
    if(importing()) { if(!a.bundle[0]) { status("Choose a profile bundle first.");return; }if(!alias_valid(a.alias_text.data)) { status("Use a local name starting with a lowercase letter; use lowercase letters, digits, - or _ (up to 32). ");return; }
        snprintf(a.home,sizeof(a.home),"/fat/home/%s",a.alias_text.data);
    } else if(!a.session_valid||!a.session.id[0]) { status("Export requires an active profile. Import is available on this desktop.");return; }
    else if(!a.output[0]) { status("Choose a new destination filename first.");return; }
    if(start(0))status("Could not start profile review.");
}
static void *picker_init_worker(void *unused) {
    (void)unused;if(a.file_ready&&a.file_job.broken) { desk_file_job_close(&a.file_job);a.file_ready=0; }
    a.picker_result=a.file_ready?0:desk_file_job_init(&a.file_job);if(!a.picker_result)a.file_ready=1;
    __atomic_store_n(&a.picker_done,1,__ATOMIC_RELEASE);return NULL;
}
static void picker_ready(void) {
    if(!a.picker_starting||!__atomic_load_n(&a.picker_done,__ATOMIC_ACQUIRE))return;
    (void)pthread_join(a.picker_worker,NULL);a.picker_starting=0;
    if(a.close_pending) { a.quit=1;return; }
    if(a.picker_result) { status("Could not open the folder browser.");layout();return; }
    int kind=a.pick_kind;char dir[PATH_CAP];if(kind==PICK_DOCUMENT)snprintf(dir,sizeof(dir),"%s/Documents",a.session.home);else snprintf(dir,sizeof(dir),"%s",a.session_valid?a.session.home:"/fat/home");
    if(desk_chooser_open(&a.chooser,kind==PICK_OUTPUT?DESK_PICKER_SAVE:DESK_PICKER_OPEN,dir,"")) { status("Could not open the file picker.");layout();return; }
    a.picker_open=1;layout();(void)desk_chooser_browse(&a.chooser);desk_log("profile-ui: state=picker operation=%s result=opened kind=%d\n",operation(),kind);
}
static void picker_open(int kind) {
    if(a.close_pending||a.picker_open||a.picker_starting||a.file_job.busy)return;
    a.pick_kind=kind;a.picker_starting=1;__atomic_store_n(&a.picker_done,0,__ATOMIC_RELAXED);
    if(pthread_create(&a.picker_worker,NULL,picker_init_worker,NULL)) { a.picker_starting=0;status("Could not start the folder browser.");return; }
    status("Opening folder browser…");layout();
}
static void picker_close(void) { a.picker_open=0;desk_chooser_cancel(&a.chooser);layout(); }
static void picker_action(struct desk_picker_result r) {
    if(r.flags&DESK_WIDGET_REDRAW)a.window.dirty=1;
    struct desk_chooser_selection selected;int outcome=desk_chooser_action(&a.chooser,r,&selected);
    if(outcome==DESK_CHOOSER_CANCELED) { picker_close();status("Selection canceled.");return; }
    if(outcome!=DESK_CHOOSER_ACCEPTED)return;
    int kind=a.pick_kind;picker_close();
    if(kind==PICK_DOCUMENT) {
        size_t n=strlen(a.session.home);
        if(strncmp(selected.path,a.session.home,n)||strncmp(selected.path+n,"/Documents/",11)) { status("Choose a file inside this profile's Documents folder.");return; }
        const char *relative=selected.path+n+1;for(unsigned i=0;i<a.count;i++)if(!strcmp(a.docs[i],relative)) { status("That document is already selected.");return; }
        if(a.count>=LIMIT) { status("At most 64 documents may be selected.");return; }
        snprintf(a.docs[a.count++],PATH_CAP,"%s",relative);rows();status("Document added. Add more files or review.");
    } else if(kind==PICK_BUNDLE) { snprintf(a.bundle,sizeof(a.bundle),"%s",selected.path);status("Bundle selected. Enter a local name and review."); }
    else { snprintf(a.output,sizeof(a.output),"%s",selected.path);status("Destination selected. Existing files cannot be overwritten."); }
    desk_log("profile-ui: state=picker operation=%s result=selected kind=%d\n",operation(),kind);layout();
}
static void request_close(void) { if(a.worker_active||a.picker_starting) { a.close_pending=1;status("Finishing the operation before closing this window…");desk_app_cancel_close(&a.window);trace("closing","waiting"); }else a.quit=1; }
static void action(struct desk_action x) {
    if(x.flags&DESK_WIDGET_REDRAW)a.window.dirty=1;
    if(a.close_pending||a.picker_starting)return;
    if(x.id==TABS&&(x.flags&DESK_WIDGET_CHANGED)) { layout();status(importing()?"Choose a bundle and local name.":"Select Documents files and a new destination.");return; }
    if(!(x.flags&DESK_WIDGET_ACTIVATED))return;
    switch(x.id) {
    case SOURCE:picker_open(importing()?PICK_BUNDLE:PICK_OUTPUT);break;
    case ADD:if(!a.session_valid||!a.session.id[0])status("Export requires an active profile.");else picker_open(PICK_DOCUMENT);break;
    case REMOVE:if(a.list.selected>=0&&(unsigned)a.list.selected<a.count) { unsigned i=(unsigned)a.list.selected;if(i+1<a.count)memmove(a.docs[i],a.docs[i+1],(a.count-i-1)*sizeof(a.docs[0]));a.count--;rows();status("Document removed from selection.");layout(); }break;
    case REVIEW:begin_review();break;
    case BACK:a.state=EDITING;status("Inputs retained. Change them or review again.");trace("editing","back");layout();break;
    case CONFIRM:if(a.state==REVIEWING&&start(1))status("Could not start the transfer.");break;
    case CLOSE:request_close();break;
    }
}
static void draw(struct desk_canvas *c) {
    struct desk_theme theme=desk_app_theme(&a.window);desk_fill(c,(struct desk_rect){0,0,c->width,c->height},theme.surface);desk_ui_draw(c,&a.ui,&theme);
    if(a.state==EDITING) {
        desk_label_ellipsis(c,(struct desk_rect){140,53,c->width-152,24},&desk_font_16,importing()?(a.bundle[0]?a.bundle:"No bundle selected"):(a.output[0]?a.output:"No destination selected"),theme.muted);
        if(importing()) { desk_label_draw(c,(struct desk_rect){12,96,96,22},&desk_font_16,"Local name",theme.muted);desk_label_ellipsis(c,(struct desk_rect){12,136,c->width-24,24},&desk_font_16,"The imported profile will appear in the next startup chooser.",theme.muted); }
    }
    if(a.state==REVIEWING&&!importing()) {
        desk_label_draw(c,(struct desk_rect){12,c->height-112,c->width-24,20},&desk_font_16,"Selected files may contain",theme.text);
        desk_label_draw(c,(struct desk_rect){12,c->height-92,c->width-24,20},&desk_font_16,"sensitive content.",theme.text);
    }
    desk_label_ellipsis(c,(struct desk_rect){12,c->height-30,c->width-24,22},&desk_font_16,a.status,theme.muted);
    if(a.picker_open)desk_picker_draw(c,&a.picker,&theme);
}
static int diagnostic_valid(const char *s) { static const char *allowed[]={"journal","directories","document","identity","commit-ready","export-partial"};for(unsigned i=0;i<sizeof(allowed)/sizeof(*allowed);i++)if(!strcmp(s,allowed[i]))return 1;return 0; }
int main(int argc,char **argv) {
    for(int i=1;i<argc;i++) { if(i==1&&!strcmp(argv[i],"profiles"))continue;if(i==2&&!argv[i][0]&&!strcmp(argv[1],"profiles"))continue;if(!strncmp(argv[i],"--stop-after=",13)&&diagnostic_valid(argv[i]+13)&&!a.diagnostic[0])snprintf(a.diagnostic,sizeof(a.diagnostic),"%s",argv[i]+13);else return 1; }
    a.session_valid=srv_profile_session_from_env(&a.session);desk_ui_init(&a.ui);desk_tabs_init(&a.tabs);
    const struct desk_tab tabs[]={{IMPORT,"Import",0},{EXPORT,"Export",0}};desk_tabs_items(&a.tabs,tabs,2);
    desk_text_buffer_init(&a.alias_text,a.alias_storage[0],a.alias_storage[1],sizeof(a.alias_storage[0]));desk_edit_init(&a.alias,&a.alias_text,0);
    desk_list_init(&a.list);desk_document_init(&a.report);desk_picker_init(&a.picker);desk_chooser_init(&a.chooser,&a.picker,&a.file_job,a.entries,a.items,a.details);
    struct desk_button *buttons[]={&a.add,&a.remove,&a.source,&a.review,&a.back,&a.confirm};for(unsigned i=0;i<6;i++)desk_button_centered(buttons[i],1);
    desk_ui_add(&a.ui,TABS,DESK_CONTROL_TABS,&a.tabs,NULL);desk_ui_add(&a.ui,ALIAS,DESK_CONTROL_EDIT,&a.alias,NULL);
    desk_ui_add(&a.ui,ADD,DESK_CONTROL_BUTTON,&a.add,"Add");desk_ui_add(&a.ui,REMOVE,DESK_CONTROL_BUTTON,&a.remove,"Remove");desk_ui_add(&a.ui,SOURCE,DESK_CONTROL_BUTTON,&a.source,"Choose bundle");
    desk_ui_add(&a.ui,REVIEW,DESK_CONTROL_BUTTON,&a.review,"Review");desk_ui_add(&a.ui,BACK,DESK_CONTROL_BUTTON,&a.back,"Back");desk_ui_add(&a.ui,CONFIRM,DESK_CONTROL_BUTTON,&a.confirm,"Import");
    desk_ui_add(&a.ui,LIST,DESK_CONTROL_LIST,&a.list,NULL);desk_ui_add(&a.ui,REPORT,DESK_CONTROL_DOCUMENT,&a.report,NULL);rows();status("Choose a bundle and local name.");
    if(desk_app_open(&a.window,"Profiles",760,560))return 1;desk_log("profile-ui: started pid=%llu\n",(unsigned long long)srv_getpid());trace("editing","ready");if(a.diagnostic[0])desk_log("profile-ui: diagnostic checkpoint=%s\n",a.diagnostic);
    while(!a.quit&&!a.window.failed&&!a.window.closed) {
        long now=srv_monotonic_us();a.now=now<0?(uint64_t)srv_ticks()*10000:(uint64_t)now;poll_child();picker_ready();
        if(!a.picker_starting&&a.file_job.busy) { int done=desk_file_job_poll(&a.file_job);if(done) { int result=desk_chooser_finish(&a.chooser,a.chooser.pending_id,done);if(result==DESK_CHOOSER_READY)desk_log("profile-ui: state=picker operation=%s result=ready\n",operation());a.window.dirty=1; } }
        for(unsigned batch=0;batch<32;batch++) { struct desk_packet p;int result=desk_app_poll(&a.window,&p);if(result<=0)break;if(result==2)continue;
            if(p.type==DESK_CONFIGURE)layout();
            else if(p.type==DESK_FOCUS) { desk_ui_window_focus(&a.ui,p.value&&!a.picker_open);desk_picker_focus(&a.picker,p.value&&a.picker_open);a.window.dirty=1; }
            else if(p.type==DESK_CLOSE_REQUEST)request_close();
            else if(p.type==DESK_KEY) { struct srv_key_event k;memcpy(&k,p.text,sizeof(k));if(a.picker_open)picker_action(desk_picker_key(&a.picker,&k));else if((k.flags&SRV_KEY_DOWN_EVENT)&&(k.modifiers&(SRV_KEY_CTRL|SRV_KEY_META))&&k.usage==26)request_close();else action(desk_ui_key(&a.ui,&k)); }
            else if(p.type==DESK_POINTER) { struct desk_widget_event e={.type=p.value&DESK_POINTER_CANCEL?DESK_WIDGET_CANCEL:DESK_WIDGET_POINTER,.x=p.x,.y=p.y,.down=(p.value&DESK_POINTER_LEFT)!=0,.pressed=(p.value&DESK_POINTER_PRESS)!=0};if(a.picker_open)picker_action(desk_picker_pointer(&a.picker,e,a.now));else action(desk_ui_pointer(&a.ui,e,a.now)); }
            else if(p.type==DESK_SCROLL) { if(a.picker_open)picker_action(desk_picker_scroll(&a.picker,p.width,p.height,p.y));else action(desk_ui_scroll(&a.ui,p.width,p.height,p.x,p.y)); }
        }
        if(a.quit||a.window.failed||a.window.closed)break;struct desk_canvas c;if(desk_app_canvas(&a.window,&c)==1) { draw(&c);desk_app_present(&a.window); }srv_sleep_ticks(1);
    }
    /* A server disconnect must not abandon an accepted transaction or child. */
    a.close_pending=1;while(a.worker_active) { poll_child();if(a.worker_active)srv_sleep_ticks(1); }
    while(a.picker_starting) { picker_ready();if(a.picker_starting)srv_sleep_ticks(1); }
    desk_chooser_cancel(&a.chooser);while(a.file_job.busy) { int done=desk_file_job_poll(&a.file_job);if(done)desk_chooser_finish(&a.chooser,a.chooser.pending_id,done);else srv_sleep_ticks(1); }
    if(a.file_ready)while(!desk_file_job_retire(&a.file_job))srv_sleep_ticks(1);
    desk_app_close(&a.window);desk_log("profile-ui: exited pid=%llu\n",(unsigned long long)srv_getpid());return a.window.failed?1:0;
}
