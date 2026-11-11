#include "avrasm/image.h"
#include "check.h"

#include <string.h>

int main(void)
{
  Arena arena;
  arenaInit(&arena, 0);
  Image im;

  imageInit(&im, &arena, 64);
  CHECK_STR(imageToText(&im, &arena), "");

  CHECK_INT(imageWriteWord(&im, 0, 0xE505), IMAGE_OK);
  CHECK_INT(imageWriteWord(&im, 2, 0xBF0F), IMAGE_OK);
  CHECK_INT(imageWriteByte(&im, 4, 0xFD), IMAGE_OK);
  CHECK_INT(imageWriteByte(&im, 5, 0xCF), IMAGE_OK);
  CHECK_STR(imageToText(&im, &arena), "000000: 05 e5 0f bf fd cf\n");
  CHECK_INT(im.bytes[0], 0x05);
  CHECK_INT(im.bytes[1], 0xE5);

  CHECK_INT(imageWriteByte(&im, 4, 0x11), IMAGE_OVERLAP);
  CHECK_INT(im.bytes[4], 0x11);
  CHECK_INT(imageWriteWord(&im, 4, 0x2222), IMAGE_OVERLAP);
  CHECK_INT(imageWriteWord(&im, 3, 0x3333), IMAGE_OVERLAP);
  CHECK_INT(imageWriteWord(&im, 6, 0x4444), IMAGE_OK);
  CHECK_INT(imageWriteWord(&im, 7, 0x5555), IMAGE_OVERLAP);

  imageInit(&im, &arena, 8);
  CHECK_INT(imageWriteByte(&im, 8, 1), IMAGE_RANGE);
  CHECK_INT(imageWriteByte(&im, 0xFFFFFFFFu, 1), IMAGE_RANGE);
  CHECK_INT(imageWriteWord(&im, 7, 0x1234), IMAGE_RANGE);
  CHECK_INT(im.used[7], 1);
  CHECK_INT(imageWriteWord(&im, 0xFFFFFFFFu, 0x1234), IMAGE_RANGE);
  CHECK_INT(im.used[0], 0);
  CHECK_STR(imageToText(&im, &arena), "000007: 34\n");

  imageInit(&im, &arena, 0x200);
  imageWriteByte(&im, 0x10, 0xAA);
  imageWriteByte(&im, 0x11, 0xBB);
  imageWriteByte(&im, 0x13, 0xCC);
  CHECK_STR(imageToText(&im, &arena), "000010: aa bb\n000013: cc\n");

  imageInit(&im, &arena, 0x200);
  for (uint32_t i = 0; i < 40; i++) imageWriteByte(&im, 0x100 + i, (uint8_t)i);
  CHECK_STR(imageToText(&im, &arena),
            "000100: 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f\n"
            "000110: 10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f\n"
            "000120: 20 21 22 23 24 25 26 27\n");

  imageInit(&im, &arena, 0x200);
  for (uint32_t i = 0; i < 18; i++) imageWriteByte(&im, 0x5 + i, 0xF0);
  CHECK_STR(imageToText(&im, &arena),
            "000005: f0 f0 f0 f0 f0 f0 f0 f0 f0 f0 f0 f0 f0 f0 f0 f0\n"
            "000015: f0 f0\n");

  imageInit(&im, &arena, 0x200);
  for (uint32_t i = 0; i < 16; i++) imageWriteByte(&im, i, 0x01);
  imageWriteByte(&im, 0x20, 0x02);
  CHECK_STR(imageToText(&im, &arena),
            "000000: 01 01 01 01 01 01 01 01 01 01 01 01 01 01 01 01\n"
            "000020: 02\n");

  imageInit(&im, &arena, 0x20000);
  imageWriteByte(&im, 0x1FFFF, 0x9A);
  imageWriteByte(&im, 0x10000, 0x01);
  CHECK_STR(imageToText(&im, &arena), "010000: 01\n01ffff: 9a\n");

  imageInit(&im, &arena, 0);
  CHECK_INT(imageWriteByte(&im, 0, 1), IMAGE_RANGE);
  CHECK_STR(imageToText(&im, &arena), "");

  imageInit(&im, &arena, 4096);
  for (uint32_t i = 0; i < 4096; i++) imageWriteByte(&im, i, 0xFF);
  Arena tight;
  arenaInit(&tight, 16);
  char *t = imageToText(&im, &tight);
  CHECK_INT(strlen(t), 4096 / 16 * (7 + 16 * 3 + 1));
  arenaFree(&tight);

  imageInit(&im, &arena, 200);
  for (uint32_t i = 0; i < 200; i += 2) imageWriteByte(&im, i, 0xAB);
  arenaInit(&tight, 16);
  t = imageToText(&im, &tight);
  CHECK_INT(strlen(t), 100 * 11);
  CHECK(strncmp(t, "000000: ab\n000002: ab\n", 22) == 0);
  arenaFree(&tight);
  arenaFree(&arena);
  
  {
    Arena blockArena;
    arenaInit(&blockArena, 0);
    Image b;
    imageInit(&b, &blockArena, 0x100);
    CHECK_INT(b.blockCount, 0);
    imageWriteWord(&b, 0x20, 0x0000);
    imageWriteWord(&b, 0x22, 0x1111);
    imageWriteWord(&b, 0x00, 0x2222);
    CHECK_INT(b.blockCount, 2);
    CHECK_INT(b.blocks[0].start, 0x20);
    CHECK_INT(b.blocks[0].end, 0x24);
    CHECK_INT(b.blocks[1].start, 0x00);
    CHECK_INT(b.blocks[1].end, 0x02);
    for (uint32_t i = 0; i < 40; i++) imageWriteByte(&b, 0x40 + 2 * i, 0);
    CHECK_INT(b.blockCount, 42);
    CHECK_INT(imageWriteByte(&b, 0x100, 0), IMAGE_RANGE);
    CHECK_INT(b.blockCount, 42);
    arenaFree(&blockArena);
  }

  return TEST_RESULT();
}