#include "avrasm/expr.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "avrasm/diag.h"
#include "avrasm/lexer.h"
#include "avrasm/source.h"
#include "avrasm/tokline.h"

#define MAX_DEPTH 200

typedef struct Eval
{
  TokenCursor *cur;
  const ExprEnv *env;
  int depth;
} Eval;

static int fail(Eval *e, Span span, const char *fmt, ...) AVRASM_PRINTF(3, 4);
static int fail(Eval *e, Span span, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  diagEmitV(e->env->diags, DIAG_ERROR, span, fmt, ap);
  va_end(ap);
  return 0;
}

// the bytes of a token and how many there are.
// not nul-terminated as it points into the source.
static const char *textOf(const Eval *e, const Token *t, size_t *len)
{
  const SourceFile *f = sourceMapGet(e->env->sources, t->span.file);
  *len = f == NULL ? 0 : t->span.end - t->span.start;
  return f == NULL ? "" : f->text + t->span.start;
}

// does text[0..len) equal 'lower' (all lower case) ignoring ascii case?
static int nameIs(const char *text, size_t len, const char *lower)
{
  if (strlen(lower) != len) return 0;
  for (size_t i = 0; i < len; i++) {
    char c = text[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != lower[i]) return 0;
  }
  return 1;
}

// if the current token has this kind it is consumed and return 1.
// else it is reported and return 0.
static int expect(Eval *e, TokenKind kind, const char *what)
{
  if (cursorAccept(e->cur, kind)) return 1;
  return fail(e, cursorPeek(e->cur)->span, "expected %s", what);
}

static int parseTernary(Eval *e, ExprValue *out);
static int parseUnary(Eval *e, ExprValue *out);

// ------   operators   ----------------------------------------------- // 

// how strongly a binary operator binds, 0 if the token is not one of them
static int binaryPrec(TokenKind k)
{
  switch (k) {
    case TOK_OROR:   return 4;
    case TOK_ANDAND: return 5;
    case TOK_PIPE:   return 6;
    case TOK_CARET:  return 7;
    case TOK_AMP:    return 8;
    case TOK_EQEQ:
    case TOK_NE:     return 9;
    case TOK_LT:
    case TOK_LE:
    case TOK_GT:
    case TOK_GE:     return 10;
    case TOK_SHL:
    case TOK_SHR:    return 11;
    case TOK_MINUS:
    case TOK_PLUS:   return 12;
    case TOK_PERCENT:
    case TOK_SLASH:
    case TOK_STAR:   return 13;
    default:         return 0;
  }
}

// right shift which keeps the sign without relying on 
// how the C compiler treats shifts on negatives
static int64_t shiftRight(int64_t v, unsigned n)
{
  if (v >= 0) return (int64_t)((uint64_t)v >> n);
  return ~(int64_t)(~(uint64_t)v >> n);
}

