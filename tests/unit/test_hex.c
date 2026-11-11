#include "avrasm/assemble.h"
#include "avrasm/hex.h"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *readFixture(void *user, const char *path, size_t *len)
{
  (void)user;
  if (strcmp(path, "inc/empty.inc") != 0) return NULL;
  *len = 0;
  return calloc(1, 1);
}

static const struct {
  int n;
  const char *src;
  const char *flash;
  const char *eeprom;
} rows[] = {
  { 1800, "nop\n",
    ":020000020000FC\r\n:020000000000FE\r\n:00000001FF\r\n",
    NULL },
  { 1801, "nop\nnop\nnop\n",
    ":020000020000FC\r\n:06000000000000000000FA\r\n:00000001FF\r\n",
    NULL },
  { 1802, ".dw 1, 2, 3, 4, 5, 6, 7, 8\n",
    ":020000020000FC\r\n:1000000001000200030004000500060007000800CC\r\n:00000001FF\r\n",
    NULL },
  { 1803, ".dw 1, 2, 3, 4, 5, 6, 7, 8, 9\n",
    ":020000020000FC\r\n:1000000001000200030004000500060007000800CC\r\n:020010000900E5\r\n:00000001FF\r\n",
    NULL },
  { 1804, ".org 3\n.dw 1, 2, 3, 4, 5, 6, 7, 8, 9, 10\n",
    ":020000020000FC\r\n:1000060001000200030004000500060007000800C6\r\n:0400160009000A00D3\r\n:00000001FF\r\n",
    NULL },
  { 1805, ".db 1, 2, 3\n",
    ":020000020000FC\r\n:0400000001020300F6\r\n:00000001FF\r\n",
    NULL },
  { 1806, "nop\n.org 0x10\nnop\n",
    ":020000020000FC\r\n:020000000000FE\r\n:020020000000DE\r\n:00000001FF\r\n",
    NULL },
  { 1807, ".org 4\nnop\n",
    ":020000020000FC\r\n:020008000000F6\r\n:00000001FF\r\n",
    NULL },
  { 1808, "nop\n.org 2\nnop\n",
    ":020000020000FC\r\n:020000000000FE\r\n:020004000000FA\r\n:00000001FF\r\n",
    NULL },
  { 1809, ".org 0x7fff\n.dw 1, 2\n",
    ":020000020000FC\r\n:02FFFE00010000\r\n:020000021000EC\r\n:020000000200FC\r\n:00000001FF\r\n",
    NULL },
  { 1810, ".org 0x8000\nnop\n",
    ":020000021000EC\r\n:020000000000FE\r\n:00000001FF\r\n",
    NULL },
  { 1811, ".eseg\n.db 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17\n",
    ":00000001FF\r\n",
    ":100000000102030405060708090A0B0C0D0E0F1068\r\n:0100100011DE\r\n:00000001FF\r\n" },
  { 1812, ".eseg\n.org 0x10\n.db 1\n.org 0x20\n.db 2\n",
    ":00000001FF\r\n",
    ":0100100001EE\r\n:0100200002DD\r\n:00000001FF\r\n" },
  { 1813, ".eseg\n.db 1\n",
    ":00000001FF\r\n",
    ":0100000001FE\r\n:00000001FF\r\n" },
  { 1814, "nop\n.eseg\n.db 1\n",
    ":020000020000FC\r\n:020000000000FE\r\n:00000001FF\r\n",
    ":0100000001FE\r\n:00000001FF\r\n" },
  { 1815, ".dseg\nv: .byte 4\n",
    ":00000001FF\r\n",
    NULL },
  { 1816, ".equ k = 1\n",
    NULL,
    NULL },
  { 2800, ".org 0\n",
    NULL,
    NULL },
  { 2801, ".cseg\n",
    NULL,
    NULL },
  { 2802, ".def a = r16\n",
    NULL,
    NULL },
  { 2803, ".set k = 1\n",
    NULL,
    NULL },
  { 2804, "; only a comment\n",
    NULL,
    NULL },
  { 2805, ".include \"inc/empty.inc\"\n",
    NULL,
    NULL },
  { 2806, ".org 0x100\n",
    NULL,
    NULL },
  { 2807, ".macro m\nnop\n.endm\n",
    NULL,
    NULL },
  { 2808, ".dseg\n.byte 0\n",
    NULL,
    NULL },
  { 2809, ".eseg\n.org 4\n",
    NULL,
    NULL },
  { 2810, ".exit\n",
    NULL,
    NULL },
  { 2820, ".org 0x10\nnop\n.org 0\nnop\n",
    ":020000020000FC\r\n:020020000000DE\r\n:020000000000FE\r\n:00000001FF\r\n",
    NULL },
  { 2821, ".org 0x8000\nnop\n.org 0\nnop\n",
    ":020000021000EC\r\n:020000000000FE\r\n:020000020000FC\r\n:020000000000FE\r\n:00000001FF\r\n",
    NULL },
  { 2822, ".org 0x7ff8\n.dw 1, 2, 3, 4, 5, 6, 7, 8\n",
    ":020000020000FC\r\n:10FFF00001000200030004000500060007000800DD\r\n:00000001FF\r\n",
    NULL },
  { 2823, ".org 0x7ff8\n.dw 1, 2, 3, 4, 5, 6, 7, 8, 9\n",
    ":020000020000FC\r\n:10FFF00001000200030004000500060007000800DD\r\n:020000021000EC\r\n:020000000900F5\r\n:00000001FF\r\n",
    NULL },
  { 2824, ".org 0x7ffc\n.dw 1, 2, 3, 4, 5, 6\n",
    ":020000020000FC\r\n:08FFF8000100020003000400F7\r\n:020000021000EC\r\n:0400000005000600F1\r\n:00000001FF\r\n",
    NULL },
  { 2825, ".org 0x8000\n.dw 1\n.org 0x10000\n.dw 2\n",
    ":020000021000EC\r\n:020000000100FD\r\n:020000022000DC\r\n:020000000200FC\r\n:00000001FF\r\n",
    NULL },
  { 2826, ".org 0x10\n.db 1, 2, 3\n.org 0x20\n.db 4\n",
    ":020000020000FC\r\n:0400200001020300D6\r\n:020040000400BA\r\n:00000001FF\r\n",
    NULL },
  { 2827, ".eseg\n.org 0x10\n.db 1\n.org 0\n.db 2\n",
    ":00000001FF\r\n",
    ":0100100001EE\r\n:0100000002FD\r\n:00000001FF\r\n" },
};

