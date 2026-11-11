#ifndef SWING_H
#define SWING_H
#include "settings.h"
#include "stock.h"
int swing_calculate(const Stock *stock,const Settings *settings,double *out);
int swing_calculate2(const Stock *stock,const Settings *settings,double *out);
int swing_calculate3(const Stock *stock,const Settings *settings,double *out);
int swing_calculate3green(const Stock *stock,const Settings *settings,double *out);
int swing_calculate_bb(const Stock *stock,const Settings *settings,double *out);
int swing_export(const Stock *stock,const Settings *settings,const char *folder,char *error,size_t error_size);
#endif
