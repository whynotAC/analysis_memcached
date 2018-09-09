#pragma once

#include <string>
#include <cstdint>
#include <cstring>
#include <errno.h>

/*
 * Wrappers around strtoull/strtoll that are safer and easier to
 * use.  for tests and assumptions, see internal_tests.c.
 *
 * str   a NULL-terminated base decimal 10 unsigned integer
 * out   out parameter, if conversion succeeded
 *
 * returns true if conversion succeeded
 */
bool safe_strtoull(const char *str, uint64_t *out);
