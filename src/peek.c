#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <poll.h>
#include <getopt.h>
#include <pthread.h>
#include <err.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <dynamic.h>
#include <reactor.h>

#include "rule.h"
#include "peek.h"

static void peek_update(peek *);
static void peek_add(peek *, char *);

static void peek_rules_release(void *arg)
{
  rule_destruct(arg);
}

static void peek_debug(peek *peek, const char *format, ...)
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

static void peek_stream_input(peek *peek, stream *stream)
{
  segment line;
  char *value;

  while (1)
  {
    line = stream_read_line(stream);
    if (line.size == 0)
      return;
    value = strndup(line.base, line.size - 1);
    peek_add(peek, value);
    stream_consume(stream, line.size);
    free(value);
  }
}

static core_status peek_input(core_event *event)
{
  peek *peek = event->state;

  if (event->type == STREAM_READ)
  {
    peek_stream_input(peek, &peek->input);
    return CORE_OK;
  }

  stream_destruct(&peek->input);
  peek_update(peek);
  return CORE_ABORT;
}

static void peek_resolver_release(void *arg)
{
  peek_resolver *resolver = arg;

  if (resolver->pid)
  {
    kill(resolver->pid, SIGKILL);
    stream_destruct(&resolver->input);
  }
  *resolver = (peek_resolver) {0};
}


static core_status peek_resolver_input(core_event *event)
{
  peek_resolver *resolver = event->state;
  peek *peek = resolver->peek;

  if (event->type == STREAM_READ)
  {
    peek_stream_input(peek, &resolver->input);
    return CORE_OK;
  }

  list_erase(resolver, peek_resolver_release);
  peek_update(peek);
  return CORE_ABORT;
}

static void peek_resolver_construct(peek_resolver *resolver, peek *peek, rule *rule, char *value)
{
  *resolver = (peek_resolver) {0};
  resolver->peek = peek;
  resolver->rule = rule;
  resolver->value = value;

  stream_construct(&resolver->input, peek_resolver_input, resolver);
}

static void peek_resolver_run(peek_resolver *resolver)
{
  int e, fd[2];

  e = pipe(fd);
  if (e == -1)
    err(1, "pipe");

  resolver->pid = fork();
  if (resolver->pid == -1)
    err(1, "fork");

  if (resolver->pid == 0)
  {
    (void) close(fd[0]);
    e = dup2(fd[1], 1);
    if (e == -1)
      err(1, "dup2");
    rule_exec(resolver->rule, resolver->value);
    err(1, "rule_exec");
  }

  (void) close(fd[1]);
  stream_open(&resolver->input, fd[0]);
  peek_debug(resolver->peek, "running resolver for %s %s", resolver->rule->name, resolver->value);
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

static void peek_add(peek *peek, char *input)
{
  rule *rule;
  peek_resolver *resolver;
  char *value;

  if (data_valid(input) == 0)
  {
    warnx("invalid value %s", input);
    return;
  }

  if (data_exists(&peek->data, input) && (peek->flags & PEEK_FLAG_OVERRIDE) == 0)
  {
    peek_debug(peek, "found existing value matching %s", input);
    return;
  }

  if (peek->flags & PEEK_FLAG_DRY_RUN)
  {
    list_foreach(&peek->rules, rule)
      if (rule_match(rule, input))
        (void) fprintf(stderr, "rule: %s %s\n", rule->name, input);
    return;
  }

  value = data_add(&peek->data, input);
  (void) fprintf(stdout, "%s\n", value);

  list_foreach(&peek->rules, rule)
    if (rule_match(rule, value))
    {
      peek_debug(peek, "applying rule %s to %s", rule->name, value);
      resolver = list_push_back(&peek->resolvers, NULL, sizeof *resolver);
      peek_resolver_construct(resolver, peek, rule, value);
    }

  peek_update(peek);
}

static void peek_update(peek *peek)
{
  peek_resolver *resolver;
  int n = 0;

  peek_debug(peek, "updating state");
  list_foreach(&peek->resolvers, resolver)
  {
    if (n == PEEK_MAX_RESOLVERS)
      return;
    if (resolver->pid == 0)
      peek_resolver_run(resolver);
    n++;
  }
}

static void peek_usage(void)
{
  extern char *__progname;

  (void) fprintf(stderr, "Usage: %s [OPTION]... [DATA POINT]...\n", __progname);
  (void) fprintf(stderr, "peek discovery framework\n");
  (void) fprintf(stderr, "\n");
  (void) fprintf(stderr, "Options:\n");
  (void) fprintf(stderr, "    -c PATTERN      clear matching values\n");
  (void) fprintf(stderr, "    -d              debug\n");
  (void) fprintf(stderr, "    -h              display this help\n");
  (void) fprintf(stderr, "    -l              list state (default if no other arguments)\n");
  (void) fprintf(stderr, "    -n              dry run, show matching rules for input\n");
  (void) fprintf(stderr, "    -o              added data points override old values\n");
  (void) fprintf(stderr, "    -r PATH         load rules from PATH (defaults to /usr/share/peek/)\n");
  (void) fprintf(stderr, "    -s PATH         use PATH to persist state (defaults to $HOME/.peek)\n");
}


static void peek_configure(peek *peek, int argc, char **argv)
{
  int c, e, i, is_stream, has_input;
  char *rules = NULL, *state = NULL;

  while (1)
  {
    c = getopt(argc, argv, "cdhlnor:s:");
    if (c == -1)
      break;
    switch (c)
    {
    case 'c':
      peek->flags |= PEEK_FLAG_CLEAR;
      break;
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
  peek_debug(peek, "loading state from %s", peek->state);
  e = data_load(&peek->data, peek->state);
  if (e == -1)
    warn("unable to load data data points from %s", peek->state);

  // clear values
  if (peek->flags & PEEK_FLAG_CLEAR)
    while (argc)
    {
      data_clear(&peek->data, argv[0]);
      argc--;
      argv++;
    }

  // list data
  if (peek->flags & PEEK_FLAG_LIST || has_input == 0)
    peek_list(peek);

  // load rules
  if (has_input)
    peek_rules(peek, rules);

  // add data points from command line
  for (i = 0; i < argc; i ++)
    peek_add(peek, argv[i]);

  // add data points from standard in
  if (is_stream)
    stream_open(&peek->input, 0);

  // resolve
  peek_update(peek);
}

void peek_construct(peek *peek, int argc, char **argv)
{
  *peek = (struct peek) {.active = 1};
  data_construct(&peek->data);
  list_construct(&peek->resolvers);
  list_construct(&peek->rules);
  stream_construct(&peek->input, peek_input, peek);
  peek_configure(peek, argc, argv);
}

void peek_destruct(peek *peek)
{
  int e;

  if (peek->active)
  {
    peek_debug(peek, "saving state to %s", peek->state);
    e = data_save(&peek->data, peek->state);
    if (e == -1)
      warn("unable to saved data data points to %s", peek->state);
    free(peek->state);
    data_destruct(&peek->data);
    list_destruct(&peek->rules, peek_rules_release);
    stream_destruct(&peek->input);
    *peek = (struct peek) {0};
  }
}
