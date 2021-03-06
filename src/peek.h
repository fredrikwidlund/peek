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
  PEEK_FLAG_OVERRIDE = 0x08,
  PEEK_FLAG_CLEAR    = 0x10
};

typedef struct peek_resolver peek_resolver;
typedef struct peek          peek;

struct peek_resolver
{
  pid_t       pid;
  peek       *peek;
  stream      input;
  rule       *rule;
  char       *value;
};

struct peek
{
  int         active;
  int         flags;
  char       *state;
  data        data;
  list        resolvers;
  list        rules;
  stream      input;
};

void peek_construct(peek *, int, char **);
void peek_destruct(peek *);

#endif /* PEEK_H_INCLUDED */
