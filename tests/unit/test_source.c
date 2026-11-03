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

  LineCol lc = sourceLineCol(fa, 0);
  CHECK_INT(lc.line, 0);
  CHECK_INT(lc.col, 0);
  lc = sourceLineCol(fa, 4);
  CHECK_INT(lc.line, 0);
  CHECK_INT(lc.col, 4);
  lc = sourceLineCol(fa, 10);
  CHECK_INT(lc.line, 0);
  CHECK_INT(lc.col, 10);
  lc = sourceLineCol(fa, 11);
  CHECK_INT(lc.line, 1);
  CHECK_INT(lc.col, 0);
  lc = sourceLineCol(fa, 13);
  CHECK_INT(lc.line, 1);
  CHECK_INT(lc.col, 2);
  lc = sourceLineCol(fa, 15);
  CHECK_INT(lc.line, 2);
  CHECK_INT(lc.col, 0);
  lc = sourceLineCol(fa, 9999);
  CHECK_INT(lc.line, 2);
  CHECK_INT(lc.col, 0);

  const SourceFile *mixed = addText(&map, "a\r\nb\rc\nd");
  REQUIRE(mixed != NULL);
  lc = sourceLineCol(mixed, 1);
  CHECK_INT(lc.line, 0);
  CHECK_INT(lc.col, 1);
  lc = sourceLineCol(mixed, 3);
  CHECK_INT(lc.line, 1);
  CHECK_INT(lc.col, 0);
  lc = sourceLineCol(mixed, 5);
  CHECK_INT(lc.line, 2);
  CHECK_INT(lc.col, 0);
  lc = sourceLineCol(mixed, 8);
  CHECK_INT(lc.line, 3);
  CHECK_INT(lc.col, 1);

  const SourceFile *empty = addText(&map, "");
  REQUIRE(empty != NULL);
  lc = sourceLineCol(empty, 0);
  CHECK_INT(lc.line, 0);
  CHECK_INT(lc.col, 0);

  char big[2001];
  for (int i = 0; i < 1000; i++) {
    big[2 * i] = 'x';
    big[2 * i + 1] = '\n';
  }
  big[2000] = '\0';
  const SourceFile *lines = addText(&map, big);
  REQUIRE(lines != NULL);
  CHECK_INT(lines->lineCount, 1001);
  int searchOk = 1;
  for (uint32_t off = 0; off <= 2000; off++) {
    lc = sourceLineCol(lines, off);
    if (lc.line != off / 2 || lc.col != off % 2) searchOk = 0;
  }
  CHECK(searchOk);

  arenaFree(&arena);
  return TEST_RESULT();
}