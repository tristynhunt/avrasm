#include "avrasm/report.h"
#include "check.h"

#include <stdio.h>
#include <string.h>

static const char *captured(const SourceMap *map, const DiagList *diags)
{
  static char buf[1024];
  FILE *f = tmpfile();
  if (f == NULL) return "(tmpfile failed)";
  reportDiags(f, map, diags);
  fflush(f);
  rewind(f);
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = '\0';
  fclose(f);
  return buf;
}

int main(void)
{
  Arena arena;
  arenaInit(&arena, 0);
  SourceMap map;
  sourceMapInit(&map, &arena);
  DiagList diags;
  diagListInit(&diags, &arena);

  const char *text = "a\nbc de\n\tx\n";
  FileId id = sourceMapAdd(&map, "dir/t.asm", text, strlen(text));

  CHECK_STR(captured(&map, &diags), "");

  Span d = { id, 5, 6 };
  diagEmit(&diags, DIAG_ERROR, d, "bad thing %d", 42);
  CHECK_STR(captured(&map, &diags), "dir/t.asm:2:4: error: bad thing 42\n");

  Span start = { id, 0, 1 };
  Span tabx = { id, 9, 10 };
  diagEmit(&diags, DIAG_WARNING, start, "careful");
  diagEmit(&diags, DIAG_INFO, tabx, "see here");
  CHECK_STR(captured(&map, &diags),
            "dir/t.asm:2:4: error: bad thing 42\n"
            "dir/t.asm:1:1: warning: careful\n"
            "dir/t.asm:3:2: note: see here\n");

  Span end = { id, (uint32_t)strlen(text), (uint32_t)strlen(text) };
  Span none = { FILE_NONE, 0, 0 };
  diagEmit(&diags, DIAG_ERROR, end, "at the end");
  diagEmit(&diags, DIAG_ERROR, none, "cannot open 'x.asm'");
  const char *got = captured(&map, &diags);
  CHECK(strstr(got, "dir/t.asm:4:1: error: at the end\n") != NULL);
  CHECK(strstr(got, "\nerror: cannot open 'x.asm'\n") != NULL);

  arenaFree(&arena);
  return TEST_RESULT();
}