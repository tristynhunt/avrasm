#include "avrasm/reader.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "avrasm/arena.h"
#include "avrasm/ctx.h"
#include "avrasm/diag.h"
#include "avrasm/expr.h"
#include "avrasm/lexer.h"
#include "avrasm/source.h"
#include "avrasm/tokline.h"

// ------- limits --------------------------------------------------------

#define READER_MAX_INCLUDE_DEPTH  8
#define READER_MAX_MACRO_DEPTH   16
#define READER_MAX_DEFINE_DEPTH  32
#define READER_MAX_PARAMS        16
#define MACRO_MAX_ARGS           10

// ------- things the reader remembers -----------------------------------

// a growing list of tokens
typedef struct TokVec
{
  Token *v;
  uint32_t n;
  uint32_t cap;
} TokVec;

static void vecPush(Arena *arena, TokVec *vec, Token t)
{
  if (vec->n == vec->cap) {
    uint32_t cap = vec->cap != 0 ? vec->cap * 2 : 16;
    Token *bigger = arenaAlloc(arena, cap * sizeof *bigger, _Alignof(Token));
    if (vec->n != 0) memcpy(bigger, vec->v, vec->n * sizeof *bigger);
    vec->v = bigger;
    vec->cap = cap;
  }
  vec->v[vec->n++] = t;
}

// #define NAME body, or #define NAME(a, b) body. the body points into the source text.
typedef struct Define
{
  const char *name;
  size_t len;
  int isFunc;
  Token params[READER_MAX_PARAMS];
  uint32_t paramCount;
  Token *body;
  uint32_t bodyCount;
  Span defined;
  struct Define *next;
} Define;

typedef struct Macro
{
  const char *name;
  size_t len;
  TokenLine *lines;
  uint32_t lineCount;
  uint32_t argc;    // 1 + highest arg number the body uses
  Span defined;
  struct Macro *next;
} Macro;

// one open #if or .if from its first line ot its #endif or .endif
typedef struct Cond
{
  int pp;               // 1: #if family, 0: .if family
  int parentActive;     // the lines around it are being assembled
  int parentPpActive;   // the lines around it are seen by the preprocessor at all
  int active;           // the branch in force is being assembled
  int ppActive;         // ... is seen by the preprocessor (a .if that is false hides nothing from it)
  int taken;            // some branch of it was taken already
  int sawElse;
  Span opened;
  struct Cond *up;
} Cond;

typedef enum FrameKind
{
  FRAME_FILE,
  FRAME_MACRO
} FrameKind;

typedef struct Frame
{
  FrameKind kind;
  struct Frame *up;
  const Cond *condBase; // r->conds when the frame begind: conditionals above belong to the frame
  int done;             // nothing more to read (end of file seen or .exit)
  int exited;           // ended by .exit
  Lexer lx;             // FRAME_FILE
  const Macro *macro;   // FRAME_MACRO
  uint32_t next;        // the next body line
  TokenLine call;       // the line of the call, args are ranges of its tokens
  uint32_t argStart[MACRO_MAX_ARGS];
  uint32_t argCount[MACRO_MAX_ARGS];
  uint32_t args;
  uint32_t scope;
  Span callSite;
} Frame;

struct Reader
{
  AvrCtx *ctx;
  ReaderHooks hooks;
  const char *baseDir;  // the dir of the main file
  const char *const *dirs;
  uint32_t dirCount;
  Define *defines;
  Macro *macros;
  Cond *conds;
  Frame *top;
  uint32_t fileDepth;
  uint32_t macroDepth;
  uint32_t nextScope;
  Macro *building;      // the macro whose body is being read, or NULL
  Frame *buildFrame;    // the frame it started in
  TokenLine *body;
  uint32_t bodyCount;
  uint32_t bodyCap;
  uint32_t maxArg;
  int pending;          // a line to hand out before reading on
  TokenLine pendingLine;
  uint32_t pendingScope;
  Span pendingCall;
  DiagList scratch;     // where the lexers complaints go inside code that isnt assembled
};

// ------- small helpers -------------------------------------------------

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

static int sameNoCase(const char *a, size_t aLen, const char *b, size_t bLen)
{
  if (aLen != bLen) return 0;
  for (size_t i = 0; i < aLen; i++) {
    if (lowerAscii(a[i]) != lowerAscii(b[i])) return 0;
  }
  return 1;
}

static const char *tokText(const Reader *r, const Token *t, size_t *len)
{
  const SourceFile *f = sourceMapGet(&r->ctx->sources, t->span.file);
  *len = t->span.end - t->span.start;
  return f->text + t->span.start;
}

static int isEnd(const Token *t)
{
  return t->kind == TOK_NEWLINE || t->kind == TOK_EOF;
}

// the index of the mnemonic or directive: 2 after a 'label:', else 0.
static uint32_t headIndex(const TokenLine *line)
{
  if (line->count > 2 && line->tokens[0].kind == TOK_IDENT && line->tokens[1].kind == TOK_COLON) return 2;
  return 0;
}

// checks if the line is over after token 'from'. reports the first token that should not be there
static int expectEnd(Reader *r, const TokenLine *line, uint32_t from, const char *what)
{
  const Token *t = &line->tokens[from < line->count ? from : line->count - 1];
  if (isEnd(t)) return 1;
  if (t->kind != TOK_ERROR) diagEmit(&r->ctx->diags, DIAG_ERROR, t->span, "unexpected text after %s", what);
  return 0;
}

static void growBody(Reader *r, TokenLine line)
{
  if (r->bodyCount == r->bodyCap) {
    uint32_t cap = r->bodyCap != 0 ? r->bodyCap * 2 : 8;
    TokenLine *bigger = arenaAlloc(&r->ctx->arena, cap * sizeof *bigger, _Alignof(TokenLine));
    if (r->bodyCount != 0) memcpy(bigger, r->body, r->bodyCount * sizeof *bigger);
    r->body = bigger;
    r->bodyCap = cap;
  }
  r->body[r->bodyCount++] = line;
}

// ------- #define: replacing names --------------------------------------

