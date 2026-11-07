#include "avrasm/image.h"
#include "avrasm/arena.h"

#include <stdint.h>
#include <stdio.h>


void imageInit(Image *im, Arena *arena, uint32_t size)
{
  im->size = size;
  im->bytes = arenaAlloc(arena, size > 0 ? size : 1, 1);
  im->used  = arenaAlloc(arena, size > 0 ? size : 1, 1);
}

ImageStatus imageWriteByte(Image *im, uint32_t addr, uint8_t value)
{
  if (addr >= im->size) return IMAGE_RANGE;
  ImageStatus status = im->used[addr] ? IMAGE_OVERLAP : IMAGE_OK;
  im->bytes[addr] = value;
  im->used[addr] = 1;
  return status;
}

ImageStatus imageWriteWord(Image *im, uint32_t byteAddr, uint16_t word)
{
  if (byteAddr == 0xFFFFFFFFu) return IMAGE_RANGE; // byteAddr + 1 would wrap around to 0
  ImageStatus lo = imageWriteByte(im, byteAddr, (uint8_t)(word & 0xFF));
  ImageStatus hi = imageWriteByte(im, byteAddr + 1, (uint8_t)(word >> 8));
  return lo > hi ? lo : hi;
}

char *imageToText(const Image *im, Arena *arena)
{
  // every used byte costs at most 3 characters (" HH") and every line at most 9
  // ("AAAAAA:" and the newline) plus one more per byte at worst so this is plenty
  uint32_t used = 0;
  for (uint32_t i = 0; i < im->size; i++) used += im->used[i];
  char *text = arenaAlloc(arena, used * 13 + 1, 1);
  char *p = text;

  uint32_t i = 0;
  while (i < im->size) {
    if (!im->used[i]) {
      i++;
      continue;
    }
    uint32_t runStart = i;
    uint32_t runEnd = i;
    while (runEnd < im->size && im->used[runEnd]) runEnd++;
    for (uint32_t row = runStart; row < runEnd; row += 16) {
      p += sprintf(p, "%06x:", (unsigned)row);
      for (uint32_t k = row; k < row + 16 && k < runEnd; k++)
        p += sprintf(p, " %02x", (unsigned)im->bytes[k]);
      *p++ = '\n';
    }
    i = runEnd;
  }
  *p = '\0';
  return text;
}