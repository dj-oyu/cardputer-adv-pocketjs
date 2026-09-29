// The host side of main/pocket/app_chunks.h: see app_chunks_host.h.
#include "app_chunks_host.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_APPS 8

typedef struct {
    char            id[64];
    app_chunk_t     chunks[APP_CHUNKS_MAX];
    app_chunk_set_t set;
} host_set_t;

static host_set_t sets[HOST_APPS];
static unsigned   set_count;

const app_chunk_set_t *app_chunks_for(const char *app_id) {
    if(!app_id) return NULL;
    for(unsigned i=0;i<set_count;i++)
        if(!strcmp(sets[i].id,app_id)) return &sets[i].set;
    return NULL;
}

void app_chunks_host_clear(void) {
    for(unsigned i=0;i<set_count;i++)
        for(uint32_t k=0;k<sets[i].set.count;k++) {
            // name, file and the bytes were three allocations each.
            free((void *)sets[i].chunks[k].name);
            free((void *)sets[i].chunks[k].file);
            free((void *)sets[i].chunks[k].start);
        }
    memset(sets,0,sizeof sets);
    set_count=0;
}

static bool good_name(const char *s) {
    size_t n=strlen(s);
    if(n<1 || n>APP_CHUNK_NAME_MAX) return false;
    for(size_t i=0;i<n;i++)
        if(!isalnum((unsigned char)s[i]) && !strchr("_.-",s[i])) return false;
    return true;
}

// Reads a whole file and leaves a NUL after it, as EMBED_TXTFILES does.
static char *slurp(const char *path, size_t *length) {
    FILE *f=fopen(path,"rb");
    if(!f) return NULL;
    fseek(f,0,SEEK_END);
    long n=ftell(f);
    fseek(f,0,SEEK_SET);
    char *buffer=n>=0?malloc((size_t)n+1):NULL;
    if(buffer && fread(buffer,1,(size_t)n,f)!=(size_t)n) { free(buffer); buffer=NULL; }
    fclose(f);
    if(!buffer) return NULL;
    buffer[n]=0;
    *length=(size_t)n;
    return buffer;
}

static char *copy(const char *s) {
    size_t n=strlen(s)+1;
    char *p=malloc(n);
    if(p) memcpy(p,s,n);
    return p;
}

int app_chunks_host_read(const char *list_path) {
    FILE *f=fopen(list_path,"r");
    if(!f) { fprintf(stderr,"app_chunks_host: cannot open %s\n",list_path); return -1; }
    // Files are relative to the list, as tools/make_app_chunks.py reads them.
    char dir[512];
    snprintf(dir,sizeof dir,"%s",list_path);
    char *slash=strrchr(dir,'/');
    if(slash) slash[1]=0; else dir[0]=0;
    host_set_t *current=NULL;
    char line[512];
    int number=0, result=0;
    while(result==0 && fgets(line,sizeof line,f)) {
        number++;
        char *hash=strchr(line,'#');
        if(hash) *hash=0;
        char a[128], b[384], extra[2];
        int words=sscanf(line,"%127s %383s %1s",a,b,extra);
        if(words<=0) continue;
        if(!strcmp(a,"app")) {
            if(words!=2 || current) { result=-1; break; }
            for(unsigned i=0;i<set_count;i++) if(!strcmp(sets[i].id,b)) current=&sets[i];
            if(!current) {
                if(set_count==HOST_APPS || strlen(b)>=sizeof current->id) { result=-1; break; }
                current=&sets[set_count++];
                memcpy(current->id,b,strlen(b)+1);   // length checked above
                current->set=(app_chunk_set_t){.app_id=current->id,.chunks=current->chunks};
            }
            continue;
        }
        if(words!=2 || !current || !good_name(a)) { result=-1; break; }
        for(uint32_t k=0;k<current->set.count;k++)
            if(!strcmp(current->chunks[k].name,a)) result=-1;
        if(result || current->set.count==APP_CHUNKS_MAX) { result=-1; break; }
        char path[1024];
        snprintf(path,sizeof path,"%s%s",dir,b);
        size_t length=0;
        char *bytes=slurp(path,&length);
        if(!bytes) { fprintf(stderr,"app_chunks_host: %s:%d: no file %s\n",list_path,number,path); result=-1; break; }
        const char *base=strrchr(b,'/');
        app_chunk_t *c=&current->chunks[current->set.count++];
        *c=(app_chunk_t){.name=copy(a),.file=copy(base?base+1:b),.start=bytes,.end=bytes+length+1};
    }
    fclose(f);
    if(result==0 && !current) result=-1;
    if(result) fprintf(stderr,"app_chunks_host: %s:%d: refused (see tools/make_app_chunks.py)\n",
                       list_path,number);
    return result;
}
