#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include <dynamic.h>

#include "rule.h"

static void rule_parse(rule *rule)
{
  char *base, *delim;
  size_t i;
  string s;
  int e;

  delim = strrchr(rule->path, '/');
  base = delim ? delim + 1 : rule->path;

  for (i = 0; i < strlen(base); i ++)
    if (!(isalnum(base[i]) || strchr("-_:", base[i])))
      return;

  delim = strchr(base, ':');
  if (!delim)
    return;
  rule->name = strndup(base, delim - base);
  base = delim + 1;

  string_construct(&s);
  string_append(&s, "^");
  string_append(&s, delim + 1);
  string_replace_all(&s, "::", ":[^:]*:");
  if (string_data(&s)[string_length(&s) - 1] == ':')
    string_append(&s, "[^:]*");
  string_append(&s, "$");
  e = regcomp(&rule->regex, string_data(&s), REG_NOSUB);
  if (e == 0)
    rule->valid = 1;
  string_destruct(&s);
}

void rule_construct(rule *rule, char *path)
{
  *rule = (struct rule) {.path = strdup(path)};

  rule_parse(rule);
  if (!rule_valid(rule))
    rule_destruct(rule);
}

void rule_destruct(rule *rule)
{
  free(rule->name);
  free(rule->path);
  if (rule->valid)
    regfree(&rule->regex);
  *rule = (struct rule) {0};
}

int rule_valid(rule *rule)
{
  return rule->valid;
}

int rule_match(rule *rule, char *value)
{
  return regexec(&rule->regex, value, 0, NULL, 0) == 0;
}

void rule_exec(rule *rule, char *value)
{
  char *delim, *arg0;

  delim = strrchr(rule->path, '/');
  arg0 = delim ? delim : rule->path;
  (void) execl(rule->path, arg0, value, NULL);
}
