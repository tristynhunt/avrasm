#include "avrasm/lexer.h"
#include "avrasm/diag.h"
#include "avrasm/source.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int isDigit(char c)
{
  return c >= '0' && c <= '9';
}

static int isAlnum(char c)
{
  return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int isIdentStart(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '@';
}

static int isIdentChar(char c)
{
  return isDigit(c) || isIdentStart(c);
}

static int isLineEnd(char c)
{
  return c == '\n' || c == '\r';
}

static int isBlank(char c)
{
  return c == ' ' || c == '\t' || c == '\f' || c == '\v';
}

static int digitValue(char c)
{
  if (isDigit(c)) return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 99;
}

void lexerInit(Lexer *lx, const SourceMap *map, FileId id, DiagList *diags)
{
  lx->file = sourceMapGet(map, id);
  lx->id = id;
  lx->diags = diags;
  lx->pos = 0;
}

static Token make(const Lexer *lx, TokenKind kind, uint32_t start)
{
  Span span = { lx->id, start, lx->pos };
  Token tok = { kind, span, 0, 0 };
  return tok;
}

// reports an error over [start, lx->pos) and returns the TOK_ERROR token for it.
// so set lx->pos BEFORE calling it
static Token fail(Lexer *lx, uint32_t start, const char *fmt, ...) AVRASM_PRINTF(3, 4);
static Token fail(Lexer *lx, uint32_t start, const char *fmt, ...)
{
  Span span = { lx->id, start, lx->pos };

  va_list ap;
  va_start(ap, fmt);
  diagEmitV(lx->diags, DIAG_ERROR, span, fmt, ap);
  va_end(ap);
  
  return make(lx, TOK_ERROR, start);
}

static void skipToLineEnd(Lexer *lx)
{
  const char *text = lx->file->text;  
  while (lx->pos < lx->file->len && !isLineEnd(text[lx->pos])) lx->pos++;
}

static Token lexNumber(Lexer *lx)
{
  const char *text = lx->file->text;
  uint32_t len = lx->file->len;
  uint32_t start = lx->pos, p = start;
  int radix = 10;
  const char *radixName = "decimal";
  if (text[p] == '$') {
    radix = 16;
    radixName = "hexadecimal";
    p++;
  } else if (text[p] == '0' && (text[p + 1] == 'x' || text[p + 1] == 'X')) {
    radix = 16;
    radixName = "hexadecimal";
    p += 2;
  } else if (text[p] == '0' && (text[p + 1] == 'b' || text[p + 1] == 'B')) {
    radix = 2;
    radixName = "binary";
    p += 2;
  }

  uint32_t wordStart = p;
  while (p < len && (isAlnum(text[p]) || text[p] == '_')) p++;

  if (radix == 10) {
    int allDigits = 1;
    for (uint32_t i = wordStart; i < p; i++) {
      if (!isDigit(text[i])) allDigits = 0;
    }
    // check for float (dec only)
    if (allDigits && text[p] == '.' && isDigit(text[p + 1])) {
      p++;
      while (p < len && isDigit(text[p])) p++;
      char buf[128];
      if (p - start >= sizeof buf) {
        lx->pos = p;
        return fail(lx, start, "floating point literal is too long");
      }
      memcpy(buf, text + start, p - start);
      buf[p - start] = '\0';
      lx->pos = p;
      Token t = make(lx, TOK_FLOAT, start);
      t.fvalue = strtod(buf, NULL);
      return t;
    }

    if (text[wordStart] == '0' && p - wordStart >= 2) {
      radix = 8;
      radixName = "octal";
    }
  }

  uint64_t val = 0;
  int digits = 0;
  for (uint32_t i = wordStart; i < p; i++) {
    if (text[i] == '_') continue;
    int d = digitValue(text[i]);
    
    if (d >= radix) {
      lx->pos = p;
      return fail(lx, start, "invalid digit '%c' in %s literal", text[i], radixName);
    }
    if (val > (UINT64_MAX - (uint64_t)d) / (uint64_t)radix) {
      lx->pos = p;
      return fail(lx, start, "number is too large");
    }

    val = val * (uint64_t)radix + (uint64_t)d;
    digits++;
  }
  lx->pos = p;
  if (digits == 0) return fail(lx, start, "expected digits in %s literal", radixName);
  Token t = make(lx, TOK_NUMBER, start);
  t.value = val;
  return t;
}

// lx->pos is on the backslash. on success the byte is stored in *out,
// on failer it is reported, pos is left after what was read
static int lexEscape(Lexer *lx, uint64_t *out)
{
  const char *text = lx->file->text;
  uint32_t len = lx->file->len;
  uint32_t start = lx->pos, p = start + 1;
  if (p >= len || isLineEnd(text[p])) {
    lx->pos = p;
    fail(lx, start, "undetermined escape sequence");
    return 0;
  }

  char e = text[p++];
  uint64_t v = 0;
  switch (e) {
    case 'a': v = 7; break;
    case 'b': v = 8; break;
    case 't': v = 9; break;
    case 'n': v = 10; break;
    case 'v': v = 11; break;
    case 'f': v = 12; break;
    case 'r': v = 13; break;
    case '\\': v = 92; break;
    case 'x': {
      int n = 0;
      while (n < 2 && p < len && digitValue(text[p]) < 16) {
        v = v * 16 + (uint64_t)digitValue(text[p]);
        p++;
        n++;
      }
      if (n == 0) {
        lx->pos = p;
        fail(lx, start, "\\x needs at least one hex digit");
        return 0;
      }
      break;
    }
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7': {
      v = (uint64_t)(e - '0');
      int n = 1;
      while (n < 3 && p < len && text[p] >= '0' && text[p] <= '7') {
        v = v * 8 + (uint64_t)(text[p] - '0');
        p++;
        n++;
      }
      if (v > 255) {
        lx->pos = p;
        fail(lx, start, "octal escape is larger than 255");
        return 0;
      }
      break;
    }
    default: v = (unsigned char)e; break;
  }

  lx->pos = p;
  *out = v;
  return 1;
}

