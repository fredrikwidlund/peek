#ifndef RULE_H_INCLUDED
#define RULE_H_INCLUDED

#include <regex.h>

typedef struct rule rule;

struct rule
{
  int      valid;
  char    *name;
  char    *path;
  regex_t  regex;
};

void rule_construct(rule *, char *);
void rule_destruct(rule *);
int  rule_valid(rule *);
int  rule_match(rule *, char *);

#endif /* RULE_H_INCLUDED */
