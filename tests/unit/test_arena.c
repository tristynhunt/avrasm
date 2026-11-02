#include "avrasm/arena.h"
#include "check.h"

#include <stdint.h>
#include <string.h>

int main(void)
{
  Arena a;
  arenaInit(&a, 256);

  int align8_ok = 1, align16_ok = 1;
  for (int i = 0; i < 100; i++) {
    char *c = arenaAlloc(&a, 1, 1);
    void *p8 = arenaAlloc(&a, 24, 8);
    void *p16 = arenaAlloc(&a, 16, 16);
    REQUIRE(c != NULL && p8 != NULL && p16 != NULL);
    if ((uintptr_t)p8 % 8 != 0) align8_ok = 0;
    if ((uintptr_t)p16 % 16 != 0) align16_ok = 0;
  }
  CHECK(align8_ok);
  CHECK(align16_ok);

  unsigned char *z = arenaAlloc(&a, 100, 1);
  REQUIRE(z != NULL);
  int all_zero = 1;
  for (int i = 0; i < 100; i++) {
    if (z[i] != 0) all_zero = 0;
  }
  CHECK(all_zero);

  enum { N = 50 };
  unsigned char *bufs[N];
  for (int i = 0; i < N; i++) {
    bufs[i] = arenaAlloc(&a, 32, 1);
    REQUIRE(bufs[i] != NULL);
    memset(bufs[i], i + 1, 32);
  }
  int intact = 1;
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < 32; j++) {
      if (bufs[i][j] != i + 1) intact = 0;
    }
  }
  CHECK(intact);

  int *nums = arenaAlloc(&a, 1000 * sizeof *nums, _Alignof(int));
  REQUIRE(nums != NULL);
  for (int i = 0; i < 1000; i++) nums[i] = i;
  int ok = 1;
  for (int i = 0; i < 1000; i++) {
    if (nums[i] != i) ok = 0;
  }
  CHECK(ok);
  CHECK(bufs[0][0] == 1);
  CHECK(bufs[N - 1][31] == N);

  char *s1 = arenaStrdup(&a, "hello");
  char *s2 = arenaStrndup(&a, "worldwide", 5);
  char *s3 = arenaStrndup(&a, "hi", 10);
  char *s4 = arenaStrdup(&a, "");
  CHECK_STR(s1, "hello");
  CHECK_STR(s2, "world");
  CHECK_STR(s3, "hi");
  CHECK_STR(s4, "");

  arenaFree(&a);
  Arena empty;
  arenaInit(&empty, 0);
  arenaFree(&empty);
  arenaInit(&a, 0);
  CHECK_STR(arenaStrdup(&a, "reuse"), "reuse");
  arenaFree(&a);

  return TEST_RESULT();
}