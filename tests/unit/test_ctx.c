#include "avrasm/ctx.h"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Entry {
  const char *path;
  const char *bytes;
  size_t len;
} Entry;

typedef struct Fake {
  const Entry *entries;
  int reads;
} Fake;

static char *fakeRead(void *user, const char *path, size_t *len)
{
  Fake *fake = user;
  fake->reads++;
  for (const Entry *e = fake->entries; e->path != NULL; e++) {
    if (strcmp(e->path, path) != 0) continue;
    char *copy = malloc(e->len ? e->len : 1);
    if (copy == NULL) return NULL;
    memcpy(copy, e->bytes, e->len);
    *len = e->len;
    return copy;
  }
  return NULL;
}

static int writeFile(const char *path, const void *bytes, size_t len)
{
  FILE *f = fopen(path, "wb");
  if (f == NULL) return 0;
  size_t put = fwrite(bytes, 1, len, f);
  fclose(f);
  return put == len;
}

int main(void)
{
  Span top = { FILE_NONE, 0, 0 };

  static const Entry entries[] = {
    { "a.asm", "ldi r16, 1\n", 11 },
    { "b.inc", "nop", 3 },
    { "bin.dat", "ab\0cd", 5 },
    { "empty.asm", "", 0 },
    { NULL, NULL, 0 }
  };
  Fake fake = { entries, 0 };
  AvrVfs vfs = { fakeRead, &fake };
  AvrCtx ctx;
  ctxInit(&ctx, vfs);

  FileId a = ctxLoad(&ctx, "a.asm", top);
  CHECK_INT(a, 0);
  CHECK_INT(fake.reads, 1);
  const SourceFile *fa = sourceMapGet(&ctx.sources, a);
  REQUIRE(fa != NULL);
  CHECK_STR(fa->path, "a.asm");
  CHECK_STR(fa->text, "ldi r16, 1\n");
  CHECK_INT(fa->len, 11);
  CHECK_INT(fa->lineCount, 2);
  CHECK_INT(ctx.diags.count, 0);

  FileId b = ctxLoad(&ctx, "b.inc", top);
  CHECK_INT(b, 1);
  const SourceFile *fb = sourceMapGet(&ctx.sources, b);
  REQUIRE(fb != NULL);
  CHECK_STR(fb->text, "nop");

  FileId again = ctxLoad(&ctx, "a.asm", top);
  CHECK_INT(again, 2);
  CHECK_INT(fake.reads, 3);

  FileId bin = ctxLoad(&ctx, "bin.dat", top);
  const SourceFile *fbin = sourceMapGet(&ctx.sources, bin);
  REQUIRE(fbin != NULL);
  CHECK_INT(fbin->len, 5);
  CHECK(fbin->text[2] == '\0' && fbin->text[3] == 'c' && fbin->text[4] == 'd');

  FileId empty = ctxLoad(&ctx, "empty.asm", top);
  CHECK(empty != FILE_NONE);
  const SourceFile *fe = sourceMapGet(&ctx.sources, empty);
  REQUIRE(fe != NULL);
  CHECK_INT(fe->len, 0);
  CHECK_INT(fe->lineCount, 1);
  CHECK_INT(ctx.diags.count, 0);

  uint32_t filesBefore = ctx.sources.count;
  Span directive = { a, 5, 21 };
  FileId missing = ctxLoad(&ctx, "nope.asm", directive);
  CHECK_INT(missing, FILE_NONE);
  CHECK_INT(ctx.sources.count, filesBefore);
  CHECK_INT(ctx.diags.errorCount, 1);
  REQUIRE(ctx.diags.head != NULL);
  CHECK_INT(ctx.diags.head->severity, DIAG_ERROR);
  CHECK_INT(ctx.diags.head->span.file, a);
  CHECK_INT(ctx.diags.head->span.start, 5);
  CHECK_INT(ctx.diags.head->span.end, 21);
  CHECK(strstr(ctx.diags.head->message, "nope.asm") != NULL);

  ctxFree(&ctx);

  AvrVfs none = { NULL, NULL };
  ctxInit(&ctx, none);

  const char bytes[] = "a\r\nb\0c";
  REQUIRE(writeFile("build/tests/ctx_fixture.bin", bytes, 6));
  FileId onDisk = ctxLoad(&ctx, "build/tests/ctx_fixture.bin", top);
  REQUIRE(onDisk != FILE_NONE);
  const SourceFile *fd = sourceMapGet(&ctx.sources, onDisk);
  REQUIRE(fd != NULL);
  CHECK_INT(fd->len, 6);
  CHECK(memcmp(fd->text, bytes, 6) == 0);
  CHECK_INT(fd->lineCount, 2);

  REQUIRE(writeFile("build/tests/ctx_empty.bin", "", 0));
  FileId emptyDisk = ctxLoad(&ctx, "build/tests/ctx_empty.bin", top);
  CHECK(emptyDisk != FILE_NONE);

  enum { BIG = 300000 };
  unsigned char *big = malloc(BIG);
  REQUIRE(big != NULL);
  for (int i = 0; i < BIG; i++) big[i] = (unsigned char)(i * 7);
  REQUIRE(writeFile("build/tests/ctx_big.bin", big, BIG));
  FileId bigId = ctxLoad(&ctx, "build/tests/ctx_big.bin", top);
  REQUIRE(bigId != FILE_NONE);
  const SourceFile *fbig = sourceMapGet(&ctx.sources, bigId);
  REQUIRE(fbig != NULL);
  REQUIRE(fbig->len == BIG);
  CHECK(memcmp(fbig->text, big, BIG) == 0);
  free(big);

  static unsigned char edge[4097];
  memset(edge, 'x', sizeof edge);
  REQUIRE(writeFile("build/tests/ctx_4096.bin", edge, 4096));
  REQUIRE(writeFile("build/tests/ctx_4097.bin", edge, 4097));
  FileId e1 = ctxLoad(&ctx, "build/tests/ctx_4096.bin", top);
  FileId e2 = ctxLoad(&ctx, "build/tests/ctx_4097.bin", top);
  REQUIRE(e1 != FILE_NONE && e2 != FILE_NONE);
  CHECK_INT(sourceMapGet(&ctx.sources, e1)->len, 4096);
  CHECK_INT(sourceMapGet(&ctx.sources, e2)->len, 4097);

  CHECK_INT(ctx.diags.count, 0);

  CHECK_INT(ctxLoad(&ctx, "build/tests/no_such_file.asm", top), FILE_NONE);
  CHECK_INT(ctxLoad(&ctx, "build/tests", top), FILE_NONE);
  CHECK_INT(ctx.diags.errorCount, 2);

  ctxFree(&ctx);

  ctxInit(&ctx, vfs);
  unsigned before = ctx.diags.count;
  CHECK(ctxLoadQuiet(&ctx, "nope.inc") == FILE_NONE);
  CHECK_INT(ctx.diags.count, before);
  FileId quiet = ctxLoadQuiet(&ctx, "b.inc");
  CHECK(quiet != FILE_NONE);
  CHECK_STR(sourceMapGet(&ctx.sources, quiet)->text, "nop");
  ctxFree(&ctx);

  ctxInit(&ctx, vfs);
  CHECK(ctxLoad(&ctx, "b.inc", top) == 0);
  ctxFree(&ctx);

  return TEST_RESULT();
}