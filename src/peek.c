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
#include <reactor.h>

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
  (void) fprintf(stderr, "    -a              add values\n");

  (void) fprintf(stderr, "    -r PATH         set rules PATH (defaults to /usr/share/peek/)\n");
  (void) fprintf(stderr, "    -s PATH         use PATH to persist state (defaults to $HOME/.peek)\n");
  (void) fprintf(stderr, "    -i              read data points from stdin\n");
  (void) fprintf(stderr, "    -o              new data points override old values\n");
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

static void peek_add(peek *peek, char *value, int override)
{
  rule *rule;
  peek_resolver *resolver;
  int matches = 0;

  if ((data_exists(&peek->resolved, value) && override == 0) ||
      data_exists(&peek->queued, value))
    return;

  list_foreach(&peek->rules, rule)
  {
    if (rule_match(rule, value))
    {
      resolver = list_push_back(&peek->queue, NULL, sizeof *resolver);
      resolver->peek = peek;
      resolver->rule = rule;
      resolver->value = strdup(value);
      matches++;
      peek_debug(peek, "rule %s applied to %s", rule->name, value);
    }
  }

  peek_debug(peek, "value %s added with %d matching rules", value, matches);
  data_add(matches ? &peek->queued : &peek->resolved, value);
  (void) fprintf(stdout, "%s\n", value);
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
      peek_add(resolver->peek, line, 0);
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

static core_status peek_stream(core_event *event)
{
  peek *peek = event->state;
  segment line;
  char *value;

  switch (event->type)
  {
  case STREAM_READ:
    while (1)
    {
      line = stream_read_line(&peek->input);
      if (line.size == 0)
        break;
      value = strndup(line.base, line.size - 1);
      peek_add(peek, value, peek->flags & PEEK_FLAG_OVERRIDE ? 1 : 0);
      stream_consume(&peek->input, line.size);
      free(value);
    }
    peek_update(peek);
    return CORE_OK;
  default:
    stream_close(&peek->input);
    return CORE_ABORT;
  }
}

static void peek_configure(peek *peek, int argc, char **argv)
{
  int c, e, i, stream = 0;
  char *rules = NULL, *home = getenv("HOME"), *value;

  (void) asprintf(&peek->state, "%s/.peek", home ? home : "/");
  while (1)
  {
    c = getopt(argc, argv, "cls:v");
    //c = getopt(argc, argv, "lvadhior:s:");
    if (c == -1)
      break;
    switch (c)
    {
    case 'c':
      peek->flags |= PEEK_FLAG_CLEAR;
      break;
    case 'l':
      peek->flags |= PEEK_FLAG_LIST;
      break;
    case 's':
      free(peek->state);
      peek->state = strdup(optarg);
      break;
    case 'v':
      peek->verbose++;
      break;

    case 'a':
      peek->flags |= PEEK_FLAG_LIST;
      break;
    case 'd':
      peek->flags |= PEEK_FLAG_DEBUG;
      break;
    case 'i':
      stream = 1;
      break;
    case 'o':
      peek->flags |= PEEK_FLAG_OVERRIDE;
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
  if (argc == 0)
    peek->flags |= PEEK_FLAG_LIST;

  e = data_load(&peek->resolved, peek->state);
  if (e == -1)
  {
    warnx("invalid state in %s, aborting to avoiding corrupting data", peek->state);
    return;
  }

  if (peek->flags & PEEK_FLAG_CLEAR)
    data_clear(&peek->resolved);

  if (peek->flags & PEEK_FLAG_LIST)
    data_foreach(&peek->resolved, value)
      (void) fprintf(stdout, "%s\n", value);

// setup state directory
  //  peek_state(peek, state);

  // load state
  //peek_load(peek);

  // list current resolved values if requested

  // load rules
  //if (argc || stream)
  //  peek_rules(peek, rules);

  // process data points
  //for (i = 0; i < argc; i ++)
  //  peek_add(peek, argv[i], peek->flags & PEEK_FLAG_OVERRIDE ? 1 : 0);

  // resolve
  //peek_update(peek);

  // stream input
  //if (stream)
  //  stream_open(&peek->input, 0);
}

void peek_construct(peek *peek, int argc, char **argv)
{
  *peek = (struct peek) {.pool_fd = -1};
  data_construct(&peek->resolved);
  data_construct(&peek->queued);
  list_construct(&peek->queue);
  list_construct(&peek->rules);
  pool_construct(&peek->pool);
  stream_construct(&peek->input, peek_stream, peek);
  peek_configure(peek, argc, argv);
}

void peek_destruct(peek *peek)
{
  //peek_save(peek);
  free(peek->state);
  list_destruct(&peek->queue, NULL);
  data_destruct(&peek->queued);
  data_destruct(&peek->resolved);
  list_destruct(&peek->rules, peek_rules_release);
  pool_destruct(&peek->pool);
  stream_destruct(&peek->input);
  *peek = (struct peek) {.pool_fd = -1};
}