static Token lexChar(Lexer *lx)
{
  const char *text = lx->file->text;
  uint32_t len = lx->file->len;
  uint32_t start = lx->pos;
  lx->pos++;  // the opening quote
  if (lx->pos >= len || isLineEnd(text[lx->pos])) return fail(lx, start, "unterminated character constant");

  if (text[lx->pos] == '\'') {
    lx->pos++;
    return fail(lx, start, "empty character constant");
  }

  uint64_t value;
  if (text[lx->pos] == '\\') {
    if (!lexEscape(lx, &value)) {
      skipToLineEnd(lx);
      return make(lx, TOK_ERROR, start); // escape reported already
    }
  } else {
    value = (unsigned char)text[lx->pos];
    lx->pos++;
  }
  
  if (lx->pos < len && text[lx->pos] == '\'') {
    lx->pos++;
    Token t = make(lx, TOK_CHAR, start);
    t.value = value;
    return t;
  }

  uint32_t q = lx->pos;
  while (q < len && !isLineEnd(text[q]) && text[q] != '\'') q++;
  if (q < len && text[q] == '\'') {
    lx->pos = q + 1;
    return fail(lx, start, "character constant has more than one character");
  }

  lx->pos = q;
  return fail(lx, start, "unterminated character constant");
}

static Token lexString(Lexer *lx)
{
  const char *text = lx->file->text;
  uint32_t len = lx->file->len;
  uint32_t start = lx->pos, p = start + 1;
  while (p < len && !isLineEnd(text[p]) && text[p] != '"') p++;
  if (p < len && text[p] == '"') {
    lx->pos = p + 1;
    return make(lx, TOK_STRING, start);
  }
  lx->pos = p;
  return fail(lx, start, "unterminated string");
}

