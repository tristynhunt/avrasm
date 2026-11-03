#include "avrasm/source.h"
#include "check.h"

#include <stdio.h>
#include <string.h>

static const SourceFile *addText(SourceMap *m, const char *text)
{
  FileId id = sourceMapAdd(m, "t.asm", text, strlen(text));
  return sourceMapGet(m, id);
}

int main(void)
{
  Arena arena;
  arenaInit(&arena, 256);
  SourceMap map;
  sourceMapInit(&map, &arena);

  char path[] = "a.asm";
  char buf[] = "ldi r16, 1\nnop\n";
  FileId a = sourceMapAdd(&map, path, buf, strlen(buf));
  CHECK_INT(a, 0);
  path[0] = 'X';
  buf[0] = 'X';
  const SourceFile *fa = sourceMapGet(&map, a);
  REQUIRE(fa != NULL);
  CHECK_STR(fa->path, "a.asm");
  CHECK_STR(fa->text, "ldi r16, 1\nnop\n");
  CHECK_INT(fa->len, 15);
  CHECK_INT(fa->lineCount, 3);
  CHECK_INT(fa->lineStarts[0], 0);
  CHECK_INT(fa->lineStarts[1], 11);
  CHECK_INT(fa->lineStarts[2], 15);

  CHECK(sourceMapGet(&map, FILE_NONE) == NULL);
  CHECK(sourceMapGet(&map, 1) == NULL);

  const SourceFile *f = addText(&map, "a\nb");
  REQUIRE(f != NULL);
  CHECK_INT(f->lineCount, 2);
  CHECK_INT(f->lineStarts[1], 2);

  f = addText(&map, "");
  REQUIRE(f != NULL);
  CHECK_INT(f->len, 0);
  CHECK_INT(f->lineCount, 1);
  CHECK_INT(f->lineStarts[0], 0);

  f = addText(&map, "\n\n");
  REQUIRE(f != NULL);
  CHECK_INT(f->lineCount, 3);
  CHECK_INT(f->lineStarts[1], 1);
  CHECK_INT(f->lineStarts[2], 2);

  f = addText(&map, "a\r\nb\rc\nd");
  REQUIRE(f != NULL);
  CHECK_INT(f->lineCount, 4);
  CHECK_INT(f->lineStarts[1], 3);
  CHECK_INT(f->lineStarts[2], 5);
  CHECK_INT(f->lineStarts[3], 7);

  FileId nulId = sourceMapAdd(&map, "nul.asm", "ab\0cd\nef", 8);
  f = sourceMapGet(&map, nulId);
  REQUIRE(f != NULL);
  CHECK_INT(f->len, 8);
  CHECK(f->text[3] == 'c' && f->text[7] == 'f' && f->text[8] == '\0');
  CHECK_INT(f->lineCount, 2);
  CHECK_INT(f->lineStarts[1], 6);

  FileId first = map.count;
  for (int i = 0; i < 100; i++) {
    char name[32];
    snprintf(name, sizeof name, "f%d.asm", i);
    FileId id = sourceMapAdd(&map, name, "nop\n", 4);
    CHECK_INT(id, first + i);
    REQUIRE(id != FILE_NONE);
  }
  for (int i = 0; i < 100; i++) {
    char name[32];
    snprintf(name, sizeof name, "f%d.asm", i);
    const SourceFile *g = sourceMapGet(&map, first + i);
    REQUIRE(g != NULL);
    CHECK_STR(g->path, name);
  }
  int allThere = 1;
  for (FileId id = 0; id < map.count; id++) {
    if (sourceMapGet(&map, id) == NULL) allThere = 0;
  }
  CHECK(allThere);
  CHECK(sourceMapGet(&map, a) == fa);
  CHECK_STR(fa->text, "ldi r16, 1\nnop\n");
  const SourceFile *again = sourceMapGet(&map, nulId);
  REQUIRE(again != NULL);
  CHECK_INT(again->len, 8);

  arenaFree(&arena);
  return TEST_RESULT();
}