static const Define *findDefine(const Reader *r, const char *name, size_t len)
{
  for (const Define *d = r->defines; d != NULL; d = d->next) {
    if (d->len == len && memcmp(d->name, name, len) == 0) return d;
  }
  return NULL;
}

static int paramIndex(const Reader *r, const Define *d, const Token *t)
{
  size_t len, plen;
  const char *text = tokText(r, t, &len);
  for (uint32_t i = 0; i < d->paramCount; i++) {
    const char *p = tokText(r, &d->params[i], &plen);
    if (plen == len && memcmp(p, text, len) == 0) return (int)i;
  }
  return -1;
}

// appends in[0..n) to 'out' with every #define name replaced. 'hide' holds the defines
// which are being expanded right now: a name does not expand inside its own replacement, so
// '#define A A' ends. returns 0 after reporting an error.
static int expandTokens(Reader *r, const Token *in ,uint32_t n, TokVec *out, const Define **hide, uint32_t nhide)
{
  Arena *arena = &r->ctx->arena;
  for (uint32_t i = 0; i < n; i++) {
    const Token *t = &in[i];
    const Define *d = NULL;
    if (t->kind == TOK_IDENT) {
      size_t len;
      const char *text = tokText(r, t, &len);
      d = findDefine(r, text, len);
      for (uint32_t k = 0; k < nhide; k++) {
        if (hide[k] == d) d = NULL;
      }
    }
    if (d == NULL) {
      vecPush(arena, out, *t);
      continue;
    }
    if (nhide >= READER_MAX_DEFINE_DEPTH) {
      diagEmit(&r->ctx->diags, DIAG_ERROR, t->span, 
        "#define names are nested more than %d deep", READER_MAX_DEFINE_DEPTH);
      return 0;
    }
    const Define *inner[READER_MAX_DEFINE_DEPTH];
    memcpy(inner, hide, nhide * sizeof *hide);
    inner[nhide] = d;

    if (!d->isFunc) {
      if (!expandTokens(r, d->body, d->bodyCount, out, inner, nhide + 1)) return 0;
      continue;
    }
    // a function like name without a call is just a name
    if (i + 1 >= n || in[i + 1].kind != TOK_LPAREN) {
      vecPush(arena, out, *t);
      continue;
    }

    // the arguments split at commas that are not inside parens
    uint32_t argStart[READER_MAX_PARAMS], argEnd[READER_MAX_PARAMS];
    uint32_t argc = 0, depth = 1, j = i + 2, start = i + 2;
    for (; j < n; j++) {
      if (in[j].kind == TOK_LPAREN) {
        depth++;
      } else if (in[j].kind == TOK_RPAREN) {
        if (--depth == 0 ) break;
      } else if (in[j].kind == TOK_COMMA && depth == 1) {
        if (argc < READER_MAX_PARAMS) { argStart[argc] = start; argEnd[argc] = j; }
        argc++;
        start = j + 1;
      }
    }
    if (j >= n) {
      diagEmit(&r->ctx->diags, DIAG_ERROR, t->span, "this call has no closing parenthesis");
      return 0;
    }
    if (argc < READER_MAX_PARAMS) { argStart[argc] = start; argEnd[argc] = j; }
    argc++;
    if (d->paramCount == 0 && argc == 1 && argStart[0] == argEnd[0]) argc = 0;  // F()
    if (argc != d->paramCount) {
      diagEmit(&r->ctx->diags, DIAG_ERROR, t->span, "'%.*s' takes %u argument%s, found %u",
        (int)d->len, d->name, d->paramCount, d->paramCount == 1 ? "" : "s", argc);
      return 0;
    }
    TokVec body = { NULL, 0, 0 };
    for (uint32_t k = 0; k < d->bodyCount; k++) {
      int p = d->body[k].kind == TOK_IDENT ? paramIndex(r, d, &d->body[k]) : -1;
      if (p < 0) {
        vecPush(arena, &body, d->body[k]);
      } else {
        for (uint32_t m = argStart[p]; m < argEnd[p]; m++) vecPush(arena, &body, in[m]);
      }
    }
    if (!expandTokens(r, body.v, body.n, out, inner, nhide + 1)) return 0;
    i = j; // loop moves past closing paren
  }
  return 1;
}

// replaces the #define names in the line (the NEWLINE or EOF at the end stays)
static void ppReplace(Reader *r, TokenLine *line)
{
  if (r->defines == NULL) return;
  TokVec out = { NULL, 0, 0 };
  const Define *hide[1];
  if (!expandTokens(r, line->tokens, line->count - 1, &out, hide, 0)) return;
  // the error was reported, keep the line
  vecPush(&r->ctx->arena, &out, line->tokens[line->count - 1]);
  line->tokens = out.v;
  line->count = out.n;
}

// defined(NAME) becomes the number 0 or 1, from token 'from' on.
// in the preprocessor NAME is a #define name. in the assembler, it is whatever the hook knows
static void rewriteDefined(Reader *r, TokenLine *line, uint32_t from, int pp)
{
  TokVec out = { NULL, 0, 0 };
  for (uint32_t i = 0; i < line->count; i++) {
    const Token *t = &line->tokens[i];
    size_t len;
    const char *word = t->kind == TOK_IDENT ? tokText(r, t, &len) : "";
    // is it in the correct form of defined(NAME)
    if (i >= from && t->kind == TOK_IDENT && i + 3 < line->count && line->tokens[i + 1].kind == TOK_LPAREN &&
        line->tokens[i + 2].kind == TOK_IDENT && line->tokens[i + 3].kind == TOK_RPAREN && 
        equalsNoCase(word, len, "defined")) {
      size_t nlen;
      const char *name = tokText(r, &line->tokens[i + 2], &nlen);
      if (!pp && findDefine(r, name, len) != NULL) {
        diagEmit(&r->ctx->diags, DIAG_ERROR, line->tokens[i + 2].span, 
          "'%.*s' is a #define name, which is replaced before the assembler sees it: use #if defined(...)", (int)len, name);
      }
      int v = pp ? findDefine(r, name, nlen) != NULL : r->hooks.isDefined(r->hooks.user, name, nlen);
      Token num = { TOK_NUMBER, { t->span.file, t->span.start, line->tokens[i + 3].span.end }, (uint64_t)v, 0.0 };
      vecPush(&r->ctx->arena, &out, num);
      i += 3;
      continue;
    }
    vecPush(&r->ctx->arena, &out, *t);
  }
  line->tokens = out.v;
  line->count = out.n;
}

