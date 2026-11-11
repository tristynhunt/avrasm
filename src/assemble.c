#include "avrasm/assemble.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "avrasm/arena.h"
#include "avrasm/ctx.h"
#include "avrasm/diag.h"
#include "avrasm/expr.h"
#include "avrasm/image.h"
#include "avrasm/instr.h"
#include "avrasm/lexer.h"
#include "avrasm/operand.h"
#include "avrasm/reader.h"
#include "avrasm/source.h"
#include "avrasm/symtab.h"
#include "avrasm/tokline.h"

// ------- statements ----------------------------------------------------

// where the statement puts things. the program counter of .cseg counts words,
// the ones in .dseg and .eseg counts bytes. each segment keepts its own counter
// while another segment is active.
typedef enum Seg
{
  SEG_CODE,
  SEG_DATA,
  SEG_EEPROM,
  SEG_COUNT
} Seg;

typedef enum StmtKind
{
  STMT_EMPTY,   // nothing or only labels
  STMT_INSTR,
  STMT_SEG,     // .cseg .dseg .eseg with 'target' as the segment to switch to
  STMT_ORG,
  STMT_EQU,
  STMT_SET,
  STMT_DB,
  STMT_DW,
  STMT_BYTE,
  STMT_DEF,
  STMT_UNDEF,
  STMT_DEVICE // .device NAME
} StmtKind;

// one parsed line. does not look at values (pass 1 and 2 do).
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
  Operand *items;         // .db and .dw values
  uint32_t itemCount;
  uint32_t dataBytes;     // .db and .dw and how many bytes the values make before the padding
  int regNum;             // .def and the register
  Seg target;             // STMT_SEG
  Seg seg;                // set by pass 1
  int64_t pc;   // the word address of this statement
  uint32_t scope;         // 0, or the macro expansion it is in.
  Span callSite;          // the macro call it came out of or FILE_NONE if not a macro
  struct Stmt *next;
} Stmt;

// a name given to a register by .def,
// the name points into the source text.
typedef struct Alias
{
  const char *name;
  size_t len;
  int reg;
  struct Alias *next;
} Alias;