static void testPrograms(void)
{
  for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
    AvrCtx ctx;
    ctxInit(&ctx, (AvrVfs){readFixture, NULL});
    FileId id = sourceMapAdd(&ctx.sources, "t.asm", rows[i].src, strlen(rows[i].src));
    Image flash, eeprom;
    imageInit(&flash, &ctx.arena, ASSEMBLE_DEFAULT_IMAGE_SIZE);
    imageInit(&eeprom, &ctx.arena, ASSEMBLE_DEFAULT_EEPROM_SIZE);
    int result = assembleAll(&ctx, id, &flash, &eeprom);
    if (rows[i].flash == NULL) {
      if (result != ASSEMBLE_EMPTY) {
        fprintf(stderr, "%d: result %d, want ASSEMBLE_EMPTY for:\n%s\n", rows[i].n, result, rows[i].src);
        check_failures_++;
      }
    } else {
      CHECK_INT(result, 1);
      const char *f = hexText(&flash, 1, &ctx.arena);
      const char *e = eeprom.blockCount > 0 ? hexText(&eeprom, 0, &ctx.arena) : NULL;
      if (strcmp(f, rows[i].flash) != 0 || (e == NULL) != (rows[i].eeprom == NULL) || (e != NULL && strcmp(e, rows[i].eeprom) != 0)) {
        fprintf(stderr, "%d: flash\n%s(want\n%s)eeprom\n%s(want\n%s)for:\n%s\n", rows[i].n, f, rows[i].flash,
                e ? e : "(none)\n", rows[i].eeprom ? rows[i].eeprom : "(none)\n", rows[i].src);
        check_failures_++;
      }
    }
    ctxFree(&ctx);
  }
}

static void testImages(void)
{
  Arena arena;
  arenaInit(&arena, 0);
  Image im;
  imageInit(&im, &arena, 1u << 18);
  CHECK_STR(hexText(&im, 1, &arena), ":00000001FF\r\n");
  imageWriteByte(&im, 0x2FFFF, 0xAB);
  imageWriteByte(&im, 0x30000, 0xCD);
  CHECK_STR(hexText(&im, 1, &arena),
            ":020000022000DC\r\n:01FFFF00AB56\r\n:020000023000CC\r\n:01000000CD32\r\n:00000001FF\r\n");
  Image ee;
  imageInit(&ee, &arena, 1u << 16);
  imageWriteByte(&ee, 0x10, 0xFF);
  CHECK_STR(hexText(&ee, 0, &arena), ":01001000FFF0\r\n:00000001FF\r\n");
  arenaFree(&arena);
}

int main(void)
{
  testPrograms();
  testImages();
  return TEST_RESULT();
}