// ------- conditionals --------------------------------------------------

typedef enum CondKind
{
  C_IF = 0,
  C_IFDEF,
  C_IFNDEF,
  C_ELIF,
  C_ELSE,
  C_ENDIF
} CondKind;

static int condKindOf(const char *text, size_t len, CondKind *kind)
{
  static const char *const names[] = { "if", "ifdef", "ifndef", "elif", "else", "endif" };
  for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
    if (equalsNoCase(text, len, names[i])) {
      *kind = (CondKind)i;
      return 1;
    }
  }
  return 0;
}

// is the head token a .if, .ifdef, .ifndef, .elif, .else or .endif
static int asmCondKind(const Reader *r, const Token *head, CondKind *kind)
{
  size_t len;
  const char *text = tokText(r, head, &len);
  return len > 1 && text[0] == '.' && condKindOf(text + 1, len - 1, kind);
}

// the value of the condition after .if, .elif, #if or #elif. returns 0 after reporting an error.
static int condValue(Reader *r, TokenLine *line, uint32_t from, int pp, int64_t *value)
{
  rewriteDefined(r, line, from, pp);
  // the head and what is before it are not touched, only the expr
  if (r->defines != NULL) {
    TokVec out = { NULL, 0, 0 };
    const Define *hide[1];
    for (uint32_t i = 0; i < from; i++) vecPush(&r->ctx->arena, &out, line->tokens[i]);
    if (!expandTokens(r, line->tokens + from, line->count - 1 - from, &out, hide, 0)) return 0;
    vecPush(&r->ctx->arena, &out, line->tokens[line->count - 1]);
    line->tokens = out.v;
    line->count = out.n;
  }
  TokenCursor cur;
  cursorInit(&cur, line);
  cur.pos = from;
  if (isEnd(cursorPeek(&cur))) {
    diagEmit(&r->ctx->diags, DIAG_ERROR, cursorPeek(&cur)->span, "a condition is missing");
    return 0;
  }
  if (pp) {
    ExprEnv env = { &r->ctx->sources, &r->ctx->diags, NULL, NULL, 0, 0, NULL, 0 };
    ExprValue v;
    if (!exprEval(&cur, &env, &v)) return 0;
    *value = v.value;
  } else if (!r->hooks.eval(r->hooks.user, &cur, value)) {
    return 0;
  }
  return expectEnd(r, line, cur.pos, "the condition");
}

// .ifdef NAME and #ifdef NAME, sets *value to 1 or 0. returns 0 on errors.
static int nameDefined(Reader *r, const TokenLine *line, uint32_t from, int pp, int *value)
{
  const Token *t = &line->tokens[from < line->count ? from : line->count - 1];
  if (t->kind != TOK_IDENT) {
    if (t->kind != TOK_ERROR) diagEmit(&r->ctx->diags, DIAG_ERROR, t->span, "a name is expected here");
    return 0;
  }
  if (!expectEnd(r, line, from + 1, "the name")) return 0;
  size_t len;
  const char *name = tokText(r, t, &len);
  if (!pp && findDefine(r, name, len) != NULL) {
    diagEmit(&r->ctx->diags, DIAG_ERROR, t->span, 
      "'%.*s' is a #define name, which is replaced before the assembler sees it: use #ifdef", (int)len, name);
    return 0;
  }
  if (pp) {
    *value = findDefine(r, name, len) != NULL;
  } else if (readerHasMacro(r, name, len)) {
    diagEmit(&r->ctx->diags, DIAG_ERROR, t->span, 
      "'%.*s' is a macro, not a symbol", (int)len, name);
    return 0;
  } else {
    *value = r->hooks.isDefined(r->hooks.user, name, len);
  }
  return 1;
}

static void doCond(Reader *r, const Frame *f, TokenLine *line, uint32_t hi, CondKind kind, int pp)
{
  Cond *c = r->conds;
  const Token head = line->tokens[hi];
  uint32_t from = hi + 1;
  size_t hlen;
  const char *htext = tokText(r, &head, &hlen);
  int inFrame = r->conds != f->condBase;

  if (kind == C_IF || kind == C_IFDEF || kind == C_IFNDEF ) {
    if (hi > 0 && !pp) {
      diagEmit(&r->ctx->diags, DIAG_ERROR, line->tokens[0].span, 
        "a label cannot be put before %.*s", (int)hlen, htext);
    }
    Cond *n = arenaAlloc(&r->ctx->arena, sizeof *n, _Alignof(Cond));
    memset(n, 0, sizeof *n);
    n->pp = pp;
    n->parentActive = c == NULL || c->active;
    n->parentPpActive = c == NULL || c->ppActive;
    n->opened = head.span;
    int value = 0;
    if (pp ? n->parentPpActive : n->parentActive) {
      int64_t v = 0;
      int d = 0, ok;
      if (kind == C_IF) {
        ok = condValue(r, line, from, pp, &v);
        value = ok && v != 0;
      } else {
        ok = nameDefined(r, line, from, pp, &d);
        value = ok && (kind == C_IFDEF ? d : !d);
      }
    }
    n->active = n->parentActive && value;
    n->ppActive = pp ? n->parentPpActive && value : n->parentPpActive;
    n->taken = value;
    n->up = r->conds;
    r->conds = n;
    return;
  }

  if (!inFrame || c->pp != pp) {
    diagEmit(&r->ctx->diags, DIAG_ERROR, head.span, "unexpected %.*s%s", (int)hlen, htext, 
      inFrame ? ": it does not match the conditional that is open" : ": there is not conditional open");
    return;
  }

  if (kind == C_ENDIF) {
    if (hi > 0 && !pp) diagEmit(&r->ctx->diags, DIAG_ERROR, line->tokens[0].span, 
      "a label cannot be put before .endif");
    expectEnd(r, line, from, ".endif");
    r->conds = c->up;
    return;
  }

  if (c->sawElse) {
    diagEmit(&r->ctx->diags, DIAG_ERROR, head.span, 
      "unexpected %.*s: there was an else already", (int)hlen, htext);
    // what follows is skipped, a # conditional also stops the # lines
    c->active = 0;
    if (pp) c->ppActive = 0;
    return;
  }

  int value = 0;
  if (kind == C_ELSE) {
    expectEnd(r, line, from, "else");
    c->sawElse = 1;
    value = !c->taken;
  } else if (!c->taken && (pp ? c->parentPpActive : c->parentActive)) {
    // .elif, only looked at if it can matter
    int64_t v;
    value = condValue(r, line, from, pp, &v) && v != 0;
  }
  c->active = c->parentActive && value;
  c->ppActive = pp ? c->parentPpActive && value : c->parentPpActive;
  if (value) c->taken = 1;
}

