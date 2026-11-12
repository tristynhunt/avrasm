#include "avrasm/assemble.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "avrasm/arena.h"
#include "avrasm/ctx.h"
#include "avrasm/device.h"
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
  STMT_DD,
  STMT_DQ,
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
  Device device;           // what the device file and .device say
  const Stmt *firstDevice; // the first .device: a warning if no device file comes
  int dataMoved;           // pass 1: a .org or .byte in .dseg has moved the data counter
  int64_t dataReserved;    // pass 1: the bytes .byte reserved in .dseg; pass1Stmt
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
  ExprEnv env = { &a->ctx->sources, &a->ctx->diags, lookupSymbol, a, 0, pc, lookupRegister, a->device.flashBytes / 2 };
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

// the size in bytes of one value of .db, .dw, .dd or .dq, and the directives name.
static uint32_t dataUnit(StmtKind kind)
{
  return kind == STMT_DB ? 1 : kind == STMT_DW ? 2 : kind == STMT_DD ? 4 : 8;
}

static const char *dataName(StmtKind kind)
{
  return kind == STMT_DB ? ".db" : kind == STMT_DW ? ".dw" : kind == STMT_DD ? ".dd" : ".dq";
}

static int isHexDigit(char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static void warnEscapes(Asm *a, const Token *str)
{
  static const struct {
    char c;
    const char *meant;    // what the escape would be
    const char *instead;  // what to put after the string for it
  } escapes[] = {
    { 'n', "a line feed", "10" }, { 'r', "a carriage return", "13" }, { 't', "a tab", "9" }, { '0', "a zero byte", "0" },
  };

  size_t n;
  const char *text = tokenText(a, str, &n);  // with its quotes
  for (size_t i = 1; i + 1 < n; i++) {
    if (text[i] != '\\') continue;
    char c = text[i + 1];
    Span at = { str->span.file, str->span.start + (uint32_t)i, str->span.start + (uint32_t)i + 2 };
    if (c == '\\') {
      diagEmit(&a->ctx->diags, DIAG_WARNING, at, 
        "in a string, \\\\ is two backslashes, not one: there are no escapes in strings so one \\ is enough");
      i++;
    } else if (c == '"') {
      diagEmit(&a->ctx->diags, DIAG_WARNING, at, "a backslash does not escape the quote after it: "
        "the string ends there (for a quote, put 34 after the string)");
    } else if (c == '\'') {
      diagEmit(&a->ctx->diags, DIAG_WARNING, at, 
        "in a string, \\' is a backslash and a quote: a ' needs no backslash there");
    } else if (c == 'x' && i + 2 < n && isHexDigit(text[i + 2])) {
      at.end += isHexDigit(text[i + 3]) && i + 3 < n - 1 ? 2 : 1;
      int digits = (int)(at.end - at.start) - 2;
      diagEmit(&a->ctx->diags, DIAG_WARNING, at, 
        "in a string, \\x%.*s is %d bytes, not the byte 0x%.*s: "
        "there are no escapes in strings (put 0x%.*s after the string)", 
        digits, text + i + 2, digits + 2, digits, text + i + 2, digits, text + i + 2);
    } else {
      for (size_t k = 0; k < sizeof escapes / sizeof escapes[0]; k++) {
        if (escapes[k].c != c) continue;
        diagEmit(&a->ctx->diags, DIAG_WARNING, at, "in a string, \\%c is the two bytes '\\' and '%c', not %s: "
          "there are no escapes in strings (for %s, put %s after the string)",
          c, c, escapes[k].meant, escapes[k].meant, escapes[k].instead);
      }
    }
  }
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
      warnEscapes(a, &item->tokens.tokens[0]);
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
  } else if (equalsNoCase(text, len, ".dd")) {
    s->kind = STMT_DD;
    parseData(a, line, from, s, ".dd", 4);
  } else if (equalsNoCase(text, len, ".dq")) {
    s->kind = STMT_DQ;
    parseData(a, line, from, s, ".dq", 8);
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

// a #pragma AVRPART line of a device file. until a .org or .byte has used the data space
// .dseg starts where the devices RAM starts.
static void hookPragma(void *user, Span at, const char *text, size_t len)
{
  Asm *a = user;
  const char *why = NULL;
  DevicePragma r = devicePragma(&a->device, text, len, &why);
  if (r != DEVICE_PRAGMA_OK)
    diagEmit(&a->ctx->diags, r == DEVICE_PRAGMA_ERROR ? DIAG_ERROR : DIAG_WARNING, at, "%s", why);
  if (!a->dataMoved) a->pos[SEG_DATA] = a->device.ramStart;
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
  const Device *d = &a->device;
  switch (seg) {
    case SEG_CODE: return d->flashBytes != 0 ?  d->flashBytes / 2 : a->flash->size / 2;
    case SEG_EEPROM: return d->eepromBytes != 0 ?  d->eepromBytes : a->eeprom->size;
    default: return d->fromFile ? (int64_t)d->ramStart + d->ramSize : 1 << 16;
  }
}

// the first position of the segment. the data space starts at the devices RAM, the others at 0.
static int64_t segmentStart(const Asm *a, Seg seg)
{
  return seg == SEG_DATA && a->device.fromFile ? a->device.ramStart : 0;
}

// after a statement has moved the counter of seg to end.
// emits one error if its past the end of the segment.
static int fitsSegment(Asm *a, Stmt *s, Seg seg, int64_t end)
{
  int64_t limit = segmentEnd(a, seg);
  if (end <= limit) return 1;
  int64_t unit = seg == SEG_CODE ? 2 : 1;
  const char *what = seg == SEG_CODE ? "program memory" : seg == SEG_EEPROM ? "EEPROM" : "RAM";
  if (seg == SEG_DATA && a->device.fromFile && a->device.ramSize == 0) {
    diagEmit(&a->ctx->diags, DIAG_ERROR, s->head.span, "the %s has no RAM", a->device.name);
  } else if (a->device.fromFile) {
    diagEmit(&a->ctx->diags, DIAG_ERROR, s->head.span, "this ends at 0x%llX, past the end of the %s's %s (0x%llX)",
      (unsigned long long)(end * unit - 1), a->device.name, what, (unsigned long long)(limit * unit - 1));
  } else {
    diagEmit(&a->ctx->diags, DIAG_ERROR, s->head.span, "this ends at 0x%llX, past the end of the %s (0x%llX)",
      (unsigned long long)(end * unit - 1), what, (unsigned long long)(limit * unit - 1));
  }
  s->bad = 1;
  return 0;
}

// where .dseg starts when no .org says otherwise.
// both avrasm2 and avra start at this address when no device is specified.
#define DATA_START 0x60

static void pass1Init(Asm *a)
{
  deviceInit(&a->device);
  a->pos[SEG_CODE] = 0;
  a->pos[SEG_DATA] = a->device.ramStart;
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
    
    case STMT_DEVICE: {
      size_t len;
      const char *text = tokenText(a, &s->name, &len);
      if (a->device.name[0] != '\0' && !sameName(a->device.name, strlen(a->device.name), text, len)) {
        diagEmit(env.diags, DIAG_ERROR, s->name.span,
          "the device is the %s already: .device cannot change it", a->device.name);
        s->bad = 1;
        break;
      }
      if (!deviceSetName(&a->device, text, len)) {
        diagEmit(env.diags, DIAG_ERROR, s->name.span, "this device name is too long");
        s->bad = 1;
        break;
      }
      if (a->firstDevice == NULL) a->firstDevice = s;
      break;
    }

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
      fitsSegment(a, s, cur, pos[cur]);
      break;

    case STMT_ORG:
      if (!evalOperand(&s->ops[0], &env, &v)) break;
      if (v.value < segmentStart(a, cur) || v.value > segmentEnd(a, cur)) {
        diagEmit(env.diags, DIAG_ERROR, s->ops[0].span, ".org address %lld is outside the segment (%lld to %lld)",
          (long long)v.value, (long long)segmentStart(a, cur), (long long)segmentEnd(a, cur));
        break;
      }
      pos[cur] = v.value;
      if (cur == SEG_DATA) a->dataMoved = 1;
      break;

    case STMT_EQU:
      if (evalOperand(&s->ops[0], &env, &v)) defineSymbol(a, &s->name, SYM_EQU, v.value);
      break;

    case STMT_SET:
      if (!evalOperand(&s->ops[0], &env, &v) || !assignSymbol(a, &s->name, v.value)) s->bad = 1;
      break;

    case STMT_DB:
    case STMT_DW:
    case STMT_DD:
    case STMT_DQ:
      if (cur == SEG_DATA) {
        diagEmit(env.diags, DIAG_ERROR, s->head.span, 
          "%s cannot give .dseg a value: .byte reserves the space, nothing initialises it",
          dataName(s->kind));
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
      fitsSegment(a, s, cur, pos[cur]);
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
      if (cur == SEG_DATA) {
        a->dataMoved = 1;
        a->dataReserved += v.value;
      }
      fitsSegment(a, s, cur, pos[cur]);
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
  uint32_t unit = dataUnit(s->kind);
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
    if (unit == 8) {
      ExprValue w;
      if (!evalOperandWide(item, env, &w)) {
        ok = 0;
        continue;
      }
      v = w.value;
    } else if (!dataValue(item, env, unit == 1 ? -128 : unit == 2 ? -32768 : INT32_MIN,
        unit == 1 ? 255 : unit == 2 ? 65535 : (int64_t)UINT32_MAX, &v)) {
      ok = 0;
      continue;
    }
    for (uint32_t b = 0; b < unit; b++) buf[n++] = (uint8_t)(((uint64_t)v >> (8 * b)) & 0xFF);
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
      case STMT_DD:
      case STMT_DQ:
        emitData(a, s, &env);
        break;
      case STMT_INSTR: {
        uint16_t words[2];
        int n = instrEncode(s->instr, s->ops, s->pc, &env, words);
        if (n > 0 && !deviceHas(&a->device, deviceGroup(words[0]))) {
          // unsupported instruction
          diagEmit(&a->ctx->diags, DIAG_ERROR, s->head.span,
            "%s is not available on the %s", s->instr->name, a->device.name);
          break;
        }
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

static int sameSpan(Span a, Span b)
{
  return a.file == b.file && a.start == b.start;
}

// a label, .equ or .set name with the value it ended with. if this statement is where the symbol
// table has it from a second definition (an error) is not added. every .set of a name defines it,
// so for .set the outline keeps the first
static void addSymbol(Asm *a, Outline *o, const Token *name, OutlineKind kind, char seg)
{
  size_t len;
  const char *text = tokenText(a, name, &len);
  const Symbol *sym = symtabFind(&a->syms, text, len);
  if (sym == NULL) return;
  SymKind want = kind == OUTLINE_LABEL ? SYM_LABEL : kind == OUTLINE_EQU ? SYM_EQU : SYM_SET;
  if (sym->kind != want || (kind != OUTLINE_SET && !sameSpan(sym->defined, name->span))) return;
  outlineAddName(o, kind, text, len, sym->value, seg, name->span);
}

// fills the outline from the statements after pass 2 and from the reader (macros and #define names).
static void fillOutline(Asm *a, Outline *o)
{
  static const char segLetter[SEG_COUNT] = { 'c', 'd', 'e' };
  for (const Stmt *s = a->first; s != NULL; s = s->next) {
    if (s->hasLabel && s->scope == 0) addSymbol(a, o, &s->label, OUTLINE_LABEL, segLetter[s->seg]);
    if (s->bad) continue;
    size_t len;
    const char *text;
    OutlineCode code = { s->head.span, s->pc, 0, 0, s->callSite, s->scope };
    switch (s->kind) {
      case STMT_EQU:
        addSymbol(a, o, &s->name, OUTLINE_EQU, 0);
        break;
      case STMT_SET:
        addSymbol(a, o, &s->name, OUTLINE_SET, 0);
        break;
      case STMT_DEF:
        text = tokenText(a, &s->name, &len);
        outlineAddName(o, OUTLINE_DEF, text, len, s->regNum, 0, s->name.span);
        break;
      case STMT_INSTR:
        code.words = s->instr->words;
        if (s->seg == SEG_CODE) outlineAddCode(o, &code);
        break;
      case STMT_DB:
      case STMT_DW:
      case STMT_DD:
      case STMT_DQ:
        code.words = (s->dataBytes + 1) / 2;
        code.isData = 1;
        if (s->seg == SEG_CODE && code.words > 0) outlineAddCode(o, &code);
        break;
      default:
        break;
    }
  }
  readerOutline(a->reader, o);
}

int assembleOpt(AvrCtx *ctx, FileId file, Image *flash, Image *eeprom, const AsmOptions *opt)
{
  Asm a;
  memset(&a, 0, sizeof a);
  a.ctx = ctx;
  a.flash = flash;
  a.eeprom = eeprom;
  symtabInit(&a.syms, &ctx->arena, 0);

  ReaderHooks hooks = { hookEval, hookIsDefined, hookNameOk, hookPragma, &a };
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
  if (a.firstDevice != NULL && !a.device.fromFile) {
    diagEmit(&ctx->diags, DIAG_WARNING, a.firstDevice->name.span,
      "no device file is included, so the memory sizes and instructions of the %s are not checked "
      "(include its file, e.g. .include \"m324Adef.inc\")", a.device.name);
  }

  pass2(&a);
  if (opt != NULL && opt->outline != NULL) fillOutline(&a, opt->outline);
  if (diagListHasErrors(&ctx->diags)) return 0;
  if (flash->blockCount == 0 && eeprom->blockCount == 0 && a.dataReserved == 0) return ASSEMBLE_EMPTY;
  return 1;
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