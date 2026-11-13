#ifndef TEST_SUPPORT_H
#define TEST_SUPPORT_H

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned test_checks, test_cases;
static char test_root[256];

#define CHECK(condition) do { \
    test_checks++; \
    if (!(condition)) { \
        fprintf(stderr,"%s:%d: check failed: %s\n",__FILE__,__LINE__,#condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)
#define NEAR(actual, expected, tolerance) do { \
    double test_a=(actual), test_b=(expected); \
    if (!isfinite(test_a) || !isfinite(test_b) || \
        fabs(test_a-test_b) > (tolerance)*fmax(1,fabs(test_b))) { \
        fprintf(stderr,"%s:%d: got %.17g; expected %.17g\n", \
                __FILE__,__LINE__,test_a,test_b); \
        exit(EXIT_FAILURE); \
    } \
    test_checks++; \
} while (0)
#define RUN(function) do { function(); test_cases++; } while (0)

static inline void test_remove_tree(const char *path)
{
    DIR *dir=opendir(path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry=readdir(dir))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        char child[8192];
        int n=snprintf(child,sizeof child,"%s/%s",path,entry->d_name);
        if (n<0 || (size_t)n>=sizeof child) abort();
        struct stat st;
        if (!lstat(child,&st) && S_ISDIR(st.st_mode)) test_remove_tree(child);
        else unlink(child);
    }
    closedir(dir);
    rmdir(path);
}

static inline void test_cleanup(void) { test_remove_tree(test_root); }

/* Every process gets a private HOME before calling any application code. */
static inline void test_setup(void)
{
    strcpy(test_root,"/tmp/candles-tests-XXXXXX");
    CHECK(mkdtemp(test_root)!=NULL);
    CHECK(atexit(test_cleanup)==0);
    CHECK(setenv("HOME",test_root,1)==0);
}

static inline void test_path(char *out,size_t size,const char *relative)
{
    int n=snprintf(out,size,"%s/%s",test_root,relative);
    CHECK(n>=0 && (size_t)n<size);
}

static inline void test_write(const char *relative,const char *contents)
{
    char path[8192]; test_path(path,sizeof path,relative);
    FILE *file=fopen(path,"w"); CHECK(file!=NULL);
    CHECK(fputs(contents,file)>=0);
    CHECK(fclose(file)==0);
}

static inline char *test_read(const char *relative)
{
    char path[8192]; test_path(path,sizeof path,relative);
    FILE *file=fopen(path,"rb"); CHECK(file!=NULL);
    CHECK(!fseek(file,0,SEEK_END)); long size=ftell(file); CHECK(size>=0);
    rewind(file);
    char *text=malloc((size_t)size+1); CHECK(text!=NULL);
    CHECK(fread(text,1,(size_t)size,file)==(size_t)size);
    text[size]=0; CHECK(!fclose(file)); return text;
}

static inline int test_done(const char *name)
{
    printf("%s: %u cases, %u checks passed\n",name,test_cases,test_checks);
    return 0;
}
#endif