Token lexerNext(Lexer *lx)
{
  if (lx->file == NULL) return make(lx, TOK_EOF, 0);
  const char *text = lx->file->text;
  uint32_t len = lx->file->len;

  uint32_t start;
  char c;
  while (1) {
    while (lx->pos < len && isBlank(text[lx->pos])) lx->pos++;
    start = lx->pos;
    if (start >= len) return make(lx, TOK_EOF, len);
    c = text[start];

    if (c == '\\') {
      uint32_t p = start + 1;
      if (p < len && text[p] == '\n') {
        lx->pos = p + 1;
        continue;
      }
      if (p < len && text[p] == '\r') {
        p++;
        if (p < len && text[p] == '\n') p++;
        lx->pos = p;
        continue;
      }
      lx->pos++;
      return fail(lx, start, "stray '\\' (a line continuation must end the line)");
    }
    if (c == ';' || (c == '/' && text[start + 1] == '/')) {
      skipToLineEnd(lx);
      continue;
    }
    if (c == '/' && text[start + 1] == '*') {
      uint32_t p = start + 2;
      int hasBreak = 0;
      while (p + 1 < len && !(text[p] == '*' && text[p + 1] == '/')) {
        if (isLineEnd(text[p])) hasBreak = 1;
        p++;
      }
      if (p + 1 >= len) {
        lx->pos = len;
        return fail(lx, start, "unterminated block comment");
      }
      lx->pos = p + 2;
      if (hasBreak) return make(lx, TOK_NEWLINE, start); // whole comment counts as line break
      continue;
    }
    break;
  }

  if (c == '\n') {
    lx->pos++;
    return make(lx, TOK_NEWLINE, start);
  }
  if (c == '\r') {
    lx->pos++;
    if (lx->pos < len && text[lx->pos] == '\n') lx->pos++;
    return make(lx, TOK_NEWLINE, start);
  }
  if (isDigit(c) || c == '$') return lexNumber(lx);
  if (isIdentStart(c)) {
    uint32_t p = start + 1;
    while (p < len && isIdentChar(text[p])) p++;
    lx->pos = p;
    return make(lx, TOK_IDENT, start);
  }
  if (c == '\'') return lexChar(lx);
  if (c == '"') return lexString(lx);

  char n = text[start + 1];
  lx->pos++;
  switch (c) {
    case '(': return make(lx, TOK_LPAREN, start);
    case ')': return make(lx, TOK_RPAREN, start);
    case ',': return make(lx, TOK_COMMA, start);
    case ':': return make(lx, TOK_COLON, start);
    case '?': return make(lx, TOK_QUESTION, start);
    case '#': return make(lx, TOK_HASH, start);
    case '*': return make(lx, TOK_STAR, start);
    case '/': return make(lx, TOK_SLASH, start);
    case '%': return make(lx, TOK_PERCENT, start);
    case '^': return make(lx, TOK_CARET, start);
    case '~': return make(lx, TOK_TILDE, start);
    case '+':
      if (n == '+') { lx->pos++; return make(lx, TOK_PLUSPLUS, start); }
      return make(lx, TOK_PLUS, start);
    case '-':
      if (n == '-') { lx->pos++; return make(lx, TOK_MINUSMINUS, start); }
      return make(lx, TOK_MINUS, start);
    case '<':
      if (n == '<') { lx->pos++; return make(lx, TOK_SHL, start); }
      if (n == '=') { lx->pos++; return make(lx, TOK_LE, start); }
      return make(lx, TOK_LT, start);
    case '>':
      if (n == '>') { lx->pos++; return make(lx, TOK_SHR, start); }
      if (n == '=') { lx->pos++; return make(lx, TOK_GE, start); }
      return make(lx, TOK_GT, start);
    case '=':
      if (n == '=') { lx->pos++; return make(lx, TOK_EQEQ, start); }
      return make(lx, TOK_EQ, start);
    case '!':
      if (n == '=') { lx->pos++; return make(lx, TOK_NE, start); }
      return make(lx, TOK_BANG, start);
    case '&':
      if (n == '&') { lx->pos++; return make(lx, TOK_ANDAND, start); }
      return make(lx, TOK_AMP, start);
    case '|':
      if (n == '|') { lx->pos++; return make(lx, TOK_OROR, start); }
      return make(lx, TOK_PIPE, start);
    default: break;
  }

  // not apart of the language
  if ((unsigned char)c >= 0x80) {
    while (lx->pos < len && (unsigned char)text[lx->pos] >= 0x80) lx->pos++;
    return fail(lx, start, "unexpected non-ASCII character");
  }
  if (c >= 0x20 && c < 0x7F) return fail(lx, start, "unexpected character '%c'", c);
  return fail(lx, start, "unexpected byte 0x%02X", (unsigned)(unsigned char)c);
}

const char *tokenKindName(TokenKind kind)
{
  switch (kind) {
    case TOK_EOF: return "EOF";
    case TOK_ERROR: return "ERROR";
    case TOK_NEWLINE: return "NEWLINE";
    case TOK_IDENT: return "IDENT";
    case TOK_NUMBER: return "NUMBER";
    case TOK_FLOAT: return "FLOAT";
    case TOK_CHAR: return "CHAR";
    case TOK_STRING: return "STRING";
    case TOK_LPAREN: return "LPAREN";
    case TOK_RPAREN: return "RPAREN";
    case TOK_COMMA: return "COMMA";
    case TOK_COLON: return "COLON";
    case TOK_QUESTION: return "QUESTION";
    case TOK_EQ: return "EQ";
    case TOK_HASH: return "HASH";
    case TOK_PLUS: return "PLUS";
    case TOK_MINUS: return "MINUS";
    case TOK_STAR: return "STAR";
    case TOK_SLASH: return "SLASH";
    case TOK_PERCENT: return "PERCENT";
    case TOK_SHL: return "SHL";
    case TOK_SHR: return "SHR";
    case TOK_LT: return "LT";
    case TOK_LE: return "LE";
    case TOK_GT: return "GT";
    case TOK_GE: return "GE";
    case TOK_EQEQ: return "EQEQ";
    case TOK_NE: return "NE";
    case TOK_AMP: return "AMP";
    case TOK_CARET: return "CARET";
    case TOK_PIPE: return "PIPE";
    case TOK_ANDAND: return "ANDAND";
    case TOK_OROR: return "OROR";
    case TOK_TILDE: return "TILDE";
    case TOK_BANG: return "BANG";
    case TOK_PLUSPLUS: return "PLUSPLUS";
    case TOK_MINUSMINUS: return "MINUSMINUS";
  }
  return "UNKNOWN";
}