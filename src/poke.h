#ifndef POKE_H
#define POKE_H

#include <stdio.h>
#include <jansson.h>

#include "value.h"

typedef struct
{
  char      **args;
  pid_t       pid;
  int         fd;
  FILE       *file;
} poke_job_t;

typedef struct
{
  const char *key;
  const char *value;
} poke_param_t;

typedef struct
{
  const char *home;
  json_t     *conf;
  json_t     *params;
  json_t     *seen;

  vector_t    jobs;
  size_t      jobs_started;
  size_t      jobs_running;
  size_t      jobs_limit;
} poke_t;

void poke_construct(poke_t *);
void poke_destruct(poke_t *);
void poke_load(poke_t *);
void poke_save(poke_t *);
void poke_add(poke_t *, const char *);
void poke_list(poke_t *);
void poke_process(poke_t *);

#endif /* POKE_H */
