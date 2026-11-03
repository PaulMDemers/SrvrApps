#include <srvros/desk_file_search.h>
#include <srvros/sys.h>
#include <string.h>
static struct { struct desk_search_header header;struct desk_search_entry entries[DESK_SEARCH_LIMIT]; } reply;
int main(int argc,char **argv) {
    if(argc!=4 || !desk_file_search_path_valid(argv[1]) || !desk_file_search_query_valid(argv[2]) || strlen(argv[3])!=1 || argv[3][0]<'0' || argv[3][0]>'2') return 1;
    unsigned kind=argv[3][0]-'0';struct srv_stat root;
    if(srv_stat(argv[1],&root) || root.type!=1) return 1;
    reply.header=(struct desk_search_header){.magic=DESK_SEARCH_MAGIC,.version=DESK_SEARCH_VERSION};
    for(uint64_t index=0;;index++) {
        char path[1024];uint64_t size;long result=srv_list(index,path,sizeof(path),&size);
        if(result<0) return 1;
        if(!result) break;
        reply.header.scanned++;
        /* Read with ample space: SYS_LIST truncates to the supplied capacity.
         * Overlong/noncanonical names must never become actionable prefixes. */
        if(!memchr(path,0,sizeof(path)) || !desk_file_search_path_valid(path)) { reply.header.skipped++;continue; }
        if(!desk_file_search_under(path,argv[1]) || !desk_file_search_matches(path,argv[2])) continue;
        struct srv_stat info;if(srv_stat(path,&info)) continue; /* May disappear during a read-only scan. */
        unsigned directory=info.type==1;if(info.type!=1 && info.type!=0) continue;
        if((kind==1 && directory) || (kind==2 && !directory)) continue;
        reply.header.matched++;
        if(reply.header.count==DESK_SEARCH_LIMIT) { reply.header.flags|=DESK_SEARCH_TRUNCATED;continue; }
        struct desk_search_entry *e=&reply.entries[reply.header.count++];memset(e,0,sizeof(*e));strcpy(e->path,path);
        e->inode=info.inode;e->size=info.size;e->modified=info.mtime;e->directory=directory;
    }
    const char *data=(const char *)&reply;size_t length=sizeof(reply.header)+reply.header.count*sizeof(*reply.entries);
    for(size_t sent=0;sent<length;) {
        long n=srv_write(1,data+sent,length-sent);if(n==SRV_ERR_INTR) continue;
        if(n<=0 || (size_t)n>length-sent) return 1;
        sent+=(size_t)n;
    }
    return 0;
}
