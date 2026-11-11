#ifndef DATA_H
#define DATA_H

#include <limits.h>
#include <stddef.h>

typedef struct {
    char date[11];
    double value;
} DataPoint;

typedef struct {
    char name[256];
    DataPoint *points;
    int count;
} DataSeries;

typedef struct {
    char **names;
    char **files;
    int count;
} DataList;

/* Imported data belongs in <chosen folder>/data. */
int  datalist_scan(DataList *list, const char *folder, char *err, size_t errn);
void datalist_free(DataList *list);
int  data_load(DataSeries *data, const char *folder, const char *file,
               const char *name, char *err, size_t errn);
void data_free(DataSeries *data);

/* Validates a two-column date,value CSV and writes a normalized copy into the
 * chosen folder's data directory. imported_name is the new selectable name. */
int data_import(const char *folder, const char *source, char *imported_name,
                size_t name_n, char *err, size_t errn);

#endif