// evaluates a binary op of values and stores it in *out. op is the operator token
static int applyBinary(Eval *e, const Token *op, ExprValue l, ExprValue r, ExprValue *out)
{
  if ((op->kind == TOK_SLASH || op->kind == TOK_PERCENT) && r.known && !r.value)
    return fail(e, op->span, "division by zero");
  if ((op->kind == TOK_SHL || op->kind == TOK_SHR) && r.known && (r.value < 0 || r.value > 63))
    return fail(e, op->span, "shift count %lld is out of range (0 to 63)", (long long)r.value);
  out->value = 0;
  out->known = l.known && r.known;
  if (!out->known) return 1;

  uint64_t a = (uint64_t)l.value, b = (uint64_t)r.value;
  switch (op->kind) {
    case TOK_PLUS: out->value = (int64_t)(a + b); break;
    case TOK_MINUS: out->value = (int64_t)(a - b); break;
    case TOK_STAR: out->value = (int64_t)(a * b); break;
    case TOK_SLASH: out->value = r.value == -1 ? (int64_t)(0 - a) : l.value / r.value; break;
    case TOK_PERCENT: out->value = r.value == -1 ? 0 : l.value % r.value; break;
    case TOK_SHL: out->value = (int64_t)(a << b); break;
    case TOK_SHR: out->value = shiftRight(l.value, (unsigned)b); break;
    case TOK_LT: out->value = l.value < r.value; break;
    case TOK_GT: out->value = l.value > r.value; break;
    case TOK_LE: out->value = l.value <= r.value; break;
    case TOK_GE: out->value = l.value >= r.value; break;
    case TOK_EQEQ: out->value = l.value == r.value; break;
    case TOK_NE: out->value = l.value != r.value; break;
    case TOK_AMP: out->value = (int64_t)(a & b); break;
    case TOK_PIPE: out->value = (int64_t)(a | b); break;
    case TOK_CARET: out->value = (int64_t)(a ^ b); break;
    case TOK_ANDAND: out->value = l.value != 0 && r.value != 0; break;
    case TOK_OROR: out->value = l.value != 0 || r.value != 0; break;
    default: return fail(e, op->span, "internal error: not a binary operator");
  }
  return 1;
}

// ------   functions   ----------------------------------------------- // 

typedef enum Func
{
  FN_LOW, FN_HIGH, FN_BYTE3, FN_BYTE4, 
  FN_LWRD, FN_HWRD, FN_PAGE, FN_EXP2, FN_LOG2, FN_ABS,
  FN_FLOAT, FN_LATER // this is stuff im too lazy to implement yet
} Func;

typedef struct FuncEntry {
  const char *name;
  Func fn;
} FuncEntry;

static const FuncEntry funcs[] = {
  { "low", FN_LOW },     { "byte1", FN_LOW },   { "high", FN_HIGH },  { "byte2", FN_HIGH },
  { "byte3", FN_BYTE3 }, { "byte4", FN_BYTE4 }, { "lwrd", FN_LWRD },  { "hwrd", FN_HWRD },
  { "page", FN_PAGE },   { "exp2", FN_EXP2 },   { "log2", FN_LOG2 },  { "abs", FN_ABS },
  { "int", FN_FLOAT },   { "frac", FN_FLOAT },  { "q7", FN_FLOAT },   { "q15", FN_FLOAT },
  { "strlen", FN_LATER },
};

static int applyFunc(Eval *e, Func fn, const Token *name, ExprValue arg, ExprValue *out)
{
  // low = bits 0-7, high/byte2 = 8-15, byte3 = 16-23, byte4 = 24-31,
  // lwrd = bits 0-15, hwrd = 16-31, page = bits 16-21 (6-bits).
  // exp2(n) = 1 << (n & 31).
  // log2(n) = floor(log2 n) of n as an unsigned 32-bit number.
  // abs(n): negated through uint64_t
  out->value = 0;
  out->known = arg.known;
  if (!arg.known) return 1;
  uint64_t v = (uint64_t)arg.value;
  switch (fn) {
    case FN_LOW: out->value = (int64_t)(v & 0xFF); break;
    case FN_HIGH: out->value = (int64_t)((v >> 8) & 0xFF); break;
    case FN_BYTE3: out->value = (int64_t)((v >> 16) & 0xFF); break;
    case FN_BYTE4: out->value = (int64_t)((v >> 24) & 0xFF); break;
    case FN_LWRD: out->value = (int64_t)(v & 0xFFFF); break;
    case FN_HWRD: out->value = (int64_t)((v >> 16) & 0xFFFF); break;
    case FN_PAGE: out->value = (int64_t)((v >> 16) & 0x3F); break;
    case FN_EXP2:
      if (arg.value < 0 || arg.value > 31) return fail(e, name->span, "exp2 needs an argument from 0 to 31");
      out->value = (int64_t)1 << arg.value;
      break;
    case FN_LOG2: {
      if (arg.value < 1 || arg.value > 0xFFFFFFFF) return fail(e, name->span, "log2 needs an argument from 1 to 0xFFFFFFFF");
      int n = 0;
      while (v > 1) {
        v >>= 1;
        n++;
      }
      out->value = n;
      break;
    }
    case FN_ABS: out->value = arg.value < 0 ? (int64_t)(0 - v) : arg.value; break;
    case FN_FLOAT:
    case FN_LATER: return fail(e, name->span, "internal error: unsupported function reached applyFunc");
  }
  return 1;
}