// ------- messages, includes, #define and other # lines -----------------

static void doMessage(Reader *r, const TokenLine *line, uint32_t from, DiagSeverity sev, const char *directive)
{
  const Token *s = &line->tokens[from < line->count ? from : line->count - 1];
  if (s->kind != TOK_STRING) {
    if (s->kind != TOK_ERROR) diagEmit(&r->ctx->diags, DIAG_ERROR, s->span,
      "%s needs a message in quotes", directive);
    return;
  }
  if (!expectEnd(r, line, from + 1, "the message")) return;
  size_t len;
  const char *text = tokText(r, s, &len);
  diagEmit(&r->ctx->diags, sev, s->span, "%.*s", (int)(len - 2), text + 1);
}

static const char *joinPath(Reader *r, const char *dir, size_t dirLen, int needSlash, const char *name)
{
  size_t nlen = strlen(name);
  char *p = arenaAlloc(&r->ctx->arena, dirLen + 1 + nlen + 1, 1);
  memcpy(p, dir, dirLen);
  size_t at = dirLen;
  if (needSlash) p[at++] = '/';
  memcpy(p + at, name, nlen + 1);
  return p;
}

static int isAbsolute(const char *path)
{
  char c = (char)(path[0] | 0x20);
  return path[0] == '/' || (c >= 'a' && c <= 'z' && path[1] == ':' && path[2] == '/');
}

static void pushFile(Reader *r, FileId id)
{
  Frame *f = arenaAlloc(&r->ctx->arena, sizeof *f, _Alignof(Frame));
  memset(f, 0, sizeof *f);
  f->kind = FRAME_FILE;
  f->up = r->top;
  f->condBase = r->conds;
  f->callSite.file = FILE_NONE;
  lexerInit(&f->lx, &r->ctx->sources, id, &r->ctx->diags);
  r->top = f;
  r->fileDepth++;
}

// .include "name" and #include "name", 'from' is the first token after the directive
static void doInclude(Reader *r, const Frame *f, const TokenLine *line, uint32_t from)
{
  const Token *t = &line->tokens[from < line->count ? from : line->count - 1];
  const char *name;
  size_t nameLen;
  int angle = 0;
  uint32_t last = from;
  Span at = t->span;
  if (t->kind == TOK_STRING) {
    size_t len;
    name = tokText(r, t, &len) + 1;
    nameLen = len - 2;
  } else if (t->kind == TOK_LT) {
    // <name>: the text between the brackets, as it was written
    uint32_t k = from + 1;
    while (!isEnd(&line->tokens[k]) && line->tokens[k].kind != TOK_GT) k++;
    if (line->tokens[k].kind != TOK_GT || k == from + 1 || line->tokens[k - 1].span.file != t->span.file) {
      diagEmit(&r->ctx->diags, DIAG_ERROR, t->span, "the file name between < and > is missing or not closed");
      return;
    }
    const SourceFile *sf = sourceMapGet(&r->ctx->sources, t->span.file);
    name = sf->text + t->span.end;
    nameLen = line->tokens[k].span.start - t->span.end;
    angle = 1;
    last = k;
    at.end = line->tokens[k].span.end;
  } else {
    if (t->kind != TOK_ERROR) diagEmit(&r->ctx->diags, DIAG_ERROR, t->span,
      "an include needs a file name in quotes");
    return;
  }
  if (!expectEnd(r, line, last + 1, "the file name")) return;
  if (nameLen == 0) {
    diagEmit(&r->ctx->diags, DIAG_ERROR, at, "the file name is empty");
    return;
  }
  if (r->fileDepth >= READER_MAX_INCLUDE_DEPTH) {
    diagEmit(&r->ctx->diags, DIAG_ERROR, at,
      "includes are nested more than %d deep (does a file include itself?)", READER_MAX_INCLUDE_DEPTH);
    return;
  }
  // '\' is a separator too
  char *clean = arenaAlloc(&r->ctx->arena, nameLen + 1, 1);
  for (size_t i = 0; i < nameLen; i++) clean[i] = name[i] == '\\' ? '/' : name[i];
  clean[nameLen] = '\0';

  FileId id = FILE_NONE;
  int absolute = isAbsolute(clean);
  if (absolute) id = ctxLoadQuiet(r->ctx, clean);
  if (!angle && !absolute) id = ctxLoadQuiet(r->ctx, joinPath(r, r->baseDir, strlen(r->baseDir), 0, clean));
  for (uint32_t i = 0; id == FILE_NONE && !absolute && i < r->dirCount; i++) {
    size_t dl = strlen(r->dirs[i]);
    int slash = dl != 0 && r->dirs[i][dl - 1] != '/' && r->dirs[i][dl - 1] != '\\';
    id = ctxLoadQuiet(r->ctx, joinPath(r, r->dirs[i], dl, slash, clean));
  }
  if (id == FILE_NONE) {
    diagEmit(&r->ctx->diags, DIAG_ERROR, at, "cannot find the include file '%s' (looked %s)", clean,
      absolute ? "only there: the path is absolute"
      : angle ? "in the -I directories" : "next to the main file, then in the -I directories");
    if (f->up != NULL) {
      diagEmit(&r->ctx->diags, DIAG_INFO, at, 
        "a path is relative to the main file, not to the file that includes");
    }
    return;
  }
  pushFile(r, id);
}

