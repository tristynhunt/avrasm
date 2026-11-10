#include "avrasm/assemble.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "avrasm/arena.h"
#include "avrasm/ctx.h"
#include "avrasm/diag.h"
#include "avrasm/expr.h"
#include "avrasm/image.h"
#include "avrasm/instr.h"
#include "avrasm/lexer.h"
#include "avrasm/operand.h"
#include "avrasm/source.h"
#include "avrasm/symtab.h"
#include "avrasm/tokline.h"

// ------- statements ----------------------------------------------------

typedef enum StmtKind
{
  STMT_EMPTY,   // includes only labels
  STMT_INSTR,
  STMT_CSEG,
  STMT_ORG,
  STMT_EQU
} StmtKind;

// one parsed line
typedef struct Stmt
{
  StmtKind kind;
  int bad;       // an error was reported in pass 1
  int hasLabel;
  Token label;            // the IDENT before the colon
  Token head;             // the mnemonic or directive name
  Token name;             // .equ: the symbol being defined
  const InstrDesc *instr; // STMT_INSTR
  Operand ops[INSTR_MAX_OPERANDS];
  int64_t pc;   // the word address of this statement
  struct Stmt *next;
} Stmt;

// everything the passes share
typedef struct Asm
{
  AvrCtx *ctx;
  Image *image;
  Symtab syms;
  Stmt *first;
  Stmt *last;
} Asm;

// ------- helpers -------------------------------------------------------

