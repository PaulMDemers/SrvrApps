#include <srvros/desk_file_search.h>
#include <srvros/desk_text.h>
#include <srvros/sys.h>
#include <string.h>
static struct { struct desk_search_header header;struct desk_search_entry entries[DESK_SEARCH_LIMIT]; } reply;
static unsigned char content[DESK_SEARCH_CONTENT_FILE_BYTES];

static int same_file(const struct srv_stat *a,const struct srv_stat *b) {
    return a->type==b->type && a->inode==b->inode && a->size==b->size && a->mtime==b->mtime;
}
/* Reject binary controls and validate the complete buffer as UTF-8. */
static int text_valid(const unsigned char *p,size_t n) {
    for(size_t i=0;i<n;i++) {
        unsigned c=p[i];
        if((c<0x20 && c!='\t' && c!='\r' && c!='\n') || c==0x7f) return 0;
    }
    return desk_text_utf8_valid((const char *)p,n);
}
/* Returns 1 for a match, 0 for a valid nonmatch, -1 for a skipped file. */
static int search_content(const char *path,const char *query,const struct srv_stat *before,uint64_t *total) {
    struct srv_stat opened,after,path_after;
    if(before->size>sizeof(content)) return -1;
    long fd=srv_open(path);if(fd<0) return -1;
    int result=-1;size_t used=0;
    if(srv_fstat((int)fd,&opened) || !same_file(before,&opened)) goto done;
    while(used<before->size) {
        size_t remaining=(size_t)(before->size-used);
        uint64_t budget=DESK_SEARCH_CONTENT_TOTAL_BYTES-*total;
        if(!budget) break;
        if(remaining>budget) remaining=(size_t)budget;
        long n=srv_read((int)fd,content+used,remaining);
        if(n==SRV_ERR_INTR) continue;
        if(n<=0 || (size_t)n>remaining) goto done;
        used+=(size_t)n;*total+=(size_t)n;
    }
    if(used!=before->size || srv_fstat((int)fd,&after) || !same_file(before,&after) ||
       srv_stat(path,&path_after) || !same_file(before,&path_after) || !text_valid(content,used)) goto done;
    result=used && desk_file_search_text_matches(content,used,query) ? 1 : 0;
done:
    srv_close((int)fd);return result;
}
int main(int argc,char **argv) {
    if((argc!=4 && argc!=5) || !desk_file_search_path_valid(argv[1]) ||
       strlen(argv[3])!=1 || argv[3][0]<'0' || argv[3][0]>'2') return 1;
    unsigned kind=argv[3][0]-'0',mode=DESK_SEARCH_NAME;
    if(argc==5) { if(strlen(argv[4])!=1 || argv[4][0]<'0' || argv[4][0]>'1') return 1;mode=argv[4][0]-'0'; }
    if(!desk_file_search_query_valid_mode(argv[2],mode) || (mode==DESK_SEARCH_CONTENTS && kind!=1)) return 1;
    struct srv_stat root;if(srv_stat(argv[1],&root) || root.type!=1) return 1;
    memset(&reply,0,sizeof(reply));
    reply.header=(struct desk_search_header){.magic=DESK_SEARCH_MAGIC,.version=mode==DESK_SEARCH_CONTENTS?DESK_SEARCH_CONTENT_VERSION:DESK_SEARCH_VERSION};
    uint64_t total=0;
    for(uint64_t index=0;;index++) {
        char path[1024];uint64_t size;long result=srv_list(index,path,sizeof(path),&size);
        if(result<0) return 1;
        if(!result) break;
        reply.header.scanned++;
        /* SYS_LIST truncates to capacity: never turn an invalid name into a prefix. */
        if(!memchr(path,0,sizeof(path)) || !desk_file_search_path_valid(path)) { reply.header.skipped++;goto next; }
        if(!desk_file_search_under(path,argv[1]) || (mode==DESK_SEARCH_NAME && !desk_file_search_matches(path,argv[2]))) goto next;
        struct srv_stat info;if(srv_stat(path,&info)) { if(mode==DESK_SEARCH_CONTENTS) reply.header.skipped++;goto next; }
        unsigned directory=info.type==1;if(info.type!=1 && info.type!=0) goto next;
        if((kind==1 && directory) || (kind==2 && !directory)) goto next;
        if(mode==DESK_SEARCH_CONTENTS) {
            int match=search_content(path,argv[2],&info,&total);
            if(match<0) { reply.header.skipped++;goto next; }
            if(!match) goto next;
        }
        reply.header.matched++;
        if(reply.header.count==DESK_SEARCH_LIMIT) { reply.header.flags|=DESK_SEARCH_TRUNCATED;goto next; }
        struct desk_search_entry *e=&reply.entries[reply.header.count++];memset(e,0,sizeof(*e));strcpy(e->path,path);
        e->inode=info.inode;e->size=info.size;e->modified=info.mtime;e->directory=directory;
next:
        if(mode==DESK_SEARCH_CONTENTS) {
            if(total==DESK_SEARCH_CONTENT_TOTAL_BYTES) reply.header.flags|=DESK_SEARCH_BYTE_LIMIT;
            if(reply.header.scanned==DESK_SEARCH_CONTENT_SCAN_LIMIT) reply.header.flags|=DESK_SEARCH_SCAN_LIMIT;
            if(reply.header.flags&(DESK_SEARCH_BYTE_LIMIT|DESK_SEARCH_SCAN_LIMIT)) break;
        }
    }
    const char *data=(const char *)&reply;size_t length=sizeof(reply.header)+reply.header.count*sizeof(*reply.entries);
    for(size_t sent=0;sent<length;) {
        long n=srv_write(1,data+sent,length-sent);if(n==SRV_ERR_INTR) continue;
        if(n<=0 || (size_t)n>length-sent) return 1;
        sent+=(size_t)n;
    }
    return 0;
}
