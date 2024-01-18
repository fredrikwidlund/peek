#include <unistd.h>
#include "poke.h"

int main(int argc, char **argv)
{
  poke_t poke;
  int i;

  poke_construct(&poke);
  poke_load(&poke, "/root/.poke.d/conf");
  if (argc <= 1)
  {
    poke_list(&poke);
  }
  else
  {
    for (i = 1; i < argc; i ++)
      poke_add(&poke, argv[i]);
    poke_process(&poke);
    poke_save(&poke);
  }

  poke_destruct(&poke);
}