static char lowerAscii(char c)
{
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int equalsNoCase(const char *text, size_t len, const char *lower)
{
  if (strlen(lower) != len) return 0;
  for (size_t i = 0; i < len; i++) {
    if (lowerAscii(text[i]) != lower[i]) return 0;
  }
  return 1;
}

static const char *tokenText(const Asm *a, const Token *t, size_t *len)
{
  const SourceFile *f = sourceMapGet(&a->ctx->sources, t->span.file);
  *len = t->span.end - t->span.start;
  return f->text + t->span.start;
}

static ExprEnv makeEnv(Asm *a, int64_t pc)
{
  ExprEnv env = { &a->ctx->sources, &a->ctx->diags, symtabLookup, &a->syms, 0, pc, NULL };
  return env;
}

// ------- parsing -------------------------------------------------------

static void parseDirective(Asm *a, const TokenLine *line, uint32_t from, Stmt *s)
{
  size_t len;
  const char *text = tokenText(a, &s->head, &len);
  DiagList *diags = &a->ctx->diags;
  Arena *arena = &a->ctx->arena;

  if (equalsNoCase(text, len, ".cseg")) {
    s->kind = STMT_CSEG;
    if (!expectOperands(line, from, arena, diags, ".cseg", 0, NULL)) s->bad = 1;
  } else if (equalsNoCase(text, len, ".org")) {
    s->kind = STMT_ORG;
    if (!expectOperands(line, from, arena, diags, ".org", 1, s->ops)) s->bad = 1;
  } else if (equalsNoCase(text, len, ".equ")) {
    // .equ NAME = expression
    s->kind = STMT_EQU;
    uint32_t last = line->count - 1;
    if (from + 2 >= last || line->tokens[from].kind != TOK_IDENT || line->tokens[from + 1].kind != TOK_EQ) {
      diagEmit(diags, DIAG_ERROR, line->tokens[from < last ? from : last].span, ".equ needs a name, =, and a value");
      s->bad = 1;
      return;
    }
    s->name = line->tokens[from];
    s->ops[0] = makeOperand(line, from + 2, last, arena);
  } else {
    diagEmit(diags, DIAG_ERROR, s->head.span, "directive %.*s is not supported yet", (int)len, text);
    s->bad = 1;
  }
}

static void parseInstruction(Asm *a, const TokenLine *line, uint32_t from, Stmt *s)
{
  size_t len;
  const char *text = tokenText(a, &s->head, &len);
  DiagList *diags = &a->ctx->diags;
  Arena *arena = &a->ctx->arena;
  
  s->kind = STMT_INSTR;
  s->instr = instrFind(text, len);
  if (s->instr == NULL) {
    diagEmit(diags, DIAG_ERROR, s->head.span, "unknown instruction '%.*s'", (int)len, text);
    s->kind = STMT_EMPTY;
    s->bad = 1;
    return;
  }
  if (!instrParseOperands(s->instr, line, from, arena, diags, s->ops)) s->bad = 1;
}

static void parseLine(Asm *a, const TokenLine *line)
{
  Stmt s;
  memset(&s, 0, sizeof s);
  TokenCursor cur;
  cursorInit(&cur, line);

  // label:
  if (cursorPeek(&cur)->kind == TOK_IDENT && cursorPeekAt(&cur, 1)->kind == TOK_COLON) {
    s.hasLabel = 1;
    s.label = *cursorNext(&cur);
    (void)cursorNext(&cur); // the colon
  }

  if (!cursorAtEnd(&cur)) {
    s.head = *cursorNext(&cur);
    if (s.head.kind != TOK_IDENT) {
      // TOK_ERROR already reported, otherwise new error
      if (s.head.kind != TOK_ERROR) {
        diagEmit(&a->ctx->diags, DIAG_ERROR, s.head.span, "expected an instruction or a directive");
      }
      s.bad = 1;
    } else {
      size_t len;
      const char *text = tokenText(a, &s.head, &len);
      if (text[0] == '.') parseDirective(a, line, cur.pos, &s);
      else parseInstruction(a, line, cur.pos, &s);
    }
  }

  if (!s.hasLabel && s.kind == STMT_EMPTY && !s.bad) return;

  Stmt *keep = arenaAlloc(&a->ctx->arena, sizeof *keep, _Alignof(Stmt));
  *keep = s;
  if (a->last == NULL) a->first = keep;
  else a->last->next = keep;
  a->last = keep;
}

// ------- pass 1: address & symbols -------------------------------------

static void defineSymbol(Asm *a, const Token *name, SymKind kind, int64_t value)
{
  size_t len;
  const char *text = tokenText(a, name, &len);
  DiagList *diags = &a->ctx->diags;

  const Symbol *existing = NULL;
  if (symtabAdd(&a->syms, text, len, kind, value, name->span, &existing) != NULL) return;
  diagEmit(diags, DIAG_ERROR, name->span, "symbol '%.*s' is already defined", (int)len, text);
  diagEmit(diags, DIAG_INFO, existing->defined, "the first definition is here");
}

static void pass1(Asm *a)
{
  int64_t pc = 0;
  int64_t limit = a->image->size / 2;
  for (Stmt *s = a->first; s != NULL; s = s->next) {
    s->pc = pc;
    if (s->hasLabel) defineSymbol(a, &s->label, SYM_LABEL, pc);
    if (s->bad) {
      if (s->kind == STMT_INSTR) pc += s->instr->words;
      continue;
    }

    ExprEnv env = makeEnv(a, pc);
    ExprValue v;
    switch (s->kind) {
      case STMT_EMPTY:
      case STMT_CSEG:
        break;
      case STMT_INSTR:
        pc += s->instr->words;
        break;
      case STMT_ORG:
        if (!evalOperand(&s->ops[0], &env, &v)) break;
        if (v.value < 0 || v.value > limit) {
          diagEmit(&a->ctx->diags, DIAG_ERROR, s->ops[0].span, 
            ".org address %lld is outside the program memory (0 to %lld)",
            (long long)v.value, (long long)limit);
          break;
        }
        pc = v.value;
        break;
      case STMT_EQU:
        if (evalOperand(&s->ops[0], &env, &v)) defineSymbol(a, &s->name, SYM_EQU, v.value);
        break;
    }
  }
}

// ------- pass 2: encoding ----------------------------------------------

static void pass2(Asm *a)
{
  for (Stmt *s = a->first; s != NULL; s = s->next) {
    if (s->bad || s->kind != STMT_INSTR) continue;

    ExprEnv env = makeEnv(a, s->pc);
    uint16_t words[2];
    int n = instrEncode(s->instr, s->ops, s->pc, &env, words);
    for (int i = 0; i < n; i++) {
      uint64_t byteAddr = (uint64_t)(s->pc + i) * 2;
      ImageStatus st = byteAddr > 0xFFFFFFFFu ? IMAGE_RANGE : imageWriteWord(a->image, (uint32_t)byteAddr, words[i]);
      if (st == IMAGE_RANGE) {
        diagEmit(&a->ctx->diags, DIAG_ERROR, s->head.span, 
          "address 0x%llX is outside the program memory", (unsigned long long)(s->pc + i));
      } else if (st == IMAGE_OVERLAP) {
        diagEmit(&a->ctx->diags, DIAG_ERROR, s->head.span, 
          "this overlaps code already placed at word address 0x%llX", (unsigned long long)(s->pc + i));
      }
    }
  }
}

// ------- main driver ---------------------------------------------------

int assemble(AvrCtx *ctx, FileId file, Image *image)
{
  Asm a;
  memset(&a, 0, sizeof a);
  a.ctx = ctx;
  a.image = image;
  symtabInit(&a.syms, &ctx->arena, 0);

  Lexer lx;
  lexerInit(&lx, &a.ctx->sources, file, &a.ctx->diags);
  while (1) {
    TokenLine line = lexLine(&lx, &a.ctx->arena);
    parseLine(&a, &line);
    if (line.tokens[line.count - 1].kind == TOK_EOF) break;
  }

  pass1(&a);
  pass2(&a);
  return !diagListHasErrors(&a.ctx->diags);
}