#define _POSIX_C_SOURCE 200809L
#include "policy.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
static const char *const names[]={"persist_boot","persist_port","auto_start_on_boot"};

int policy_bool(const char *text,int *value) {
    if(!strcmp(text,"true")) {*value=1;return 0;}
    if(!strcmp(text,"false")) {*value=0;return 0;}
    return 1;
}
static char *trim(char *s) {
    while(isspace((unsigned char)*s)) ++s;
    char *end=s+strlen(s);
    while(end>s && isspace((unsigned char)end[-1])) *--end='\0';
    return s;
}
static int config_path(char *dest,size_t size) {
    const char *base=getenv("XDG_CONFIG_HOME");
    char fallback[PATH_MAX];
    if(!base || base[0]!='/') {
        const char *home=getenv("HOME");
        if(!home || home[0]!='/') {fputs("HOME must be an absolute directory\n",stderr);return 1;}
        int n=snprintf(fallback,sizeof fallback,"%s/.config",home);
        if(n<0 || (size_t)n>=sizeof fallback) return 1;
        base=fallback;
    }
    int n=snprintf(dest,size,"%s/serve/config",base);
    return n<0 || (size_t)n>=size;
}
static int make_directories(const char *path) {
    char copy[PATH_MAX];
    if(strlen(path)>=sizeof copy) return 1;
    strcpy(copy,path);
    for(char *p=copy+1;;++p) {
        if(*p!='/' && *p!='\0') continue;
        char saved=*p; *p='\0';
        if(mkdir(copy,0700) && errno!=EEXIST) {perror(copy);return 1;}
        struct stat st;
        if(stat(copy,&st) || !S_ISDIR(st.st_mode)) {fprintf(stderr,"Invalid directory: %s\n",copy);return 1;}
        *p=saved;
        if(!saved) return 0;
    }
}
static void defaults(ServePolicy *p) {
    *p=(ServePolicy){.persist_boot=0,.persist_port=1,.auto_start_on_boot=0};
}
int policy_global(ServePolicy *p) {
    defaults(p);
    char path[PATH_MAX];
    if(config_path(path,sizeof path)) return 1;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd==-1 && errno==ENOENT) return 0;
    if(fd==-1) {perror(path);return 1;}
    FILE *f=fdopen(fd,"r");
    if(!f) {close(fd);perror("fdopen");return 1;}
    char *line=NULL; size_t cap=0;
    unsigned seen=0,lineno=0; int error=0;
    while(getline(&line,&cap,f)!=-1) {
        ++lineno;
        char *s=trim(line);
        if(!*s || *s=='#') continue;
        char *eq=strchr(s,'=');
        if(!eq) {error=1;break;}
        *eq='\0';
        char *key=trim(s),*value=trim(eq+1);
        int i;
        for(i=0;i<3 && strcmp(names[i],key);++i) {}
        int v;
        if(i==3 || (seen&(1u<<i)) || policy_bool(value,&v)) {error=1;break;}
        seen|=1u<<i;
        if(i==0) p->persist_boot=v;
        else if(i==1) p->persist_port=v;
        else p->auto_start_on_boot=v;
    }
    if(ferror(f)) error=1;
    free(line);
    if(fclose(f)) error=1;
    if(error) fprintf(stderr,"Invalid global config %s at line %u\n",path,lineno);
    return error;
}
int policy_effective(const ServeConfig *cfg,ServePolicy *policy) {
    if(policy_global(policy)) return 1;
    if(cfg->persist_boot>=0) policy->persist_boot=cfg->persist_boot;
    if(cfg->persist_port>=0) policy->persist_port=cfg->persist_port;
    if(cfg->auto_start_on_boot>=0) policy->auto_start_on_boot=cfg->auto_start_on_boot;
    return 0;
}
static int write_global(const ServePolicy *p) {
    char path[PATH_MAX],folder[PATH_MAX],temp[PATH_MAX];
    if(config_path(path,sizeof path)) return 1;
    strcpy(folder,path);
    char *slash=strrchr(folder,'/');
    if(!slash) return 1;
    *slash='\0';
    if(make_directories(folder)) return 1;
    int n=snprintf(temp,sizeof temp,"%s/.config-XXXXXX",folder);
    if(n<0 || (size_t)n>=sizeof temp) return 1;
    int fd=mkstemp(temp);
    if(fd<0) {perror("mkstemp");return 1;}
    FILE *f=fdopen(fd,"w");
    if(!f) {close(fd);unlink(temp);return 1;}
    int ok=fprintf(f,"persist_boot = %s\npersist_port = %s\nauto_start_on_boot = %s\n",
        p->persist_boot?"true":"false",p->persist_port?"true":"false",
        p->auto_start_on_boot?"true":"false")>0;
    if(fflush(f) || fsync(fd)) ok=0;
    if(fclose(f)) ok=0;
    if(ok && rename(temp,path)) {perror("rename");ok=0;}
    if(!ok) unlink(temp);
    return !ok;
}
int policy_config(int argc,const char *const *argv) {
    ServePolicy p;
    if(policy_global(&p)) return 1;
    if(argc==0) {
        printf("persist_boot       = %s\npersist_port       = %s\nauto_start_on_boot = %s\n",
            p.persist_boot?"true":"false",p.persist_port?"true":"false",
            p.auto_start_on_boot?"true":"false");
        return 0;
    }
    if(argc!=2) {fputs("Usage: serve --config [key true|false]\n",stderr);return 1;}
    int key;
    for(key=0;key<3 && strcmp(names[key],argv[0]);++key) {}
    int value;
    if(key==3 || policy_bool(argv[1],&value)) {
        fputs("Invalid global setting or value (expected true/false)\n",stderr);
        return 1;
    }
    if(key==0) p.persist_boot=value;
    else if(key==1) p.persist_port=value;
    else p.auto_start_on_boot=value;
    if(write_global(&p)) return 1;
    printf("Global %s = %s\n",names[key],value?"true":"false");
    return 0;
}
