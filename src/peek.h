#ifndef PEEK_H_INCLUDED
#define PEEK_H_INCLUDED

#include <jansson.h>

#include "data.h"
#include "rule.h"

#define PEEK_MAX_RESOLVERS 10

enum
{
  PEEK_FLAG_DEBUG    = 0x01,
  PEEK_FLAG_LIST     = 0x02,
  PEEK_FLAG_DRY_RUN  = 0x04,
  PEEK_FLAG_OVERRIDE = 0x08
};

typedef struct peek_input    peek_input;
typedef struct peek_resolver peek_resolver;
typedef struct peek          peek;

struct peek_input
{
  int         override;
  peek       *peek;
  stream      stream;
};

struct peek_resolver
{
  int         running;
  peek       *peek;
  int         pipe[2];
  peek_input  input;
  rule       *rule;
  char       *value;
};

struct peek
{
  int         flags;
  char       *state;
  data        data;
  data        queued;
  list        queue;
  list        rules;
  peek_input  stdin;
  pool        pool;
  int         pool_fd;
  size_t      resolvers;
  stream      input;
};

void peek_construct(peek *, int, char **);
void peek_destruct(peek *);

#endif /* PEEK_H_INCLUDED */