// #define NAME body  and  #define NAME(a, b) body
static void doDefine(Reader *r, const TokenLine *line)
{
  uint32_t n = line->count - 1;
  if (n < 3 || line->tokens[2].kind != TOK_IDENT) {
    diagEmit(&r->ctx->diags, DIAG_ERROR, line->tokens[n < 3 ? n : 2].span, "#define needs a name");
    return;
  }
  Define *d = arenaAlloc(&r->ctx->arena, sizeof *d, _Alignof(Define));
  memset(d, 0, sizeof *d);
  const Token *name = &line->tokens[2];
  d->name = tokText(r, name, &d->len);
  d->defined = name->span;
  uint32_t i = 3;
  if (i < n && line->tokens[i].kind == TOK_LPAREN && line->tokens[i].span.file == name->span.file &&
      line->tokens[i].span.start == name->span.end) {
    // '(' right after the name implies parameters
    d->isFunc = 1;
    i++;
    if (i < n && line->tokens[i].kind == TOK_RPAREN) {
      i++;
    } else {
      while(1) {
        if (i >= n || line->tokens[i].kind != TOK_IDENT || d->paramCount == READER_MAX_PARAMS) {
          diagEmit(&r->ctx->diags, DIAG_ERROR, line->tokens[i < n ? i : n].span, 
            "a parameter name is expected here");
          return;
        }
        d->params[d->paramCount++] = line->tokens[i++];
        if (i < n && line->tokens[i].kind == TOK_COMMA) { i++; continue; }
        if (i < n && line->tokens[i].kind == TOK_RPAREN) { i++; break; }
        diagEmit(&r->ctx->diags, DIAG_ERROR, line->tokens[i < n ? i : n].span, 
          "expected , or ) in the parameters");
        return;
      }
    }
  }
  d->bodyCount = n - i;
  d->body = arenaAlloc(&r->ctx->arena, (d->bodyCount + 1) * sizeof *d->body, _Alignof(Token));
  memcpy(d->body, line->tokens + i, d->bodyCount * sizeof *d->body);

  for (Define **p = &r->defines; *p != NULL; p = &(*p)->next) {
    if ((*p)->len == d->len && memcmp((*p)->name, d->name, d->len) == 0) {
      diagEmit(&r->ctx->diags, DIAG_WARNING, name->span, "'%.*s' is defined again", (int)d->len, d->name);
      diagEmit(&r->ctx->diags, DIAG_INFO, (*p)->defined, "the earlier definition is here");
      *p = (*p)->next;
      break;
    }
  }
  d->next = r->defines;
  r->defines = d;
}

static void doUndef(Reader *r, const TokenLine *line)
{
  const Token *t = &line->tokens[2 < line->count ? 2 : line->count - 1];
  if (t->kind != TOK_IDENT) {
    if (t->kind != TOK_ERROR) diagEmit(&r->ctx->diags, DIAG_ERROR, t->span, "#undef needs a name");
    return;
  }
  if (!expectEnd(r, line, 3, "the name")) return;
  size_t len;
  const char *name = tokText(r, t, &len);
  for (Define **p = &r->defines; *p != NULL; p = &(*p)->next) {
    if ((*p)->len == len && memcmp((*p)->name, name, len) == 0) {
      *p = (*p)->next;
      return;
    }
  }
}

// #pragma AVRPART ...: the rest of the line goes to the assembler (the device file's sizes and core).
static void passPragma(Reader *r, const TokenLine *line, uint32_t from)
{
  if (r->hooks.pragma == NULL) return;
  uint32_t last = line->count - 1;
  if (from >= last) {
    r->hooks.pragma(r->hooks.user, line->tokens[last].span, "", 0);
    return;
  }
  Span at = line->tokens[from].span;
  at.end = line->tokens[last - 1].span.end;
  r->hooks.pragma(r->hooks.user, at, sourceMapGet(&r->ctx->sources, at.file)->text + at.start, at.end - at.start);
}

// a #define written as 'spec' (NAME or NAME=value, as -D takes it), in a source of its own called 'origin'.
static int defineFrom(Reader *r, const char *origin, const char *spec)
{
  const char *eq = strchr(spec, '=');
  size_t nameLen = eq != NULL ? (size_t)(eq - spec) : strlen(spec);
  const char *value = eq != NULL ? eq + 1 : "1";
  size_t total = 8 + nameLen + 1 + strlen(value) + 2;
  char *text = arenaAlloc(&r->ctx->arena, total, 1);
  memcpy(text, "#define ", 8);
  memcpy(text + 8, spec, nameLen);
  text[8 + nameLen] = ' ';
  strcpy(text + 9 + nameLen, value);
  strcat(text, "\n");
  FileId id = sourceMapAdd(&r->ctx->sources, origin, text, strlen(text));
  Lexer lx;
  lexerInit(&lx, &r->ctx->sources, id, &r->ctx->diags);
  uint32_t errors = r->ctx->diags.errorCount;
  TokenLine line = lexLine(&lx, &r->ctx->arena);
  if (line.count >= 3 && line.tokens[0].kind == TOK_HASH) doDefine(r, &line);
  else diagEmit(&r->ctx->diags, DIAG_ERROR, line.tokens[0].span, "-D needs NAME or NAME=value");
  return r->ctx->diags.errorCount == errors;
}

// .device NAME also defines __PART_NAME__ (the name as a string: .db __PART_NAME__ gives its characters)
// and __NAME__ (1) as AVRASM2 does. only the first .device does, another name is the assemblers error
// and the same name again changes nothing
static void deviceDefines(Reader *r, const TokenLine *line, uint32_t from)
{
  const Token *name = &line->tokens[from < line->count - 1 ? from : line->count - 1];
  if (name->kind != TOK_IDENT || findDefine(r, "__PART_NAME__", 13) != NULL) return;
  size_t len;
  const char *text = tokText(r, name, &len);
  if (len > 31) return;  // the assembler says the name is too long
  char spec[80];
  snprintf(spec, sizeof spec, "__PART_NAME__=\"%.*s\"", (int)len, text);
  defineFrom(r, "<.device>", spec);
  snprintf(spec, sizeof spec, "__%.*s__", (int)len, text);
  defineFrom(r, "<.device>", spec);
}

