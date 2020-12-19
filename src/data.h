#ifndef DATA_H_INCLUDED
#define DATA_H_INCLUDED

#include <stdlib.h>
#include <stdint.h>

#include <dynamic.h>

#define data_foreach(p, v) list_foreach(&(p)->values, v)

typedef struct data data;

struct data
{
  list values;
  maps set;
};

void  data_construct(data *);
void  data_destruct(data *);
int   data_load(data *, char *);
int   data_save(data *, char *);
char *data_add(data *, char *);
void  data_clear(data *, char *);
void  data_delete(data *, char *);
int   data_exists(data *, char *);
int   data_valid(char *);

#endif /* DATA_H_INCLUDED */
