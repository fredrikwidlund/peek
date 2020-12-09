#include <stdlib.h>
#include <stdint.h>
#include <signal.h>

#include <dynamic.h>
#include <reactor.h>

#include "peek.h"

static void cancel(int arg)
{
  (void) arg;
  core_abort(NULL);
}

int main(int argc, char **argv)
{
  peek peek;

  signal(SIGTERM, cancel);
  signal(SIGINT, cancel);
  signal(SIGCHLD, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);

  core_construct(NULL);
  peek_construct(&peek, argc, argv);
  core_loop(NULL);
  peek_destruct(&peek);
  core_destruct(NULL);
}
