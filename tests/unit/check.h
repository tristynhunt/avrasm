// Tiny test helpers.
//
//   CHECK(cond)           record a failure and carry on
//   CHECK_INT(got, want)  compare integers, print both on failure
//   CHECK_STR(got, want)  compare strings (NULL counts as a failure)
//   REQUIRE(cond)         record a failure and return 1 from main() at once;
//                         use it before dereferencing something that might be NULL
//
// A test's main() ends with:  return TEST_RESULT();
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>
#include <string.h>

static int check_failures_;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      check_failures_++; \
    } \
  } while (0)

#define CHECK_INT(got, want) \
  do { \
    long long g_ = (long long)(got), w_ = (long long)(want); \
    if (g_ != w_) { \
      fprintf(stderr, "%s:%d: %s: got %lld, want %lld\n", \
              __FILE__, __LINE__, #got, g_, w_); \
      check_failures_++; \
    } \
  } while (0)

#define CHECK_STR(got, want) \
  do { \
    const char *g_ = (got), *w_ = (want); \
    if (!g_ || strcmp(g_, w_) != 0) { \
      fprintf(stderr, "%s:%d: %s: got \"%s\", want \"%s\"\n", \
              __FILE__, __LINE__, #got, g_ ? g_ : "(null)", w_); \
      check_failures_++; \
    } \
  } while (0)

#define REQUIRE(cond) \
  do { \
    if (!(cond)) { \
      fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #cond); \
      return 1; \
    } \
  } while (0)

#define TEST_RESULT() \
  (check_failures_ \
     ? (fprintf(stderr, "%d check(s) failed\n", check_failures_), 1) \
     : (puts("all checks passed"), 0))

#endif