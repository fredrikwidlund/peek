#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/ioctl.h>
#include <poll.h>
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

static void peek_setup(peek *peek, char *state)
{
  char *home;
  int e;

  if (state)
    peek->state = strdup(state);
  else
  {
    home = getenv("HOME");
    e = asprintf(&peek->state, "%s/.peek", home ? home : "/");
    if (e == -1)
      err(1, "asprintf");
  }
  peek_debug(peek, "persisting state in %s", peek->state);
}

static void peek_load(peek *peek)
{
  int e;

  peek_debug(peek, "loading state");
  e = data_load(&peek->data, peek->state);
  if (e == -1)
    warn("unable to load data data points from %s", peek->state);
}

static void peek_rules(peek *peek, char *dir)
{
  int i, n, count = 0;
  struct dirent **namelist;
  char path[PATH_MAX];
  rule *rule;

  if (!dir)
    dir = "/usr/share/peek";
  n = scandir(dir, &namelist, NULL, NULL);
  if (n > 0)
  {
    for (i = 0; i < n; i ++)
    {
      if (namelist[i]->d_type == DT_REG)
      {
        (void) snprintf(path, sizeof path, "%s/%s", dir, namelist[i]->d_name);
        peek_debug(peek, "loading rule from %s", path);
        rule = list_push_back(&peek->rules, NULL, sizeof *rule);
        rule_construct(rule, path);
        if (rule_valid(rule))
          count++;
        else
        {
          list_erase(rule, NULL);
          warnx("discarding rule %s", path);
        }
      }
      free(namelist[i]);
    }
    free(namelist);
  }

  peek_debug(peek, "loaded %d rules", count);
  if (count == 0)
    warnx("no rules loaded");
}

static void peek_list(peek *peek)
{
  char *value;

  peek_debug(peek, "listing values");
  data_foreach(&peek->data, value)
    (void) fprintf(stdout, "%s\n", value);
}

static void peek_add(peek *peek, char *value, int override)
{
  rule *rule;
  peek_resolver *resolver;
  int e, matches = 0;

  if (data_valid(value) == 0)
  {
    warnx("invalid value %s", value);
    return;
  }

  if (data_exists(&peek->data, value))
  {
    peek_debug(peek, "found existing value matching %s", value);
    if (override == 0)
      return;
    peek_debug(peek, "overriding existing value of %s", value);
  }

  if ((data_exists(&peek->data, value) && override == 0) ||
      data_exists(&peek->queued, value))
    return;

  list_foreach(&peek->rules, rule)
    if (rule_match(rule, value))
    {
      if (peek->flags & PEEK_FLAG_DRY_RUN)
        (void) fprintf(stderr, "%s %s\n", rule->name, value);
      else
      {
        resolver = list_push_back(&peek->queue, NULL, sizeof *resolver);
        resolver->peek = peek;
        resolver->rule = rule;
        resolver->value = strdup(value);
        matches++;
        peek_debug(peek, "applying rule %s to %s", rule->name, value);
      }
    }

  peek_debug(peek, "%d matching rules queued for %s", matches, value);
  if (peek->flags & PEEK_FLAG_DRY_RUN)
    return;
  e = data_add(matches ? &peek->queued : &peek->data, value);
  if (e == 0)
    (void) fprintf(stdout, "%s\n", value);
  if (e == -1)
    warnx("invalid value %s", value);
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
      (void) data_add(&peek->data, completed->value);
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
    {
      peek_debug(peek, "resolver limit reached");
      break;
    }
    pool_enqueue(&peek->pool, peek_task, resolver);
    resolver->running = 1;
    peek->resolvers++;
  }

  if (peek->resolvers && peek->pool_fd == -1)
  {
    peek_debug(peek, "starting up pool");
    peek->pool_fd = pool_fd(&peek->pool);
    core_add(NULL, peek_poll, peek, peek->pool_fd, EPOLLIN | EPOLLET);
  }
  else if (peek->resolvers == 0 && peek->pool_fd >= 0)
  {
    peek_debug(peek, "shutting down pool");
    core_delete(NULL, peek->pool_fd);
    peek->pool_fd = -1;
  }
}

static void peek_input_destruct(peek_input *input)
{
  stream_destruct(&input->stream);
}

static core_status peek_input_event(core_event *event)
{
  peek_input *input = event->state;
  segment line;
  char *value;

  switch (event->type)
  {
  case STREAM_READ:
    while (1)
    {
      line = stream_read_line(&input->stream);
      if (line.size == 0)
        break;
      value = strndup(line.base, line.size - 1);
      peek_add(input->peek, value, input->override);
      stream_consume(&input->stream, line.size);
      free(value);
    }
    peek_update(input->peek);
    return CORE_OK;
  default:
    peek_input_destruct(input);
    return CORE_ABORT;
  }
}

