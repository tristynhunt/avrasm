#ifndef AVRASM_IMAGE_H
#define AVRASM_IMAGE_H

#include <stdint.h>

#include "avrasm/arena.h"

// a run of bytes that were written one after the other, [start, end). avrasm2 writes its hex records
// block by block in the order the program wrote them and not sorted by address
typedef struct ImageBlock
{
  uint32_t start;
  uint32_t end;
} ImageBlock;

// a memory image. 'size' bytes big, each either written or not. bytes which
// are not written are not part of the output at all.
typedef struct Image
{
  uint8_t *bytes;
  uint8_t *used;  // used[i] is 1, once byte i has been written
  uint32_t size;
  Arena *arena;   // where the block list grows
  ImageBlock *blocks;   // in the order they were written, a write right after the last block extends it
  uint32_t blockCount;
  uint32_t blockCap;
} Image;

typedef enum ImageStatus
{
  IMAGE_OK = 0,
  IMAGE_OVERLAP,  // the byte was already written, new replaces old
  IMAGE_RANGE     // the address is outside the image and nothing was written
} ImageStatus;

// an image of 'size' bytes with nothing written. 
void imageInit(Image *im, Arena *arena, uint32_t size);

ImageStatus imageWriteByte(Image *im, uint32_t addr, uint8_t value);

// writes a 16-bit word at a byte address, low byte is first (little-endian)
// and returns the worse of the two bytes statuses.
ImageStatus imageWriteWord(Image *im, uint32_t byteAddr, uint16_t word);

// the same text form which ihex2img.py prints for avrasm2.exe's hex files.
// it contains one line per 16 bytes, "ADDRESS: HH HH HH ...", and a
// new line at the exact address after every gap.
char *imageToText(const Image *im, Arena *arena);

#endif