// ------   primaries   ----------------------------------------------- // 

// parse an identifier primary. includes defined, pc, a function call, or a symbol
static int parseIdent(Eval *e, ExprValue *out)
{
  const Token *name = cursorNext(e->cur);
  size_t len;
  const char *text = textOf(e, name, &len);
  const ExprEnv *env = e->env;

  // defined(IDENT) -- does a symbol exist
  if (nameIs(text, len, "defined")) {
    int paren = cursorAccept(e->cur, TOK_LPAREN);
    const Token *sym = cursorPeek(e->cur);
    if (sym->kind != TOK_IDENT) return fail(e, sym->span, "expected a symbol name");
    (void)cursorNext(e->cur);
    if (paren && !expect(e, TOK_RPAREN, "')'")) return 0;
    size_t symLen;
    const char *symText = textOf(e, sym, &symLen);
    int64_t ignored;
    out->value = env->lookup != NULL && env->lookup(env->user, symText, symLen, &ignored);
    out->known = 1;
    return 1;
  }

  // PC -- current program counter value
  if (nameIs(text, len, "pc")) {
    out->value = env->pc;
    out->known = 1;
    return 1;
  }

  // a name existing in the funcs[] array
  for (size_t i = 0; i < sizeof funcs / sizeof funcs[0]; i++) {
    if (!nameIs(text, len, funcs[i].name)) continue;
    if (funcs[i].fn == FN_FLOAT)
      return fail(e, name->span, "%.*s works on floating-point values, which are not supported", (int)len, text);
    if (funcs[i].fn == FN_LATER) return fail(e, name->span, "this function is not supported yet");
    if (!expect(e, TOK_LPAREN, "'(' after a function name")) return 0;
    ExprValue arg;
    if (!parseTernary(e, &arg)) return 0;
    if (!expect(e, TOK_RPAREN, "')'")) return 0;
    return applyFunc(e, funcs[i].fn, name, arg, out);
  }

  int64_t value = 0;
  if (env->lookup != NULL && env->lookup(env->user, text, len, &value)) {
    out->value = value;
    out->known = 1;
    return 1;
  }
  if (env->allowUndefined) {
    out->value = 0;
    out->known = 0;
    return 1;
  }
  return fail(e, name->span, "undefined symbol '%.*s'", (int)len, text);
}

static int parsePrimary(Eval *e, ExprValue *out)
{
  const Token *t = cursorPeek(e->cur);
  switch (t->kind) {
    case TOK_NUMBER:
    case TOK_CHAR:
      (void)cursorNext(e->cur);
      out->value = (int64_t)t->value;
      out->known = 1;
      return 1;
    case TOK_FLOAT:
      return fail(e, t->span, "floating-point values are not supported yet");
    case TOK_LPAREN:
      (void)cursorNext(e->cur);
      if (!parseTernary(e, out)) return 0;
      return expect(e, TOK_RPAREN, "')'");
    case TOK_IDENT:
      return parseIdent(e, out);
    case TOK_ERROR: 
      return 0; // has been reported by the lexer
    default:
      return fail(e, t->span, "expected an expression");
  }
}

// ---- unary, binary, ternary ---------------------------------------- // 