static void peek_input_construct(peek_input *input, peek *peek)
{
  input->peek = peek;
  stream_construct(&input->stream, peek_input_event, input);
}

static void peek_input_open(peek_input *input, int fd, int override)
{
  input->override = override;
  stream_open(&input->stream, fd);
}


/******************************/

static void peek_usage(void)
{
  extern char *__progname;

  (void) fprintf(stderr, "Usage: %s [OPTION]... [DATA POINT]...\n", __progname);
  (void) fprintf(stderr, "peek discovery framework\n");
  (void) fprintf(stderr, "\n");
  (void) fprintf(stderr, "Options:\n");
  (void) fprintf(stderr, "    -d              debug\n");
  (void) fprintf(stderr, "    -h              display this help\n");
  (void) fprintf(stderr, "    -l              list state (default if no other arguments)\n");
  (void) fprintf(stderr, "    -n              dry run, show matching rules for input\n");
  (void) fprintf(stderr, "    -o              added data points override old values\n");
  (void) fprintf(stderr, "    -r PATH         load rules from PATH (defaults to /usr/share/peek/)\n");
  (void) fprintf(stderr, "    -s PATH         use PATH to persist state (defaults to $HOME/.peek)\n");
}

static void peek_rules_release(void *arg)
{
  rule_destruct(arg);
}


static void peek_save(peek *peek)
{
  char path[PATH_MAX];
  int e;

  (void) snprintf(path, sizeof path, "%s/data", peek->state);
  e = data_save(&peek->data, path);
  if (e == -1)
    warn("unable to saved data data points to %s", path);

  (void) snprintf(path, sizeof path, "%s/queued", peek->state);
  e = data_save(&peek->queued, path);
  if (e == -1)
    warn("unable to save queued data points to %s", path);
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

/*****************************************************/

static void peek_configure(peek *peek, int argc, char **argv)
{
  int c, i, is_stream, has_input;
  char *rules = NULL, *state = NULL;

  while (1)
  {
    c = getopt(argc, argv, "dhlnor:s:");
    if (c == -1)
      break;
    switch (c)
    {
    case 'd':
      peek->flags |= PEEK_FLAG_DEBUG;
      break;
    case 'l':
      peek->flags |= PEEK_FLAG_LIST;
      break;
    case 'n':
      peek->flags |= PEEK_FLAG_DRY_RUN;
      break;
    case 'o':
      peek->flags |= PEEK_FLAG_OVERRIDE;
      break;
    case 'r':
      rules = optarg;
      break;
    case 's':
      state = optarg;
      break;
    default:
      peek_usage();
      return;
    }
  }

  argc -= optind;
  argv += optind;
  is_stream = isatty(0) == 0;
  has_input = argc || is_stream;

  // setup state directory
  peek_setup(peek, state);

  // load state
  peek_load(peek);

  // list data
  if (peek->flags & PEEK_FLAG_LIST || has_input == 0)
    peek_list(peek);

  // load rules
  if (has_input)
    peek_rules(peek, rules);

  // add data points
  for (i = 0; i < argc; i ++)
    peek_add(peek, argv[i], peek->flags & PEEK_FLAG_OVERRIDE ? 1 : 0);

  // stream input
  if (is_stream)
    peek_input_open(&peek->stdin, 0, peek->flags & PEEK_FLAG_OVERRIDE ? 1 : 0);

  // resolve
  peek_update(peek);
}

void peek_construct(peek *peek, int argc, char **argv)
{
  *peek = (struct peek) {.pool_fd = -1};
  data_construct(&peek->data);
  data_construct(&peek->queued);
  list_construct(&peek->queue);
  list_construct(&peek->rules);
  pool_construct(&peek->pool);
  peek_input_construct(&peek->stdin, peek);
  stream_construct(&peek->input, peek_stream, peek);
  peek_configure(peek, argc, argv);
}

void peek_destruct(peek *peek)
{
  peek_save(peek);
  free(peek->state);
  list_destruct(&peek->queue, NULL);
  data_destruct(&peek->queued);
  data_destruct(&peek->data);
  list_destruct(&peek->rules, peek_rules_release);
  pool_destruct(&peek->pool);
  stream_destruct(&peek->input);
  peek_input_destruct(&peek->stdin);
  *peek = (struct peek) {.pool_fd = -1};
}
