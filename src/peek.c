#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <getopt.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <err.h>
#include <errno.h>
#include <sys/stat.h>

#include <dynamic.h>

#include "rule.h"
#include "peek.h"

static void peek_update(peek *);

static void peek_usage(void)
{
  extern char *__progname;

  (void) fprintf(stderr, "Usage: %s [OPTION]... [DATA POINT]...\n", __progname);
  (void) fprintf(stderr, "peek discovery framework\n");
  (void) fprintf(stderr, "\n");
  (void) fprintf(stderr, "Options:\n");
  (void) fprintf(stderr, "    -a              list state (default if no other arguments)\n");
  (void) fprintf(stderr, "    -r PATH         set rules PATH (defaults to /usr/share/peek/)\n");
  (void) fprintf(stderr, "    -s PATH         use PATH to persist state (defaults to $HOME/.peek)\n");
  (void) fprintf(stderr, "    -d              debug\n");
  (void) fprintf(stderr, "    -h              display this help\n");
}

void peek_debug(peek *peek, const char *format, ...)
{
  va_list args;

  if (peek->flags & PEEK_FLAG_DEBUG)
  {
    va_start(args, format);
    (void) fprintf(stderr, "[debug] ");
    (void) vfprintf(stderr, format, args);
    (void) fprintf(stderr, "\n");
    va_end(args);
  }
}

static void peek_rules_release(void *arg)
{
  rule_destruct(arg);
}

static void peek_rules(peek *peek, char *dir)
{
  int i, n;
  struct dirent **namelist;
  char path[PATH_MAX];
  rule *rule;

  if (!dir)
    dir = "/usr/share/peek/rules";
  n = scandir(dir, &namelist, NULL, NULL);
  if (n > 0)
  {
    for (i = 0; i < n; i ++)
    {
      if (namelist[i]->d_type == DT_REG)
      {
        (void) snprintf(path, sizeof path, "%s/%s", dir, namelist[i]->d_name);
        rule = list_push_back(&peek->rules, NULL, sizeof *rule);
        rule_construct(rule, path);
        if (!rule_valid(rule))
        {
          list_erase(rule, NULL);
          warnx("discarding rule %s", path);
        }
      }
      free(namelist[i]);
    }
    free(namelist);
  }

  if (list_empty(&peek->rules))
    warnx("no rules loaded");
}

static void peek_state(peek *peek, char *state)
{
  char *home;
  int e;

  // set state directory
  if (state)
    peek->state = strdup(state);
  else
  {
    home = getenv("HOME");
    e = asprintf(&peek->state, "%s/.peek", home ? home : "/");
    if (e == -1)
      err(1, "asprintf");
  }

  // create state directory
  e = mkdir(peek->state, 0700);
  if (e == -1 && errno != EEXIST)
    warn("unable to create directory %s", peek->state);
}

static void peek_add(peek *peek, char *value)
{
  rule *rule;
  peek_resolver *resolver;
  int matches = 0;

  if (data_exists(&peek->resolved, value) || data_exists(&peek->queued, value))
    return;

  list_foreach(&peek->rules, rule)
    if (rule_match(rule, value))
      {
        resolver = list_push_back(&peek->queue, NULL, sizeof *resolver);
        resolver->peek = peek;
        resolver->rule = rule;
        resolver->value = strdup(value);
        matches++;
        peek_debug(peek, "rule %s applied to %s", rule->name, value);
      }

  data_add(matches ? &peek->queued : &peek->resolved, value);
  (void) fprintf(stdout, "%s\n", value);
}

static void peek_load(peek *peek)
{
  char path[PATH_MAX], *value;
  int e;
  data saved;

  (void) snprintf(path, sizeof path, "%s/resolved", peek->state);
  e = data_load(&peek->resolved, path);
  if (e == -1)
    warn("unable to load resolved data points from %s", path);

  (void) snprintf(path, sizeof path, "%s/queued", peek->state);
  data_construct(&saved);
  e = data_load(&saved, path);
  if (e == -1)
    warn("unable to load queued data points from %s", path);
  data_foreach(&saved, value)
    peek_add(peek, value);
  data_destruct(&saved);
}

static void peek_save(peek *peek)
{
  char path[PATH_MAX];
  int e;

  (void) snprintf(path, sizeof path, "%s/resolved", peek->state);
  e = data_save(&peek->resolved, path);
  if (e == -1)
    warn("unable to saved resolved data points to %s", path);

  (void) snprintf(path, sizeof path, "%s/queued", peek->state);
  e = data_save(&peek->queued, path);
  if (e == -1)
    warn("unable to save queued data points to %s", path);
}

static void peek_list(peek *peek)
{
  char *value;

  data_foreach(&peek->resolved, value)
    (void) fprintf(stdout, "%s\n", value);
}

static void peek_task(void *arg)
{
  peek_resolver *resolver = arg;
  char *command;
  int e;
  FILE *f;
  char *line = NULL;
  size_t size = 0;
  ssize_t n;

  e = asprintf(&command, "%s %s", resolver->rule->path, resolver->value);
  if (e == -1)
    err(1, "asprintf");
  peek_debug(resolver->peek, "run %s", command);
  f = popen(command, "r");
  if (!f)
    err(1, "popen %s", command);
  while (1)
    {
      n = getline(&line, &size, f);
      if (n == -1)
        break;
      if (line[n - 1] == '\n')
        line[n - 1] = 0;
      peek_debug(resolver->peek, "read %s", line);
      peek_add(resolver->peek, line);
    }
  (void) pclose(f);
  free(line);
  free(command);
}


