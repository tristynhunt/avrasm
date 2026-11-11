#include "avrasm/hex.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "avrasm/arena.h"
#include "avrasm/image.h"

// writes into buf when it is not NULL but still always counts.
// hexText runs twice, once to count and once to write.
typedef struct Out
{
  char *buf;
  size_t len;
} Out;

// one record: type, 16-bit address, and the data bytes, then the checksum and "\r\n".
static void record(Out *o, uint8_t type, uint16_t addr, const uint8_t *data, uint32_t n)
{
  uint8_t sum = (uint8_t)(n + (addr >> 8) + (addr & 0xFF) + type);
  char line[64];
  int k = snprintf(line, sizeof line, ":%02X%04X%02X", (unsigned)n, (unsigned)addr, (unsigned)type);
  for (uint32_t i = 0; i < n; i++) {
    k += snprintf(line + k, sizeof line - (size_t)k, "%02X", (unsigned)data[i]);
    sum = (uint8_t)(sum + data[i]);
  }
  k += snprintf(line + k, sizeof line - (size_t)k, "%02X\r\n", (unsigned)(uint8_t)(0x100 - sum));
  if (o->buf != NULL) memcpy(o->buf + o->len, line, (size_t)k);
  o->len += (size_t)k;
}

static void writeAll(Out *o, const Image *im, int segments)
{
  uint32_t current = 0xFFFFFFFFu;
  for (uint32_t b = 0; b < im->blockCount; b++) {
    uint32_t at = im->blocks[b].start, end = im->blocks[b].end;
    while (at < end) {
      uint32_t seg = at >> 16;
      if (segments && seg != current) {
        uint8_t base[2] = { (uint8_t)((seg << 12) >> 8), (uint8_t)((seg << 12) & 0xFF) };
        record(o, 2, 0, base, 2);
      }
      current = seg;
      uint32_t stop = at + 16;
      if (stop > end) stop = end;
      if (stop > (seg + 1) << 16) stop = (seg + 1) << 16;
      record(o, 0, (uint16_t)(at & 0xFFFF), im->bytes + at, stop - at);
      at = stop;
    }
  }
  record(o, 1, 0, NULL, 0);
}

char *hexText(const Image *im, int segments, Arena *arena)
{
  Out count = { NULL, 0 };
  writeAll(&count, im, segments);
  Out out = { arenaAlloc(arena, count.len + 1, 1), 0 };
  writeAll(&out, im, segments);
  out.buf[out.len] = '\0';
  return out.buf;
}