// everything the passes share
typedef struct Asm
{
  AvrCtx *ctx;
  Image *flash;
  Image *eeprom;
  Symtab syms;
  Alias *aliases;
  Stmt *first;
  Stmt *last;
  Reader *reader;
  Seg cur;                 // pass 1: the segment in force
  int64_t pos[SEG_COUNT];  // pass 1: the counter of each segment
  uint32_t scope;          // the scope of the statement being handled: symbol lookups use it
  const Stmt *encoding;    // pass 2: the statement being encoded (NULL in pass 1)
  Token device;            // .device NAME
  int hasDevice;
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

static int sameName(const char *a, size_t aLen, const char *b, size_t bLen)
{
  if (aLen != bLen) return 0;
  for (size_t i = 0; i < aLen; i++) {
    if (lowerAscii(a[i]) != lowerAscii(b[i])) return 0;
  }
  return 1;
}

// checks that 'name' is not a register, which no symbols should have
static int isReservedName(const char *text, size_t len)
{
  int reg;
  if (parseRegisterName(text, len, &reg)) return 1;
  if (len != 1) return 0;
  char c = lowerAscii(text[0]);
  return c == 'x' || c == 'y' || c == 'z';
}

// checks any other names which are reserved. this includes:
// every instruction (including the XMEGA only ones), every built-in function, and pc
static const char *keywordKind(const char *text, size_t len)
{
  static const char *const xmegaOnly[] = { "des", "lac", "las", "lat", "xch" };
  if (instrFind(text, len) != NULL) return "an instruction";
  for (size_t i = 0; i < sizeof xmegaOnly / sizeof xmegaOnly[0]; i++) {
    if (equalsNoCase(text, len, xmegaOnly[i])) return "an instruction";
  }
  if (equalsNoCase(text, len, "pc")) return "the program counter";
  if (exprIsBuiltinName(text, len)) return "a built-in function";
  return NULL;
}

static const char *tokenText(const Asm *a, const Token *t, size_t *len)
{
  const SourceFile *f = sourceMapGet(&a->ctx->sources, t->span.file);
  *len = t->span.end - t->span.start;
  return f->text + t->span.start;
}

#define SCOPED_MAX 256

static size_t scopedName(char *buf, size_t cap, const char *name, size_t len, uint32_t scope)
{
  if (len + 12 > cap) return 0;
  memcpy(buf, name, len);
  buf[len] = '\x01';
  return len + 1 + (size_t)snprintf(buf + len + 1, 11, "%u", scope);
}

// checks if a .set of this name comes before the statement being enconded.
static int setComesFirst(const Asm *a, const char *name, size_t len)
{
  for (const Stmt *s = a->first; s != NULL && s != a->encoding; s = s->next) {
    if (s->kind != STMT_SET || s->bad) continue;
    size_t n;
    const char *text = tokenText(a, &s->name, &n);
    if (sameName(text, n, name, len)) return 1;
  }
  return 0;
}

// ExprEnv.lookup and ExprEnv.lookupRegister. 'user' is the Asm.
static int lookupSymbol(void *user, const char *name, size_t len, int64_t *value)
{
  Asm *a = user;
  if (a->scope != 0) {
    // the labels of this macro expansion first
    char buf[SCOPED_MAX];
    size_t n = scopedName(buf, sizeof buf, name, len, a->scope);
    if (n != 0 && symtabLookup(&a->syms, buf, n, value)) return 1;
  }
  const Symbol *sym = symtabFind(&a->syms, name, len);
  if (sym == NULL) return 0;
  if (sym->kind == SYM_SET && a->encoding != NULL && !setComesFirst(a, name, len)) {
    diagEmit(&a->ctx->diags, DIAG_ERROR, a->encoding->head.span, "'%.*s' is used before its first .set", (int)len, name);
  }
  *value = sym->value;
  return 1;
}

static int lookupRegister(void *user, const char *name, size_t len, int *reg)
{
  for (const Alias *al = ((Asm *)user)->aliases; al != NULL; al = al->next) {
    if (sameName(al->name, al->len, name, len)) {
      *reg = al->reg;
      return 1;
    }
  }
  return 0;
}

static ExprEnv makeEnv(Asm *a, int64_t pc)
{
  ExprEnv env = { &a->ctx->sources, &a->ctx->diags, lookupSymbol, a, 0, pc, lookupRegister };
  return env;
}

// ------- parsing -------------------------------------------------------

// '.equ NAME = expr' and '.set' and '.def'
// fills s->name and s->ops[0]. returns 0 after one error.
static int parseNamed(Asm *a, const TokenLine *line, uint32_t from, Stmt *s, const char *directive)
{
  uint32_t last = line->count - 1;
  if (from + 2 >= last || line->tokens[from].kind != TOK_IDENT || line->tokens[from + 1].kind != TOK_EQ) {
    diagEmit(&a->ctx->diags, DIAG_ERROR, line->tokens[from < last ? from : last].span, 
      "%s needs a name, =, and a value", directive);
    s->bad = 1;
    return 0;
  }
  s->name = line->tokens[from];
  s->ops[0] = makeOperand(line, from + 2, last, &a->ctx->arena);
  return 1;
}

// is the operand one string
static int isString(const Operand *op)
{
  return op->tokens.count == 2 && op->tokens.tokens[0].kind == TOK_STRING;
}

// is the operand one identifier and nothing else
static int isNameOnly(const Operand *op)
{
  return op->tokens.count == 2 && op->tokens.tokens[0].kind == TOK_IDENT;
}

// .db and .dw: a comma-separated list of values. 'unit' is the size of one value in bytes.
// fills s->itemCount and s->dataBytes, a string is its length in bytes.
static void parseData(Asm *a, const TokenLine *line, uint32_t from, Stmt *s, const char *directive, uint32_t unit)
{
  Arena *arena = &a->ctx->arena;
  DiagList *diags = &a->ctx->diags;
  s->items = arenaAlloc(arena, line->count * sizeof *s->items, _Alignof(Operand));
  int n = splitOperands(line, from, arena, diags, s->items, line->count);
  if (n < 0) {
    s->bad = 1;
    return;
  }
  if (n == 0) {
    diagEmit(diags, DIAG_ERROR, s->head.span, "%s needs at least one value", directive);
    s->bad = 1;
    return;
  }
  s->itemCount = (uint32_t)n;
  for (uint32_t i = 0; i < s->itemCount; i++) {
    const Operand *item = &s->items[i];
    if (!isString(item)) {
      s->dataBytes += unit;
    } else if (unit == 1) {
      uint32_t chars = item->span.end - item->span.start - 2;
      if (chars == 0) {
        diagEmit(diags, DIAG_ERROR, item->span, "an empty string has no bytes");
        s->bad = 1;
        return;
      }
      size_t n;
      const char *str = tokenText(a, &item->tokens.tokens[0], &n);
      if (memchr(str, '\\', n) != NULL) {
        diagEmit(diags, DIAG_WARNING, item->span, 
          "a backslash in a string is just a backslash: \"\\n\" is the two bytes '\\' and 'n' (for a line feed, write 10)");
      }
      s->dataBytes += chars;
    } else {
      diagEmit(diags, DIAG_ERROR, item->span, "a string can only be used with .db");
      s->bad = 1;
      return;
    }
  }
}

static void parseDirective(Asm *a, const TokenLine *line, uint32_t from, Stmt *s)
{
  size_t len;
  const char *text = tokenText(a, &s->head, &len);
  DiagList *diags = &a->ctx->diags;
  Arena *arena = &a->ctx->arena;

  if (equalsNoCase(text, len, ".cseg") || equalsNoCase(text, len, ".dseg") || equalsNoCase(text, len, ".eseg")) {
    s->kind = STMT_SEG;
    s->target = lowerAscii(text[1]) == 'c' ? SEG_CODE : lowerAscii(text[1]) == 'd' ? SEG_DATA : SEG_EEPROM;
    if (!expectOperands(line, from, arena, diags, ".cseg", 0, NULL)) s->bad = 1;
  } else if (equalsNoCase(text, len, ".org")) {
    s->kind = STMT_ORG;
    if (!expectOperands(line, from, arena, diags, ".org", 1, s->ops)) s->bad = 1;
  } else if (equalsNoCase(text, len, ".equ")) {
    s->kind = STMT_EQU;
    parseNamed(a, line, from, s, ".equ");
  } else if (equalsNoCase(text, len, ".set")) {
    s->kind = STMT_SET;
    parseNamed(a, line, from, s, ".set");
  } else if (equalsNoCase(text, len, ".db")) {
    s->kind = STMT_DB;
    parseData(a, line, from, s, ".db", 1);
  } else if (equalsNoCase(text, len, ".dw")) {
    s->kind = STMT_DW;
    parseData(a, line, from, s, ".dw", 2);
  } else if (equalsNoCase(text, len, ".byte")) {
    s->kind = STMT_BYTE;
    if (!expectOperands(line, from, arena, diags, ".byte", 1, s->ops)) s->bad = 1;
  } else if (equalsNoCase(text, len, ".def")) {
    s->kind = STMT_DEF;
    if (!parseNamed(a, line, from, s, ".def")) return;
    if (!operandRegister(&s->ops[0], &a->ctx->sources, &s->regNum)) {
      diagEmit(diags, DIAG_ERROR, s->ops[0].span, ".def needs a register, r0 to r31");
      s->bad = 1;
    }
  } else if (equalsNoCase(text, len, ".undef")) {
    s->kind = STMT_UNDEF;
    if (!expectOperands(line, from, arena, diags, ".undef", 1, s->ops)) {
      s->bad = 1;
    } else if (!isNameOnly(&s->ops[0])) {
      diagEmit(diags, DIAG_ERROR, s->ops[0].span, ".undef needs the name of a register alias");
      s->bad = 1;
    } else {
      s->name = s->ops[0].tokens.tokens[0];
    }
  } else if (equalsNoCase(text, len, ".device")) {
    s->kind = STMT_DEVICE;
    uint32_t last = line->count - 1;
    const Token *nameTok = &line->tokens[from < last ? from : last];
    if (nameTok->kind != TOK_IDENT || from + 1 != last) {
      if (nameTok->kind != TOK_ERROR) diagEmit(diags, DIAG_ERROR, nameTok->span, ".device needs a device name");
      s->bad = 1;
    } else {
      s->name = *nameTok;
    }
  } else if (equalsNoCase(text, len, ".list") || equalsNoCase(text, len, ".nolist") || equalsNoCase(text, len, ".listmac") || 
      equalsNoCase(text, len, ".overlap") || equalsNoCase(text, len, ".nooverlap")) {
    if (!expectOperands(line, from, arena, diags, "this directive", 0, NULL)) s->bad = 1;
  } else {
    diagEmit(diags, DIAG_ERROR, s->head.span, "directive %.*s is not supported yet", (int)len, text);
    s->bad = 1;
  }
}

static void parseInstruction(Asm *a, const TokenLine *line, uint32_t from, Stmt *s)
{
  size_t len;
  const char *text = tokenText(a, &s->head, &len);
  s->kind = STMT_INSTR;
  s->instr = instrFind(text, len);
  if (s->instr == NULL) {
    diagEmit(&a->ctx->diags, DIAG_ERROR, s->head.span, "unknown instruction '%.*s'", (int)len, text);
    s->kind = STMT_EMPTY;
    s->bad = 1;
    return;
  }
  if (!instrParseOperands(s->instr, line, from, &a->ctx->arena, &a->ctx->diags, s->ops)) s->bad = 1;
}

// turns one line into a statement and appends it to the list.
// returns the statement, or NULL for a blank line
static Stmt *parseLine(Asm *a, const TokenLine *line, uint32_t scope, Span callSite)
{
  Stmt s;
  memset(&s, 0, sizeof s);
  s.scope = scope;
  s.callSite = callSite;
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

  if (!s.hasLabel && s.kind == STMT_EMPTY && !s.bad) return NULL;

  Stmt *keep = arenaAlloc(&a->ctx->arena, sizeof *keep, _Alignof(Stmt));
  *keep = s;
  if (a->last == NULL) a->first = keep;
  else a->last->next = keep;
  a->last = keep;
  return keep;
}

// ------- pass 1: address & symbols -------------------------------------

// checks if a name can become a label, an .equ or a .set symbol
static int nameIsFree(Asm *a, const Token *name)
{
  DiagList *diags = &a->ctx->diags;
  size_t len;
  const char *text = tokenText(a, name, &len);
  int reg;
  const char *keyword = keywordKind(text, len);
  if (isReservedName(text, len)) {
    diagEmit(diags, DIAG_ERROR, name->span, "'%.*s' is a register name and cannot be a symbol", (int)len, text);
    return 0;
  }
  if (keyword != NULL) {
    diagEmit(diags, DIAG_ERROR, name->span, "'%.*s' is %s and cannot be a symbol", (int)len, text, keyword);
    return 0;
  }
  if (lookupRegister(a, text, len, &reg)) {
    diagEmit(diags, DIAG_ERROR, name->span, "'%.*s' is a register alias (.def) and cannot be a symbol", (int)len, text);
    return 0;
  }
  if (readerHasMacro(a->reader, text, len)) {
    diagEmit(diags, DIAG_ERROR, name->span, "'%.*s' is the name of a macro and cannot be a symbol", (int)len, text);
    return 0;
  }
  return 1;
}

static void defineSymbol(Asm *a, const Token *name, SymKind kind, int64_t value)
{
  if (!nameIsFree(a, name)) return;
  size_t len;
  const char *text = tokenText(a, name, &len);
  DiagList *diags = &a->ctx->diags;

  const Symbol *existing = NULL;
  if (symtabAdd(&a->syms, text, len, kind, value, name->span, &existing) != NULL) return;
  diagEmit(diags, DIAG_ERROR, name->span, "symbol '%.*s' is already defined", (int)len, text);
  diagEmit(diags, DIAG_INFO, existing->defined, "the first definition is here");
}

// a label. inside a macro expansion it is private to that expansion (every call can have its own 'loop:'),
// so it is stored under a scoped name. that is also why it may share its name with a global label.
// inside the expansion the private one is found first (lookupSymbol) and outside there is only the global one.
static void defineLabel(Asm *a, const Token *name, int64_t value)
{
  if (a->scope == 0) {
    defineSymbol(a, name, SYM_LABEL, value);
    return;
  }
  if (!nameIsFree(a, name)) return;
  size_t len;
  const char *text = tokenText(a, name, &len);
  char buf[SCOPED_MAX];
  size_t n = scopedName(buf, sizeof buf, text, len, a->scope);
  if (n == 0) {
    diagEmit(&a->ctx->diags, DIAG_ERROR, name->span, "this label name is too long to be used inside a macro");
    return;
  }
  const Symbol *existing = NULL;
  if (symtabAdd(&a->syms, buf, n, SYM_LABEL, value, name->span, &existing) == NULL) {
    diagEmit(&a->ctx->diags, DIAG_ERROR, name->span, 
      "label '%.*s' is already defined in this macro call", (int)len, text);
    diagEmit(&a->ctx->diags, DIAG_INFO, existing->defined, "the first definition is here");
  }
}

static int hookEval(void *user, TokenCursor *cur, int64_t *value)
{
  Asm *a = user;
  ExprEnv env = makeEnv(a, a->pos[a->cur]);
  ExprValue v;
  if (!exprEval(cur, &env, &v)) return 0;
  *value = v.value;
  return 1;
}

// defined so far. a label, an .equ or .set symbol, or a .def alias that earlier lines made
static int hookIsDefined(void *user, const char *name, size_t len)
{
  Asm *a = user;
  int reg;
  return symtabFind(&a->syms, name, len) != NULL || lookupRegister(a, name, len, &reg);
}

// checks if 'name' can be the name of a macro.
static int hookNameOk(void *user, const Token *name)
{
  Asm *a = user;
  size_t len;
  const char *text = tokenText(a, name, &len);
  // a keyword is never a register name
  const char *what = keywordKind(text, len);
  int reg;
  if (what == NULL) {
    if (isReservedName(text, len)) what = "a register name";
    else if (symtabFind(&a->syms, text, len) != NULL) what = "a symbol";
    else if (lookupRegister(a, text, len, &reg)) what = "a register alias";
  }
  if (what == NULL) return 1;
  diagEmit(&a->ctx->diags, DIAG_ERROR, name->span, 
    "'%.*s' is %s and cannot be the name of a macro", (int)len, text, what);
  return 0;
}

// .set: defines the name or gives it a new vaule. returns 0 on any error.
static int assignSymbol(Asm *a, const Token *name, int64_t value)
{
  if (!nameIsFree(a, name)) return 0;
  size_t len;
  const char *text = tokenText(a, name, &len);
  DiagList *diags = &a->ctx->diags;
  const Symbol *existing = NULL;
  if (symtabAssign(&a->syms, text, len, value, name->span, &existing) != NULL) return 1;
  diagEmit(diags, DIAG_ERROR, name->span,
    "symbol '%.*s' is already defined and is not a .set symbol", (int)len, text);
  diagEmit(diags, DIAG_INFO, existing->defined, "the first definition is here");
  return 0;
}

// .def with a 'report' param which controls if checks are reported.
static int addAlias(Asm *a, const Stmt *s, int report)
{
  size_t len;
  const char *text = tokenText(a, &s->name, &len);
  DiagList *diags = &a->ctx->diags;
  const char *keyword = keywordKind(text, len);
  if (report && isReservedName(text, len)) {
    diagEmit(diags, DIAG_ERROR, s->name.span, "'%.*s' is a register name and cannot be an alias", (int)len, text);
    return 0;
  }
  if (report && keyword != NULL) {
    diagEmit(diags, DIAG_ERROR, s->name.span, "'%.*s' is %s and cannot be an alias", (int)len, text, keyword);
    return 0;
  }
  if (report && symtabFind(&a->syms, text, len) != NULL) {
    diagEmit(diags, DIAG_ERROR, s->name.span, "'%.*s' is already a symbol", (int)len, text);
    return 0;
  }

  for (Alias *al = a->aliases; al != NULL; al = al->next) {
    if (sameName(al->name, al->len, text, len)) {
      if (al->reg == s->regNum) return 1;
      if (report) {
        diagEmit(diags, DIAG_ERROR, s->name.span, 
          "'%.*s' is already r%d: .undef it before giving it another register", (int)len, text, al->reg);
      }
      return 0;
    }
  }
  if (report) {
    for (const Alias *al = a->aliases; al != NULL; al = al->next) {
      if (al->reg == s->regNum) {
        diagEmit(diags, DIAG_WARNING, s->name.span,
          "register r%d already has the name '%.*s'", s->regNum, (int)al->len, al->name);
        break;
      }
    }
  }
  Alias *al = arenaAlloc(&a->ctx->arena, sizeof *al, _Alignof(Alias));
  al->name = text;
  al->len = len;
  al->reg = s->regNum;
  al->next = a->aliases;
  a->aliases = al;
  return 1;
}

static int removeAlias(Asm *a, const Stmt *s, int report)
{
  size_t len;
  const char *text = tokenText(a, &s->name, &len);
  for (Alias **p = &a->aliases; *p != NULL; p = &(*p)->next) {
    if (sameName((*p)->name, (*p)->len, text, len)) {
      *p = (*p)->next;
      return 1;
    }
  }
  if (report) diagEmit(&a->ctx->diags, DIAG_ERROR, s->name.span, "'%.*s' is not a register alias", (int)len, text);
  return 0;
}

// the first pos that is past the end of the segment
static int64_t segmentEnd(const Asm *a, Seg seg)
{
  switch (seg) {
    case SEG_CODE: return a->flash->size / 2;
    case SEG_EEPROM: return a->eeprom->size;
    default: return 1 << 16; // the data space default, will add device specifics later
  }
}

// where .dseg starts when no .org says otherwise.
// both avrasm2 and avra start at this address when no device is specified.
#define DATA_START 0x60

static void pass1Init(Asm *a)
{
  a->pos[SEG_CODE] = 0;
  a->pos[SEG_DATA] = DATA_START;
  a->pos[SEG_EEPROM] = 0;
  a->cur = SEG_CODE;
}

static void pass1Stmt(Asm *a, Stmt *s)
{
  int64_t *pos = a->pos;
  Seg cur = a->cur;
  s->seg = cur;
  s->pc = pos[cur];
  if (s->hasLabel) defineLabel(a, &s->label, pos[cur]);
  if (s->bad) {
    if (s->kind == STMT_INSTR && cur == SEG_CODE) pos[cur] += s->instr->words;
    return;
  }

  ExprEnv env = makeEnv(a, pos[cur]);
  ExprValue v;
  switch (s->kind) {
    case STMT_EMPTY:
      break;
    
    case STMT_DEVICE:
      if (a->hasDevice) {
        size_t l1, l2;
        const char *t1 = tokenText(a, &a->device, &l1);
        const char *t2 = tokenText(a, &s->name, &l2);
        if (!sameName(t1, l1, t2, l2)) {
          diagEmit(env.diags, DIAG_WARNING, s->name.span, "the device is changed from %.*s to %.*s", (int)l1, t1, (int)l2, t2);
        }
      }
      a->device = s->name;
      a->hasDevice = 1;
      break;

    case STMT_DEF:
      if (!addAlias(a, s, 1)) s->bad = 1;
      break;

    case STMT_UNDEF:
      if (!removeAlias(a, s, 1)) s->bad = 1;
      break;

    case STMT_SEG:
      cur = a->cur = s->target;
      break;

    case STMT_INSTR:
      if (cur != SEG_CODE) {
        diagEmit(env.diags, DIAG_ERROR, s->head.span, "instructions can only be placed in .cseg");
        s->bad = 1;
        break;
      }
      pos[cur] += s->instr->words;
      break;

    case STMT_ORG:
      if (!evalOperand(&s->ops[0], &env, &v)) break;
      if (v.value < 0 || v.value > segmentEnd(a, cur)) {
        diagEmit(env.diags, DIAG_ERROR, s->ops[0].span, 
          ".org address %lld is outside the segment (0 to %lld)", (long long)v.value, (long long)segmentEnd(a, cur));
        break;
      }
      pos[cur] = v.value;
      break;

    case STMT_EQU:
      if (evalOperand(&s->ops[0], &env, &v)) defineSymbol(a, &s->name, SYM_EQU, v.value);
      break;

    case STMT_SET:
      if (!evalOperand(&s->ops[0], &env, &v) || !assignSymbol(a, &s->name, v.value)) s->bad = 1;
      break;

    case STMT_DB:
    case STMT_DW:
      if (cur == SEG_DATA) {
        diagEmit(env.diags, DIAG_ERROR, s->head.span, 
          "%s cannot give .dseg a value: .byte reserves the space, nothing initialises it",
          s->kind == STMT_DB ? ".db" : ".dw");
        s->bad = 1;
        break;
      }
      if (cur == SEG_CODE) {
        if (s->dataBytes & 1) {
          diagEmit(env.diags, DIAG_WARNING, s->head.span, ".db in .cseg has an odd number of bytes: a zero byte is added");
        }
        pos[cur] += (s->dataBytes + 1) / 2;
      } else {
        pos[cur] += s->dataBytes;
      }
      break;

    case STMT_BYTE:
      if (cur == SEG_CODE) {
        diagEmit(env.diags, DIAG_ERROR, s->head.span, ".byte is not allowed in .cseg");
        break;
      }
      if (!evalOperand(&s->ops[0], &env, &v)) break;
      if (v.value < 0) {
        diagEmit(env.diags, DIAG_ERROR, s->ops[0].span, ".byte needs a size of 0 or more, not %lld", (long long)v.value);
        break;
      }
      pos[cur] += v.value;
      break;
  }
}

// ------- pass 2: encoding ----------------------------------------------

static void putBytes(Asm *a, const Stmt *s, Image *im, uint64_t addr, const uint8_t *bytes, size_t n)
{
  int overlap = 0;
  for (size_t i = 0; i < n; i++) {
    uint64_t at = addr + i;
    ImageStatus st = at > 0xFFFFFFFFu ? IMAGE_RANGE : imageWriteByte(im, (uint32_t)at, bytes[i]);
    if (st == IMAGE_RANGE) {
      diagEmit(&a->ctx->diags, DIAG_ERROR, s->head.span, 
        "address 0x%llX is outside the %s", (unsigned long long)at, im == a->eeprom ? "EEPROM" : "program memory");
      return;
    }
    if (st == IMAGE_OVERLAP) overlap = 1;
  }
  if (overlap) {
    diagEmit(&a->ctx->diags, DIAG_ERROR, s->head.span,
      "overlap: bytes at 0x%llX were placed already", (unsigned long long)addr);
  }
}

// one value of .db or .dw: an expr in lo..hi.
static int dataValue(const Operand *op, const ExprEnv *env, int64_t lo, int64_t hi, int64_t *out)
{
  ExprValue v;
  if (!evalOperand(op, env, &v)) return 0;
  if (v.value < lo || v.value > hi) {
    diagEmit(env->diags, DIAG_ERROR, op->span, 
      "value %lld does not fit: allowed %lld to %lld", (long long)v.value, (long long)lo, (long long)hi);
    return 0;
  }
  *out = v.value;
  return 1;
}

static void emitData(Asm *a, const Stmt *s, const ExprEnv *env)
{
  uint32_t unit = s->kind == STMT_DB ? 1 : 2;
  size_t total = s->dataBytes;
  if (s->seg == SEG_CODE && (total & 1)) total++;
  uint8_t *buf = arenaAlloc(&a->ctx->arena, total + 1, 1);
  size_t n = 0;
  int ok = 1;
  for (uint32_t i = 0; i < s->itemCount; i++) {
    const Operand *item = &s->items[i];
    if (isString(item)) {
      size_t len;
      const char *text = tokenText(a, &item->tokens.tokens[0], &len);
      memcpy(buf + n, text + 1, len - 2);
      n += len - 2;
      continue;
    }
    int64_t v;
    if (!dataValue(item, env, unit == 1 ? -128 : -32768, unit == 1 ? 255 : 65535, &v)) {
      ok = 0;
      continue;
    }
    buf[n++] = (uint8_t)((uint64_t)v & 0xFF);
    if (unit == 2) buf[n++] = (uint8_t)(((uint64_t)v >> 8) & 0xFF);
  }
  if (!ok) return;
  while (n < total) buf[n++] = 0;
  if (s->seg == SEG_CODE) putBytes(a, s, a->flash, (uint64_t)s->pc * 2, buf, n);
  else putBytes(a, s, a->eeprom, (uint64_t)s->pc, buf, n);
}

static void noteCallSite(Asm *a, Span callSite, uint32_t before);

static void pass2(Asm *a)
{
  a->aliases = NULL;
  for (Stmt *s = a->first; s != NULL; s = s->next) {
    if (s->bad) continue;
    a->scope = s->scope;
    a->encoding = s;
    uint32_t before = a->ctx->diags.count;
    ExprEnv env = makeEnv(a, s->pc);
    ExprValue v;
    switch (s->kind) {
      case STMT_EMPTY:
      case STMT_DEVICE:
      case STMT_SEG:
      case STMT_ORG:
      case STMT_EQU:
      case STMT_BYTE:
        break;
      case STMT_SET: // replay sets
        if (evalOperand(&s->ops[0], &env, &v)) assignSymbol(a, &s->name, v.value);
        break;
      case STMT_DEF:
        addAlias(a, s, 0);
        break;
      case STMT_UNDEF:
        removeAlias(a, s, 0);
        break;
      case STMT_DB:
      case STMT_DW:
        emitData(a, s, &env);
        break;
      case STMT_INSTR: {
        uint16_t words[2];
        int n = instrEncode(s->instr, s->ops, s->pc, &env, words);
        uint8_t bytes[4];
        for (int i = 0; i < n; i++) {
          bytes[2 * i] = (uint8_t)(words[i] & 0xFF);
          bytes[2 * i + 1] = (uint8_t)(words[i] >> 8);
        }
        if (n > 0) putBytes(a, s, a->flash, (uint64_t)s->pc * 2, bytes, (size_t)n * 2);
        break;
      }
    }
    noteCallSite(a, s->callSite, before);
  }
  a->scope = 0;
  a->encoding = NULL;
}

// ------- main driver ---------------------------------------------------

static void noteCallSite(Asm *a, Span callSite, uint32_t before)
{
  if (callSite.file != FILE_NONE && a->ctx->diags.count > before) {
    diagEmit(&a->ctx->diags, DIAG_INFO, callSite, "this line comes from the macro called here");
  }
}
int assembleOpt(AvrCtx *ctx, FileId file, Image *flash, Image *eeprom, const AsmOptions *opt)
{
  Asm a;
  memset(&a, 0, sizeof a);
  a.ctx = ctx;
  a.flash = flash;
  a.eeprom = eeprom;
  symtabInit(&a.syms, &ctx->arena, 0);

  ReaderHooks hooks = { hookEval, hookIsDefined, hookNameOk, &a };
  a.reader = readerCreate(ctx, &hooks, file, opt != NULL ? opt->includeDirs : NULL, opt != NULL ? opt->includeDirCount : 0);
  if (opt != NULL) {
    for (uint32_t i = 0; i < opt->defineCount; i++) readerDefine(a.reader, opt->defines[i]);
  }

  pass1Init(&a);
  ReaderLine rl;
  while (1) {
    a.scope = 0;
    if (!readerNext(a.reader, &rl)) break;
    uint32_t before = ctx->diags.count;
    a.scope = rl.scope;
    Stmt *s = parseLine(&a, &rl.line, rl.scope, rl.callSite);
    if (s != NULL) pass1Stmt(&a, s);
    noteCallSite(&a, rl.callSite, before);
  }
  a.scope = 0;

  pass2(&a);
  return !diagListHasErrors(&ctx->diags);
}

int assembleAll(AvrCtx *ctx, FileId file, Image *flash, Image *eeprom)
{
  return assembleOpt(ctx, file, flash, eeprom, NULL);
}

int assemble(AvrCtx *ctx, FileId file, Image *image)
{
  Image eeprom;
  imageInit(&eeprom, &ctx->arena, ASSEMBLE_DEFAULT_EEPROM_SIZE);
  return assembleAll(ctx, file, image, &eeprom);
}