// A # line. when the preprocessor is not reading ('live' is 0) only the conditionals count
static void doPP(Reader *r, Frame *f, TokenLine *line, int live)
{
  const Token *h = &line->tokens[1];
  if (h->kind != TOK_IDENT) {
    if (live && h->kind != TOK_ERROR) diagEmit(&r->ctx->diags, DIAG_ERROR, h->span,
      "a preprocessor directive is expected after #");
    return;
  }
  size_t len;
  const char *name = tokText(r, h, &len);
  CondKind kind;
  int isCond = 0;
  for (const char *p = name; p < name + len; p++) {
    // directive names are in lower case
    if (*p >= 'A' && *p <= 'Z') { isCond = -1; break; }
  }
  if (isCond == 0 && condKindOf(name, len, &kind)) {
    doCond(r, f, line, 1, kind, 1);
    return;
  }
  if (!live) return;
  if (len == 6 && memcmp(name, "define", 6) == 0) doDefine(r, line);
  else if (len == 5 && memcmp(name, "undef", 5) == 0) doUndef(r, line);
  else if (len == 7 && memcmp(name, "include", 7) == 0) doInclude(r, f, line, 2);
  else if (len == 5 && memcmp(name, "error", 5) == 0) doMessage(r, line, 2, DIAG_ERROR, "#error");
  else if (len == 7 && memcmp(name, "warning", 7) == 0) doMessage(r, line, 2, DIAG_WARNING, "#warning");
  else if (len == 7 && memcmp(name, "message", 7) == 0) doMessage(r, line, 2, DIAG_INFO, "#message");
  else if (len == 6 && memcmp(name, "pragma", 6) == 0) {
    const Token *p = &line->tokens[2 < line->count ? 2 : line->count - 1];
    size_t plen = 0;
    const char *ptext = p->kind == TOK_IDENT ? tokText(r, p, &plen) : "";
    if (plen == 7 && memcmp(ptext, "AVRPART", 7) == 0) {
      passPragma(r, line, 3);
    } else if (!(plen == 7 && memcmp(ptext, "partinc", 7) == 0)) {
      diagEmit(&r->ctx->diags, DIAG_WARNING, p->span, "this #pragma is not known and is ignored");
    }
  } else {
    diagEmit(&r->ctx->diags, DIAG_ERROR, h->span, "'#%.*s' is not a preprocessor directive", (int)len, name);
  }
}

// ------- macros --------------------------------------------------------

static const Macro *findMacro(const Reader *r, const char *name, size_t len)
{
  for (const Macro *m = r->macros; m != NULL; m = m->next) {
    if (sameNoCase(m->name, m->len, name, len)) return m;
  }
  return NULL;
}

int readerHasMacro(const Reader *r, const char *name, size_t len)
{
  return findMacro(r, name, len) != NULL;
}

void readerOutline(const Reader *r, Outline *o)
{
  for (const Macro *m = r->macros; m != NULL; m = m->next) {
    outlineAddName(o, OUTLINE_MACRO, m->name, m->len, m->argc, 0, m->defined);
  }
  for (const Define *d = r->defines; d != NULL; d = d->next) {
    outlineAddName(o, OUTLINE_DEFINE, d->name, d->len, 0, 0, d->defined);
  }
}

static void beginMacro(Reader *r, Frame *f, const TokenLine *line, uint32_t hi)
{
  const Token *name = &line->tokens[hi + 1 < line->count ? hi + 1 : line->count - 1];
  if (hi > 0) diagEmit(&r->ctx->diags, DIAG_ERROR, line->tokens[0].span, 
    "a label cannot be put before .macro");
  Macro *m = arenaAlloc(&r->ctx->arena, sizeof *m, _Alignof(Macro));
  memset(m, 0, sizeof *m);
  m->defined = line->tokens[hi].span;
  if (name->kind != TOK_IDENT) {
    if (name->kind != TOK_ERROR) diagEmit(&r->ctx->diags, DIAG_ERROR, name->span, ".macro needs a name");
    m->name = "";
  } else {
    m->name = tokText(r, name, &m->len);
    const Macro *old = findMacro(r, m->name, m->len);
    if (old != NULL) {
      diagEmit(&r->ctx->diags, DIAG_ERROR, name->span, "macro '%.*s' is defined already", (int)m->len, m->name);
      diagEmit(&r->ctx->diags, DIAG_INFO, old->defined, "the first definition is here");
    } else {
      r->hooks.nameOk(r->hooks.user, name);
    }
    expectEnd(r, line, hi + 2, "the macro name");
  }
  r->building = m;
  r->buildFrame = f;
  r->bodyCount = 0;
  r->maxArg = 0;
}

static void endMacro(Reader *r)
{
  Macro *m = r->building;
  m->lineCount = r->bodyCount;
  m->lines = arenaAlloc(&r->ctx->arena, (m->lineCount + 1) * sizeof *m->lines, _Alignof(TokenLine));
  if (m->lineCount != 0) memcpy(m->lines, r->body, m->lineCount * sizeof *m->lines);
  m->argc = r->maxArg;
  if (m->len != 0 && findMacro(r, m->name, m->len) == NULL) {
    m->next = r->macros;
    r->macros = m;
  }
  r->building = NULL;
}

// checks if an identifier is an argument @0 to @9. stores its number in *index.
// an identifier with an @ anywhere else is reported
static int isArgument(Reader *r, const Token *t, uint32_t *index, int report)
{
  size_t len;
  const char *text = tokText(r, t, &len);
  if (memchr(text, '@', len) == NULL) return 0;
  if (len == 2 && text[0] == '@' && text[1] >= '0' && text[1] <= '9') {
    *index = (uint32_t)(text[1] - '0');
    return 1;
  }
  if (report) {
    diagEmit(&r->ctx->diags, DIAG_ERROR, t->span,
      len > 2 && text[0] == '@' ? "'%.*s' is not an argument: a macro has the arguments @0 to @9"
      : "'%.*s' is not an argument: @0 to @9 must stand alone, not inside a name", (int)len, text);
  }
  return 0;
}

