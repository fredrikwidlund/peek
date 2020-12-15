#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#include <dynamic.h>

#include "data.h"

static void data_set_release(maps_entry *entry)
{
  free(entry->key);
}

static char *data_key(char *value)
{
  char *delim;

  delim = strchr(value, '=');
  return delim ? strndup(value, delim - value) : strdup(value);
}

static int data_end(char *v)
{
  return v[0] == 0 || v[0] == '=';
}

static int data_integer(char *v)
{
  return isdigit(v[0]) || (v[0] == '-' && isdigit(v[1]));
}

static int data_compare(char *v1, char *v2)
{
  int64_t i1, i2;

  while (1)
  {
    if (data_end(v1))
      return data_end(v2) ? 0 : -1;
    if (data_end(v2))
      return 1;

    if (v1[0] == ':')
    {
      if (v2[0] == ':')
      {
        v1++;
        v2++;
        continue;
      }
      return -1;
    }
    if (v2[0] == ':')
      return 1;

    if (data_integer(v1))
    {
      if (!data_integer(v2))
        return -1;
      i1 = strtoll(v1, &v1, 0);
      i2 = strtoll(v2, &v2, 0);
      if (i1 < i2)
        return -1;
      if (i1 > i2)
        return 1;
      continue;
    }
    if (data_integer(v2))
      return 1;

    if (v1[0] < v2[0])
      return -1;
    if (v1[0] > v2[0])
      return 1;

    v1++;
    v2++;
  }
}

void data_construct(data *data)
{
  list_construct(&data->values);
  maps_construct(&data->set);
}

void data_destruct(data *data)
{
  list_destruct(&data->values, NULL);
  maps_destruct(&data->set, data_set_release);
}

int data_load(data *data, char *path)
{
  FILE *f;
  int status = 0;
  char *value = NULL;
  size_t size = 0;
  ssize_t n;

  f = fopen(path, "a+");
  if (!f)
    return -1;

  while (status == 0)
  {
    n = getline(&value, &size, f);
    if (n == -1)
      break;
    if (value[n - 1] == '\n')
      value[n - 1] = 0;
    if (data_valid(value))
      data_add(data, value);
    else
      status = -1;
  }

  free(value);
  fclose(f);
  return status;
}

int data_save(data *data, char *path)
{
  FILE *f;
  char *value;

  f = fopen(path, "w");
  if (!f)
    return -1;

  data_foreach(data, value)
    (void) fprintf(f, "%s\n", value);
  fclose(f);
  return 0;
}

int data_valid(char *value)
{
  int delims = 0;

  while (*value != '\0' && *value != '=')
  {
    if (!(isalnum(*value) || strchr("-_:*.", *value)))
      return 0;
    if (*value == ':')
      delims++;
    value++;
  }
  return delims != 0;
}

void data_add(data *data, char *value)
{
  char *key, *new, *after;

  key = data_key(value);
  if (maps_at(&data->set, key) == 0)
  {
    list_foreach(&data->values, after)
    {
      if (data_compare(value, after) < 0)
        break;
    }
    new = list_insert(after, value, strlen(value) + 1);
    maps_insert(&data->set, key, (uintptr_t) new, NULL);
  }
  else
    free(key);
}

void data_delete(data *data, char *value)
{
  char *key, *current;

  key = data_key(value);
  current = (char *) maps_at(&data->set, key);
  if (current)
  {
    list_erase(current, NULL);
    maps_erase(&data->set, key, data_set_release);
  }
  free(key);
}

void data_clear(data *data)
{
  maps_clear(&data->set, data_set_release);
  list_clear(&data->values, NULL);
}

int data_exists(data *data, char *value)
{
  char *key;
  int result;

  key = data_key(value);
  result = maps_at(&data->set, key);
  free(key);
  return result;
}
