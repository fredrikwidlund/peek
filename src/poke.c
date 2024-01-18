#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <regex.h>
#include <err.h>
#include <assert.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <sys/param.h>

#include "value.h"
#include "poke.h"

void poke_construct(poke_t *poke)
{
  *poke = (poke_t) {.jobs = vector_null(), .jobs_limit = 5};
}

void poke_destruct(poke_t *poke)
{
  vector_clear(poke->jobs);
  json_decref(poke->conf);
  json_decref(poke->params);
}

void poke_load(poke_t *poke, const char *conf)
{
  const char *params;

  poke->conf = json_load_file(conf, 0, NULL);
  if (!poke->conf)
    err(1, "unable to load configuration: %s", conf);
  params = json_string_value(json_object_get(poke->conf, "params"));
  if (!params)
    errx(1, "no parameter file specified");
  poke->params = json_load_file(params, 0, NULL);
  if (!poke->params)
    poke->params = json_object();
}

void poke_save(poke_t *poke)
{
  const char *params;
  int e;

  params = json_string_value(json_object_get(poke->conf, "params"));
  e = json_dump_file(poke->params, params, JSON_INDENT(2));
  if (e == -1)
    err(1, "json_dump_file");
}

static void poke_match_and_queue(poke_t *poke, const char *value, const char *pattern, json_t *command)
{
  regex_t reg;
  regmatch_t match[16];
  poke_job_t job = {.fd = -1};
  int e, i, n, x;
  json_t *arg;

  e = regcomp(&reg, pattern, REG_EXTENDED);
  if (e != 0)
    err(1, "regcomp");
  e = regexec(&reg, value, 16, match, 0);
  if (e == 0)
  {
    assert(json_is_array(command));
    n = json_array_size(command);
    job.args = calloc(n + 1, sizeof (char *));
    for (i = 0; i < n; i++)
    {
      arg = json_array_get(command, i);
      if (json_is_string(arg))
        job.args[i] = strdup(json_string_value(arg));
      else
      {
        x = json_number_value(arg);
        assert(x >= 0 && x < 16);
        job.args[i] = strndup(value + match[x].rm_so, match[i].rm_eo - match[i].rm_so);
      }
    }
    poke->jobs = vector_push(poke->jobs, poke_job_t, job);
  }
  regfree(&reg);
}

static void poke_param_print(poke_param_t p, FILE *f)
{
  if (p.value)
    (void) fprintf(f, "%s=%s\n", p.key, p.value);
  else
    (void) fprintf(f, "%s\n", p.key);
}

void poke_add(poke_t *poke, const char *value)
{
  poke_param_t p = {.key = value};
  char *delim;
  const char *pattern;
  json_t *command;

  json_object_foreach(json_object_get(poke->conf, "rules"), pattern, command)
    poke_match_and_queue(poke, value, pattern, command);

  delim = strchr(value, '=');
  if (delim)
    p = (poke_param_t) {.key = strndup(value, delim - value), .value = delim + 1};
  json_object_set_new(poke->params, p.key, p.value ? json_string(p.value) : json_null());
  poke_param_print(p, stdout);
  if (delim)
    free((void *) p.key);
}

static int poke_param_sort(const void *arg1, const void *arg2)
{
  const poke_param_t *p1 = arg1, *p2 = arg2;
  const char *a = p1->key, *b = p2->key;
  size_t i = 0;

  while (a[i] == b[i] && a[i])
    i++;
  if (isdigit(a[i]) || isdigit(b[i]))
  {
    while (i > 0 && isdigit(a[i - 1]))
      i--;
    if (isdigit(a[i]) && isdigit(b[i]))
      return strtoull(a + i, NULL, 0) > strtoull(b + i, NULL, 0);
  }
  return strcmp(a + i, b + i);
}

void poke_list(poke_t *poke)
{
  const char *key;
  json_t *value;
  vector_t v;
  size_t i;

  v = vector_null();
  json_object_foreach(poke->params, key, value)
    v = vector_push(v, poke_param_t, (poke_param_t) {key, json_string_value(value)});
  v = vector_sort(v, poke_param_t, poke_param_sort);
  for (i = 0; i < vector_length(v, poke_param_t); i ++)
    poke_param_print(vector_at(v, poke_param_t, i), stdout);
  vector_clear(v);
}

void poke_run(poke_t *poke, int efd)
{
  size_t i;
  poke_job_t *job;
  int fd[2], e;
  pid_t cpid;

  i = poke->jobs_started;
  job = &vector_at(poke->jobs, poke_job_t, i);
  poke->jobs_started++;
  poke->jobs_running++;

  e = pipe(fd);
  if (e == -1)
    err(1, "pipe");
  cpid = fork();
  if (cpid == 0)
  {
    char path[PATH_MAX] = {0};

    (void) close(fd[0]);
    dup2(fd[1], 1);
    (void) close(fd[1]);

    snprintf(path, sizeof path, "%s/.poke.d/modules/%s", getenv("HOME"), job->args[0]);
    (void) execve(path, job->args, NULL);
    err(1, "execve: %s", job->args[0]);
  }
  (void) close(fd[1]);
  job->pid = cpid;
  job->fd = fd[0];
  job->file = fdopen(job->fd, "r");
  e = epoll_ctl(efd, EPOLL_CTL_ADD, job->fd, (struct epoll_event []) {{.events = EPOLLIN, .data.u64 = i}});
  if (e == -1)
    err(1, "epoll_ctl");
}

void poke_result(poke_t *poke, int efd, size_t i)
{
  poke_job_t job;
  char *line = NULL, **arg;
  size_t n = 0;
  ssize_t e;

  job = vector_at(poke->jobs, poke_job_t, i);
  e = getline(&line, &n, job.file);
  if (e >= 0)
  {
    if (e > 0 && line[e - 1] == '\n')
      line[e - 1] = 0;
    poke_add(poke, line);
  }
  else
  {
    e = waitpid(job.pid, NULL, 0);
    assert(e == job.pid);
    e = epoll_ctl(efd, EPOLL_CTL_DEL, job.fd, NULL);
    if (e == -1)
      err(1, "epoll_ctl");
    (void) close(job.fd);
    fclose(job.file);
    for (arg = job.args; *arg; arg++)
      free(*arg);
    free(job.args);
    poke->jobs_running--;
  }
  free(line);
}

void poke_process(poke_t *poke)
{
  struct epoll_event events[poke->jobs_limit];
  int efd, n, i;

  efd = epoll_create1(0);
  if (efd == -1)
    err(1, "epoll_create1");

  while (1)
  {
    while (poke->jobs_started < vector_length(poke->jobs, poke_job_t) &&
           poke->jobs_running < poke->jobs_limit)
      poke_run(poke, efd);
    if (!poke->jobs_running)
      break;

    n = epoll_wait(efd, events, poke->jobs_limit, -1);
    if (n == -1)
      err(1, "epoll_wait");
    for (i = 0; i < n; i++)
      poke_result(poke, efd, events[i].data.u64);
  }

  close(efd);
}