static core_status peek_poll(core_event *event)
{
  peek *peek = event->state;
  peek_resolver *completed, *resolver;
  int matches;

  while (1)
  {
    completed = pool_collect(&peek->pool, POOL_DONTWAIT);
    if (!completed)
      break;
    matches = 0;
    list_foreach(&peek->queue, resolver)
      if (strcmp(resolver->value, completed->value) == 0)
        matches++;
    if (matches == 1)
    {
      data_add(&peek->resolved, completed->value);
      data_delete(&peek->queued, completed->value);
    }
    free(completed->value);
    list_erase(completed, NULL);
    peek->resolvers --;
  }

  peek_update(peek);
  return CORE_OK;
}

static void peek_update(peek *peek)
{
  peek_resolver *resolver;

  list_foreach(&peek->queue, resolver)
  {
    if (resolver->running)
      continue;
    if (peek->resolvers >= PEEK_MAX_RESOLVERS)
      break;
    pool_enqueue(&peek->pool, peek_task, resolver);
    resolver->running = 1;
    peek->resolvers++;
  }

  if (peek->resolvers && peek->pool_fd == -1)
  {
    peek->pool_fd = pool_fd(&peek->pool);
    core_add(NULL, peek_poll, peek, peek->pool_fd, EPOLLIN | EPOLLET);
  }
  else if (peek->resolvers == 0 && peek->pool_fd >= 0)
  {
    core_delete(NULL, peek->pool_fd);
    peek->pool_fd = -1;
  }
}

static void peek_configure(peek *peek, int argc, char **argv)
{
  int c, i;
  char *rules = NULL, *state = NULL;

  while (1)
    {
      c = getopt(argc, argv, "adhmr:s:");
      if (c == -1)
        break;
      switch (c)
        {
        case 'a':
          peek->flags |= PEEK_FLAG_LIST;
          break;
        case 'd':
          peek->flags |= PEEK_FLAG_DEBUG;
          break;
        case 's':
          state = optarg;
          break;
        case 'r':
          rules = optarg;
          break;
        default:
          peek_usage();
          return;
        }
    }

  argc -= optind;
  argv += optind;

  // setup state directory
  peek_state(peek, state);

  // load state
  peek_load(peek);

  // list current resolved values if requested
  if (peek->flags & PEEK_FLAG_LIST)
    peek_list(peek);

  // load rules
  if (argc)
    peek_rules(peek, rules);

  // process data points
  for (i = 0; i < argc; i ++)
    peek_add(peek, argv[i]);

  // resolve
  peek_update(peek);
}

/*****************************************/

/*


static void peek_list_machine(peek *peek)
{
  (void) json_dumpf(peek->resolved, stdout, JSON_SORT_KEYS | JSON_INDENT(2));
  (void) fprintf(stdout, "\n");
}

static void peek_print(FILE *file, const char *key, json_t *value)
{
  if (json_is_string(value))
    (void) fprintf(file, "%s=%s\n", key, json_string_value(value));
  else
    (void) fprintf(file, "%s\n", key);
}

static void peek_list_human(peek *peek)
{
  json_t *value;
  const char *key;

  json_object_foreach(peek->resolved, key, value)
    peek_print(stdout, key, value);
}

static core_status peek_dequeue(core_event *event)
{
  peek *peek = event->state;
  peek_resolver *resolver;

  list_foreach(&peek->queue, resolver)
    {
      if (!resolver->running)
        {
          pool_enqueue(&peek->pool, peek_task, resolver);
          resolver->running = 1;
          peek->running++;
        }
    }

  if (peek->running && peek->pool_fd == -1)
    {
      peek->pool_fd = pool_fd(&peek->pool);
      core_add(NULL, peek_result, peek, peek->pool_fd, EPOLLIN | EPOLLET);
    }

  return CORE_OK;
}

static void peek_schedule(peek *peek)
{
  if (peek->next || list_empty(&peek->queue))
    return;

  peek->next = core_next(NULL, peek_dequeue, peek);
}

static int peek_is_resolved(peek *peek, char *key)
{
  return json_object_get(peek->resolved, key) != NULL;
}

static void peek_resolve(peek *peek, peek_rule *rule, char *key, json_t *value)
{
}

static int peek_is_queued(peek *peek, char *key)
{
  peek_resolver *resolver;

  list_foreach(&peek->queue, resolver)
    if (strcmp(resolver->key, key) == 0)
      return 1;
  return 0;
}


static void peek_queue_string(peek *peek, char *string)
{
  char *key, *delim;
  json_t *value;

  key = strdup(string);
  delim = strchr(key, '=');
  if (delim)
    {
      *delim = 0;
      value = json_string(delim + 1);
      peek_queue(peek, key, value);
      json_decref(value);
    }
  else
    peek_queue(peek, key, NULL);
  free(key);
}

*/

void peek_construct(peek *peek, int argc, char **argv)
{
  *peek = (struct peek) {.pool_fd = -1};
  data_construct(&peek->resolved);
  data_construct(&peek->queued);
  list_construct(&peek->queue);
  list_construct(&peek->rules);
  pool_construct(&peek->pool);

  peek_configure(peek, argc, argv);
}

void peek_destruct(peek *peek)
{
  peek_save(peek);
  free(peek->state);
  list_destruct(&peek->queue, NULL);
  data_destruct(&peek->queued);
  data_destruct(&peek->resolved);
  list_destruct(&peek->rules, peek_rules_release);
  pool_destruct(&peek->pool);
  *peek = (struct peek) {.pool_fd = -1};
}
