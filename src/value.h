#ifndef VALUE_H
#define VALUE_H

#include <string.h>
#include <stdint.h>
#include <sys/uio.h>

#define VALUE_INLINE inline __attribute__((always_inline))

VALUE_INLINE uint64_t value_roundup(uint64_t x)
{
  if (x == 0)
    return 0;
  if (x < 16)
    return 16;
  return 1 << (64 - __builtin_clzll(x - 1));
}

/*************/
/* data type */
/*************/

#define data_element(d, type, i)        (((type *) data_base(d))[i])

typedef struct iovec data_t;

VALUE_INLINE data_t data(const void *base, size_t size)
{
  return (data_t) {.iov_base = (void *) base, .iov_len = size};
}

VALUE_INLINE data_t data_null(void)
{
  return data(NULL, 0);
}

VALUE_INLINE const void *data_base(const data_t d)
{
  return d.iov_base;
}

VALUE_INLINE const void *data_base_offset(const data_t d, size_t size)
{
  return (uint8_t *) d.iov_base + size;
}

VALUE_INLINE size_t data_size(const data_t d)
{
  return d.iov_len;
}

VALUE_INLINE data_t data_realloc(const data_t d, size_t size)
{
  if (size == 0)
  {
    free((void *) data_base(d));
    return data_null();
  }

  return data(realloc((void *) data_base(d), size), data_size(d));
}

VALUE_INLINE data_t data_resize(const data_t d, size_t size)
{
  return data(data_base(d), size);
}

VALUE_INLINE data_t data_clear(const data_t d)
{
  return data_realloc(d, 0);
}

#define vector(type, ...)               data((type[]) {__VA_ARGS__}, sizeof ((type[]) {__VA_ARGS__}))
#define vector_length(v, type)          (data_size(v) / sizeof (type))
#define vector_at(v, type, i)           data_element(v, type, i)
#define vector_push(v, type, ...)       vector_push_args(v, vector(type, __VA_ARGS__))
#define vector_sort(v, type, compare)   vector_sort_size(v, sizeof (type), compare)

typedef data_t vector_t;

VALUE_INLINE vector_t vector_null(void)
{
  return data(NULL, 0);
}

VALUE_INLINE vector_t vector_clear(vector_t v)
{
  return data_clear(v);
}

VALUE_INLINE vector_t vector_resize(vector_t v, size_t size)
{
  if (value_roundup(data_size(v) != value_roundup(size)))
    v = data_realloc(v, value_roundup(size));
  return data_resize(v, size);
}

VALUE_INLINE vector_t vector_push_args(vector_t v, vector_t args)
{
  size_t size = data_size(v);

  v = vector_resize(v, data_size(v) + data_size(args));
  memcpy((void *) data_base_offset(v, size), data_base(args), data_size(args));
  return v;
}

VALUE_INLINE vector_t vector_sort_size(vector_t v, size_t element_size, int compare(const void *, const void *))
{
  qsort((void *) data_base(v), data_size(v) / element_size, element_size, compare);
  return v;
}

#endif /* VALUE_H */