// a line of the body of a macro that is being defined is kept and notes the arguments it uses
static void addBodyLine(Reader *r, const TokenLine *line)
{
  for (uint32_t i = 0; i + 1 < line->count; i++) {
    uint32_t index;
    if (line->tokens[i].kind == TOK_IDENT && isArgument(r, &line->tokens[i], &index, 1) && index + 1 > r->maxArg) {
      r->maxArg = index + 1;
    }
  }
  growBody(r, *line);
}

// the line 'body' with each @N replaced by the tokens of argument N of the call in frame f
static TokenLine expandBodyLine(Reader *r, const Frame *f, const TokenLine *body)
{
  TokVec out = { NULL, 0, 0 };
  for (uint32_t i = 0; i + 1 < body->count; i++) {
    uint32_t index;
    if (body->tokens[i].kind == TOK_IDENT && isArgument(r, &body->tokens[i], &index, 0) && index < f->args) {
      for (uint32_t k = 0; k < f->argCount[index]; k++)
        vecPush(&r->ctx->arena, &out, f->call.tokens[f->argStart[index] + k]);
    } else {
      vecPush(&r->ctx->arena, &out, body->tokens[i]);
    }
  }
  vecPush(&r->ctx->arena, &out, body->tokens[body->count - 1]);
  TokenLine line = { out.v, out.n };
  return line;
}

// a macro call: 'macro-name arg, arg'. the label in front of it (hi > 0) is handed out first
static void callMacro(Reader *r, Frame *f, const Macro *m, TokenLine *line, uint32_t hi)
{
  const Token *head = &line->tokens[hi];
  if (r->macroDepth >= READER_MAX_MACRO_DEPTH) {
    diagEmit(&r->ctx->diags, DIAG_ERROR, head->span,
      "macros are nested more than %d deep (does a macro call itself?)",
      READER_MAX_MACRO_DEPTH);
    return;
  }
  Frame *n = arenaAlloc(&r->ctx->arena, sizeof *n, _Alignof(Frame));
  memset(n, 0, sizeof *n);
  n->kind = FRAME_MACRO;
  n->macro = m;
  n->call = *line;
  n->callSite = head->span;
  n->scope = ++r->nextScope;
  n->condBase = r->conds;

  // the arguments, split at the commas that are not inside parentheses
  uint32_t start = hi + 1, depth = 0, count = 0;
  int any = !isEnd(&line->tokens[start]);
  for (uint32_t i = start; any; i++) {
    const Token *t = &line->tokens[i];
    int last = isEnd(t);
    if (t->kind == TOK_LPAREN) depth++;
    if (t->kind == TOK_RPAREN && depth > 0) depth--;
    if (last || (t->kind == TOK_COMMA && depth == 0)) {
      if (count < MACRO_MAX_ARGS) {
        n->argStart[count] = start;
        n->argCount[count] = i - start;
      }
      count++;
      start = i + 1;
    }
    if (last) break;
  }
  if (count != m->argc) {
    diagEmit(&r->ctx->diags, DIAG_ERROR, head->span, 
      "macro '%.*s' uses %u argument%s (@0 to @%u), found %u", (int)m->len, m->name,
      m->argc, m->argc == 1 ? "" : "s", m->argc == 0 ? 0 : m->argc - 1, count);
    diagEmit(&r->ctx->diags, DIAG_INFO, m->defined, "the macro is defined here");
    return;
  }
  n->args = count;
  if (hi > 0) {
    // the label goes in front of the first line of the expansion
    Token *lab = arenaAlloc(&r->ctx->arena, 3 * sizeof *lab, _Alignof(Token));
    lab[0] = line->tokens[0];
    lab[1] = line->tokens[1];
    lab[2] = line->tokens[line->count - 1];
    lab[2].kind = TOK_NEWLINE;
    r->pending = 1;
    r->pendingLine.tokens = lab;
    r->pendingLine.count = 3;
    r->pendingScope = f->scope;
    r->pendingCall = f->callSite;
  }
  n->up = r->top;
  r->top = n;
  r->macroDepth++;
}

// ------- frames --------------------------------------------------------

static void closeConds(Reader *r, const Frame *f, int report)
{
  while (r->conds != f->condBase) {
    if (report) {
      diagEmit(&r->ctx->diags, DIAG_ERROR, r->conds->opened, "this %s is never closed (%s is missing)",
      r->conds->pp ? "#if" : ".if", r->conds->pp ? "#endif" : ".endif");
    }
    r->conds = r->conds->up;
  }
}

static void endFrame(Reader *r, Frame *f)
{
  if (r->building != NULL && r->buildFrame == f) {
    diagEmit(&r->ctx->diags, DIAG_ERROR, r->building->defined, "this macro has no .endm before the end of %s",
      f->kind == FRAME_FILE ? "the file" : "the macro it is in");
    r->building = NULL;
  }
  closeConds(r, f, !f->exited);
  if (f->kind == FRAME_FILE) r->fileDepth--;
  else r->macroDepth--;
  r->top = f->up;
}

// ------- main loop -----------------------------------------------------

