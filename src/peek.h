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
  PEEK_FLAG_OVERRIDE = 0x04
};

typedef struct peek_resolver peek_resolver;
typedef struct peek          peek;

struct peek_resolver
{
  int         running;
  peek       *peek;
  rule       *rule;
  char       *value;
};

struct peek
{
  int         flags;
  char       *state;
  data        resolved;
  data        queued;
  list        queue;
  list        rules;
  pool        pool;
  int         pool_fd;
  size_t      resolvers;
  stream      input;
};

void peek_construct(peek *, int, char **);
void peek_destruct(peek *);

#endif /* PEEK_H_INCLUDED */
