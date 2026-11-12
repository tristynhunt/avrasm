#include "avrasm/assemble.h"
#include "avrasm/outline.h"
#include "check.h"

#include <string.h>

static const char *program =
  ".dseg\n"
  "buf: .byte 2\n"
  ".cseg\n"
  ".equ A = 1\n"
  ".set S = 1\n"
  ".set S = 2\n"
  ".def t = r16\n"
  ".undef t\n"
  ".def t = r17\n"
  ".macro m\n"
  "inner: nop\n"
  "ldi r16, @1\n"
  ".endm\n"
  "#define D 3\n"
  "lp: m 1, 2\n"
  "lp: nop\n"
  ".dw 1\n"
  "jmp 0\n";

static Span at(FileId file, const char *before, const char *name)
{
  uint32_t start = (uint32_t)(strstr(program, before) - program + strlen(before));
  return (Span){ file, start, start + (uint32_t)strlen(name) };
}

static void checkName(const Outline *o, const char *name, OutlineKind kind, int64_t value, char seg, Span defined)
{
  const OutlineName *n = outlineFind(o, name, strlen(name));
  if (n == NULL) {
    fprintf(stderr, "no name %s in the outline\n", name);
    check_failures_++;
    return;
  }
  CHECK_INT(n->kind, kind);
  CHECK_INT(n->value, value);
  CHECK_INT(n->seg, seg);
  CHECK_INT(n->defined.file, defined.file);
  CHECK_INT(n->defined.start, defined.start);
  CHECK_INT(n->defined.end, defined.end);
}

int main(void)
{
  AvrCtx ctx;
  ctxInit(&ctx, (AvrVfs){ NULL, NULL });
  FileId id = sourceMapAdd(&ctx.sources, "t.asm", program, strlen(program));
  Image flash, eeprom;
  imageInit(&flash, &ctx.arena, ASSEMBLE_DEFAULT_IMAGE_SIZE);
  imageInit(&eeprom, &ctx.arena, ASSEMBLE_DEFAULT_EEPROM_SIZE);
  Outline o;
  outlineInit(&o, &ctx.arena);
  AsmOptions opt = { NULL, 0, NULL, 0, &o };
  CHECK_INT(assembleOpt(&ctx, id, &flash, &eeprom, &opt), 0);
  CHECK_INT(ctx.diags.errorCount, 1);

  checkName(&o, "buf", OUTLINE_LABEL, 0x60, 'd', at(id, ".dseg\n", "buf"));
  checkName(&o, "a", OUTLINE_EQU, 1, 0, at(id, ".equ ", "A"));
  checkName(&o, "S", OUTLINE_SET, 2, 0, at(id, ".set ", "S"));
  checkName(&o, "t", OUTLINE_DEF, 16, 0, at(id, ".def ", "t"));
  checkName(&o, "m", OUTLINE_MACRO, 2, 0, at(id, "r17\n", ".macro"));
  checkName(&o, "D", OUTLINE_DEFINE, 0, 0, at(id, "#define ", "D"));
  checkName(&o, "lp", OUTLINE_LABEL, 0, 'c', at(id, "D 3\n", "lp"));
  CHECK(outlineFind(&o, "inner", 5) == NULL);
  uint32_t lps = 0;
  for (uint32_t i = 0; i < o.nameCount; i++) lps += strcmp(o.names[i].name, "lp") == 0;
  CHECK_INT(lps, 1);

  REQUIRE(o.codeCount == 5);
  CHECK_INT(o.code[0].pc, 0);
  CHECK_INT(o.code[0].words, 1);
  CHECK_INT(o.code[0].head.start, at(id, "inner: ", "nop").start);
  CHECK_INT(o.code[0].callSite.start, at(id, "D 3\nlp: ", "m").start);
  CHECK(o.code[0].scope != 0 && o.code[1].scope == o.code[0].scope);
  CHECK_INT(o.code[1].pc, 1);
  CHECK_INT(o.code[2].pc, 2);
  CHECK_INT(o.code[2].callSite.file, FILE_NONE);
  CHECK_INT(o.code[3].isData, 1);
  CHECK_INT(o.code[3].pc, 3);
  CHECK_INT(o.code[4].pc, 4);
  CHECK_INT(o.code[4].words, 2);
  CHECK_INT(o.code[4].isData, 0);

  ctxFree(&ctx);
  return TEST_RESULT();
}