static int parseUnaryBody(Eval *e, ExprValue *out)
{
  const Token *t = cursorPeek(e->cur);
  if (t->kind != TOK_MINUS && t->kind != TOK_TILDE && t->kind != TOK_BANG)
    return parsePrimary(e, out);

  (void)cursorNext(e->cur);
  ExprValue v;
  if (!parseUnary(e, &v)) return 0;
  out->value = 0;
  out->known = v.known;
  if (!v.known) return 1;
  if (t->kind == TOK_MINUS) out->value = (int64_t)(0 - (uint64_t)v.value);
  else if (t->kind == TOK_TILDE) out->value = ~v.value;
  else out->value = v.value == 0;
  return 1;
}

// depth guard is a wrapper around the real function so that there is only
// one place that counts up and down.
static int parseUnary(Eval *e, ExprValue *out)
{
  if (++e->depth > MAX_DEPTH) return fail(e, cursorPeek(e->cur)->span, "expression is nested too deeply");
  int ok = parseUnaryBody(e, out);
  e->depth--;
  return ok;
}

// precedence climbing happens here
static int parseBinary(Eval *e, int minPrec, ExprValue *out)
{
  ExprValue lhs;
  if (!parseUnary(e, &lhs)) return 0;
  while(1) {
    const Token *op = cursorPeek(e->cur);
    int prec = binaryPrec(op->kind);
    if (prec == 0 || prec < minPrec) break;
    (void)cursorNext(e->cur);
    ExprValue rhs, result;
    if (!parseBinary(e, prec + 1, &rhs)) return 0;
    if (!applyBinary(e, op, lhs, rhs, &result)) return 0;
    lhs = result;
    if (prec == 9 || prec == 10) {
      const Token *next = cursorPeek(e->cur);
      if (binaryPrec(next->kind) == prec)
        return fail(e, next->span, "comparison operators cannot be chained, add parentheses");
    }
  }
  *out = lhs;
  return 1;
}

static int parseTernaryBody(Eval *e, ExprValue *out)
{
  ExprValue cond;
  if (!parseBinary(e, 4, &cond)) return 0;
  if (!cursorAccept(e->cur, TOK_QUESTION)) {
    *out = cond;
    return 1;
  }
  
  ExprValue a, b;
  if (!parseTernary(e, &a)) return 0;
  if (!expect(e, TOK_COLON, "':'")) return 0;
  if (!parseTernary(e, &b)) return 0;

  if (!cond.known) {
    out->value = 0;
    out->known = 0;
  } else {
    *out = cond.value != 0 ? a : b;
  }
  return 1;
}

static int parseTernary(Eval *e, ExprValue *out)
{
  if (++e->depth > MAX_DEPTH) return fail(e, cursorPeek(e->cur)->span, "expression is nested too deeply");
  int ok = parseTernaryBody(e, out);
  e->depth--;
  return ok;
}

int exprEvalWide(TokenCursor *cur, const ExprEnv *env, ExprValue *out)
{
  Eval e = { cur, env, 0 };
  if (parseTernary(&e, out)) return 1;
  out->value = 0;
  out->known = 0;
  return 0;
}

int exprEval(TokenCursor *cur, const ExprEnv *env, ExprValue *out)
{
  Span at = cursorPeek(cur)->span;
  if (!exprEvalWide(cur, env, out)) return 0;
  if (!out->known) return 1;
  if (out->value < INT32_MIN || out->value > (int64_t)UINT32_MAX) {
    diagEmit(env->diags, DIAG_ERROR, at, "%lld does not fit in 32 bits", (long long)out->value);
    out->value = 0;
    out->known = 0;
    return 0;
  }
  // thats one cursed cast...
  out->value = (int64_t)(int32_t)(uint32_t)(uint64_t)out->value;
  return 1;
}

int exprIsBuiltinName(const char *name, size_t len)
{
  if (nameIs(name, len, "pc") || nameIs(name, len, "defined")) return 1;
  for (size_t i = 0; i < sizeof funcs / sizeof funcs[0]; i++) {
    if (nameIs(name, len, funcs[i].name)) return 1;
  }
  return 0;
}