// the line after the # and name handling, decides what to do with one line of a file or macro.
// returns 1 if 'out' has a line to hand to the assembler
static int handleLine(Reader *r, Frame *f, TokenLine *line, ReaderLine *out)
{
  if (isEnd(&line->tokens[0])) return 0;
  const Cond *top = r->conds;
  int ppLive = top == NULL || top->ppActive;
  int asmLive = top == NULL || top->active;

  if (line->tokens[0].kind == TOK_HASH) {
    doPP(r, f, line, ppLive);
    return 0;
  }
  if (!ppLive) return 0;

  uint32_t hi = headIndex(line);
  CondKind ck;
  int isCond = line->tokens[hi].kind == TOK_IDENT && asmCondKind(r, &line->tokens[hi], &ck);
  if (!asmLive) {
    if (isCond) doCond(r, f, line, hi, ck, 0);
    return 0;
  }
  if (!isCond && f->kind == FRAME_FILE) {
    // a macro's lines were replaced when they were read
    ppReplace(r, line);
    hi = headIndex(line);
  }

  const Token *head = &line->tokens[hi];
  size_t len = 0;
  const char *text = head->kind == TOK_IDENT ? tokText(r, head, &len) : "";

  if (r->building != NULL) {
    if (equalsNoCase(text, len, ".endm") || equalsNoCase(text, len, ".endmacro")) {
      if (hi > 0) diagEmit(&r->ctx->diags, DIAG_ERROR, line->tokens[0].span, 
        "a label cannot be put before .endm");
      endMacro(r);
    } else if (equalsNoCase(text, len, ".macro") || equalsNoCase(text, len, ".include")) {
      diagEmit(&r->ctx->diags, DIAG_ERROR, head->span, 
        "%.*s is not allowed inside a macro", (int)len, text);
    } else {
      addBodyLine(r, line);
    }
    return 0;
  }
  if (isCond) {
    doCond(r, f, line, hi, ck, 0);
    return 0;
  }
  if (head->kind == TOK_IDENT && equalsNoCase(text, len, ".device")) deviceDefines(r, line, hi + 1);
  if (head->kind == TOK_IDENT) {
    int dir = 1;
    if (equalsNoCase(text, len, ".macro")) {
      beginMacro(r, f, line, hi);
    } else if (equalsNoCase(text, len, ".endm") || equalsNoCase(text, len, ".endmacro")) {
      diagEmit(&r->ctx->diags, DIAG_ERROR, head->span, 
        "unexpected %.*s: there is no .macro open", (int)len, text);
    } else if (equalsNoCase(text, len, ".include")) {
      if (hi > 0) {
        diagEmit(&r->ctx->diags, DIAG_ERROR, line->tokens[0].span,
          "a label cannot be put before .include");
      } else {
        doInclude(r, f, line, hi + 1);
      }
    } else if (equalsNoCase(text, len, ".exit")) {
      if (f->kind != FRAME_FILE) diagEmit(&r->ctx->diags, DIAG_ERROR, head->span, 
        ".exit is not allowed inside a macro");
      else if (expectEnd(r, line, hi + 1, ".exit")) f->done = f->exited = 1;
    } else if (equalsNoCase(text, len, ".message")) {
      doMessage(r, line, hi + 1, DIAG_INFO, ".message");
    } else if (equalsNoCase(text, len, ".warning")) {
      doMessage(r, line, hi + 1, DIAG_WARNING, ".warning");
    } else if (equalsNoCase(text, len, ".error")) {
      doMessage(r, line, hi + 1, DIAG_ERROR, ".error");
    } else {
      dir = 0;
    }
    if (dir) return 0;

    const Macro *m = findMacro(r, text, len);
    if (m != NULL) {
      callMacro(r, f, m, line, hi);
      return 0;
    }
  }

  for (uint32_t i = 0; i + 1 < line->count; i++) {
    // an @0 outside a macro body
    if (line->tokens[i].kind == TOK_IDENT) {
      size_t l2;
      const char *t2 = tokText(r, &line->tokens[i], &l2);
      if (memchr(t2, '@', l2) != NULL) {
        diagEmit(&r->ctx->diags, DIAG_ERROR, line->tokens[i].span, 
          "'%.*s': @ arguments only exist inside a macro", (int)l2, t2);
        return 0;
      }
    }
  }
  out->line = *line;
  out->scope = f->scope;
  out->callSite = f->callSite;
  return 1;
}

int readerNext(Reader *r, ReaderLine *out)
{
  Arena *arena = &r->ctx->arena;
  if (r->pending) {
    r->pending = 0;
    out->line = r->pendingLine;
    out->scope = r->pendingScope;
    out->callSite = r->pendingCall;
    return 1;
  }
  while (1) {
    Frame *f = r->top;
    if (f == NULL) return 0;
    TokenLine line;
    if (f->kind == FRAME_FILE) {
      if (f->done) {
        endFrame(r, f);
        continue;
      }
      int dead = r->conds != NULL && !(r->conds->ppActive && r->conds->active);
      DiagList *real = f->lx.diags;
      // what is not assembled may contain anything
      if (dead) f->lx.diags = &r->scratch;
      line = lexLine(&f->lx, arena);
      f->lx.diags = real;
      if (line.tokens[line.count - 1].kind == TOK_EOF) f->done = 1;
    } else {
      if (f->next >= f->macro->lineCount) {
        endFrame(r, f);
        continue;
      }
      line = expandBodyLine(r, f, &f->macro->lines[f->next++]);
    }
    if (handleLine(r, f, &line, out)) return 1;
    if (r->pending) {
      r->pending = 0;
      out->line = r->pendingLine;
      out->scope = r->pendingScope;
      out->callSite = r->pendingCall;
      return 1;
    }
  }
}

Reader *readerCreate(AvrCtx *ctx, const ReaderHooks *hooks, FileId main,
  const char *const *includeDirs, uint32_t dirCount)
{
  Reader *r = arenaAlloc(&ctx->arena, sizeof *r, _Alignof(Reader));
  memset(r, 0, sizeof *r);
  r->ctx = ctx;
  r->hooks = *hooks;
  diagListInit(&r->scratch, &ctx->arena);

  const SourceFile *sf = sourceMapGet(&ctx->sources, main);
  size_t dirLen = 0;
  for (size_t i = 0; sf != NULL && sf->path[i] != '\0'; i++) {
    if (sf->path[i] == '/' || sf->path[i] == '\\') dirLen = i + 1;
  }
  char *base = arenaAlloc(&ctx->arena, dirLen + 1, 1);
  for (size_t i = 0; i < dirLen; i++) base[i] = sf->path[i] == '\\' ? '/' : sf->path[i];
  base[dirLen] = '\0';
  r->baseDir = base;

  const char **dirs = arenaAlloc(&ctx->arena, (dirCount + 1) * sizeof *dirs, _Alignof(const char *));
  for (uint32_t i = 0; i < dirCount; i++) dirs[i] = arenaStrdup(&ctx->arena, includeDirs[i]);
  r->dirs = dirs;
  r->dirCount = dirCount;

  pushFile(r, main);
  return r;
}

int readerDefine(Reader *r, const char *spec)
{
  return defineFrom(r, "<command line>", spec);
}