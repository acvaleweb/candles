#include "data.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int date_ok(const char *s)
{
    return strlen(s) == 10 && s[4] == '-' && s[7] == '-' &&
           isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) &&
           isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]) &&
           isdigit((unsigned char)s[5]) && isdigit((unsigned char)s[6]) &&
           isdigit((unsigned char)s[8]) && isdigit((unsigned char)s[9]);
}

static int cmp_point(const void *a, const void *b)
{
    const DataPoint *aa = a, *bb = b;
    return strcmp(aa->date, bb->date);
}

void datalist_free(DataList *list)
{
    for (int i = 0; i < list->count; i++) { free(list->names[i]); free(list->files[i]); }
    free(list->names); free(list->files); memset(list, 0, sizeof *list);
}

int datalist_scan(DataList *list, const char *folder, char *err, size_t errn)
{
    datalist_free(list);
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/data", folder) >= (int)sizeof path) {
        snprintf(err, errn, "Data path too long"); return -1;
    }
    DIR *dir = opendir(path);
    if (!dir) { if (errno == ENOENT) return 0; snprintf(err,errn,"Cannot read data: %s",strerror(errno)); return -1; }
    int cap = 0; struct dirent *entry;
    while ((entry = readdir(dir))) {
        size_t len = strlen(entry->d_name);
        if (len < 5 || strcmp(entry->d_name + len - 4, ".csv")) continue;
        if (list->count == cap) {
            int next = cap ? cap * 2 : 8;
            char **names = realloc(list->names, (size_t)next * sizeof *names);
            if (!names) { closedir(dir); datalist_free(list); snprintf(err,errn,"Out of memory"); return -1; }
            list->names = names;
            char **files = realloc(list->files, (size_t)next * sizeof *files);
            if (!files) { closedir(dir); datalist_free(list); snprintf(err,errn,"Out of memory"); return -1; }
            list->files = files; cap = next;
        }
        list->files[list->count] = strdup(entry->d_name);
        list->names[list->count] = strndup(entry->d_name, len - 4);
        if (!list->files[list->count] || !list->names[list->count]) { closedir(dir); datalist_free(list); snprintf(err,errn,"Out of memory"); return -1; }
        list->count++;
    }
    closedir(dir);
    /* Keep names and files paired while sorting, as the file is just name.csv. */
    for (int i=0;i<list->count;i++) for (int j=i+1;j<list->count;j++)
        if (strcasecmp(list->names[i],list->names[j]) > 0) {
            char *t=list->names[i];list->names[i]=list->names[j];list->names[j]=t;
            t=list->files[i];list->files[i]=list->files[j];list->files[j]=t;
        }
    return 0;
}

void data_free(DataSeries *data)
{
    free(data->points); memset(data, 0, sizeof *data);
}

int data_load(DataSeries *data, const char *folder, const char *file,
              const char *name, char *err, size_t errn)
{
    data_free(data);
    char path[PATH_MAX];
    if (snprintf(path,sizeof path,"%s/data/%s",folder,file) >= (int)sizeof path) { snprintf(err,errn,"Data path too long"); return -1; }
    FILE *f=fopen(path,"r"); if(!f){snprintf(err,errn,"Cannot open data: %s",strerror(errno));return -1;}
    int cap=0; char line[512];
    while(fgets(line,sizeof line,f)) {
        char date[32], extra; double value;
        if (sscanf(line," %31[^,],%lf %c",date,&value,&extra) != 2 || !date_ok(date) || !isfinite(value)) continue;
        if(data->count==cap){int next=cap?cap*2:128;DataPoint *p=realloc(data->points,(size_t)next*sizeof*p);if(!p){fclose(f);data_free(data);snprintf(err,errn,"Out of memory");return -1;}data->points=p;cap=next;}
        memcpy(data->points[data->count].date, date, 11);
        data->points[data->count++].value=value;
    }
    fclose(f);
    if(!data->count){data_free(data);snprintf(err,errn,"Data has no valid date,value rows");return -1;}
    qsort(data->points,(size_t)data->count,sizeof *data->points,cmp_point);
    snprintf(data->name,sizeof data->name,"%s",name); return 0;
}

static void base_name(const char *source, char out[128])
{
    const char *p=strrchr(source,'/'); p=p?p+1:source; size_t n=strlen(p);
    if(n>4&&!strcasecmp(p+n-4,".csv"))n-=4;
    if(!n)n=1;
    if(n>100)n=100;
    for(size_t i=0;i<n;i++) out[i]=(isalnum((unsigned char)p[i])||p[i]=='_'||p[i]=='-')?p[i]:'_';
    out[n]='\0';
}

int data_import(const char *folder, const char *source, char *imported_name,
                size_t name_n, char *err, size_t errn)
{
    DataSeries data={0}; char base[128]; base_name(source,base);
    /* Load directly first; it validates the selected source without requiring
       it to already be under the application's data directory. */
    FILE *f=fopen(source,"r"); if(!f){snprintf(err,errn,"Cannot open data: %s",strerror(errno));return -1;}
    int cap=0; char line[512];
    while(fgets(line,sizeof line,f)) { char date[32],extra; double value; if(sscanf(line," %31[^,],%lf %c",date,&value,&extra)!=2||!date_ok(date)||!isfinite(value))continue; if(data.count==cap){int next=cap?cap*2:128;DataPoint*p=realloc(data.points,(size_t)next*sizeof*p);if(!p){fclose(f);data_free(&data);snprintf(err,errn,"Out of memory");return -1;}data.points=p;cap=next;}memcpy(data.points[data.count].date,date,11);data.points[data.count++].value=value; }
    fclose(f);
    if(!data.count){data_free(&data);snprintf(err,errn,"Data has no valid date,value rows");return -1;}
    qsort(data.points,(size_t)data.count,sizeof *data.points,cmp_point);
    char dir[PATH_MAX]; if(snprintf(dir,sizeof dir,"%s/data",folder)>=(int)sizeof dir){data_free(&data);snprintf(err,errn,"Data path too long");return -1;} if(mkdir(dir,0755)&&errno!=EEXIST){data_free(&data);snprintf(err,errn,"Cannot create data folder: %s",strerror(errno));return -1;}
    char file[PATH_MAX], name[160]; int suffix=1;
    do { if(suffix==1)snprintf(name,sizeof name,"%s",base);else snprintf(name,sizeof name,"%s_%02d",base,suffix); if(snprintf(file,sizeof file,"%s/%s.csv",dir,name)>=(int)sizeof file){data_free(&data);snprintf(err,errn,"Data path too long");return -1;} suffix++; } while(access(file,F_OK)==0&&suffix<1000);
    if(access(file,F_OK)==0){data_free(&data);snprintf(err,errn,"Too many data files with this name");return -1;}
    FILE *out=fopen(file,"w");if(!out){data_free(&data);snprintf(err,errn,"Cannot save data: %s",strerror(errno));return -1;}
    fprintf(out,"date,value\n");for(int i=0;i<data.count;i++)fprintf(out,"%s,%.17g\n",data.points[i].date,data.points[i].value);fclose(out);
    snprintf(imported_name,name_n,"%s",name);data_free(&data);return 0;
}
