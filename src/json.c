#include "avrasm/json.h"
#include "avrasm/arena.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64

// ---- parsing ----

typedef struct Parser {
  Arena *arena;
  const char *p;    // the next character
  const char *end;
  int depth;
} Parser;

static void skipSpace(Parser *ps)
{
  while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

// checks if the text continue with `word` and moves past it if so.
static int takeWord(Parser *ps, const char *word)
{
  size_t n = strlen(word);
  if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0) return 0;
  ps->p += n;
  return 1;
}

static int hexDigit(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// the 4 hex digits after a \u, -1 if they are not there.
static long hex4(Parser *ps)
{
  if (ps->end - ps->p < 4) return -1;
  long v = 0;
  for (int i = 0; i < 4; i++) {
    int d = hexDigit(ps->p[i]);
    if (d < 0) return -1;
    v = v * 16 + d;
  }
  ps->p += 4;
  return v;
}

// writes code point c as UTF-8 at out. returns how many bytes.
static size_t putUtf8(char *out, unsigned long c)
{
  if (c < 0x80) {
    out[0] = (char)c;
    return 1;
  }
  if (c < 0x800) {
    out[0] = (char)(0xC0 | (c >> 6));
    out[1] = (char)(0x80 | (c & 0x3F));
    return 2;
  }
  if (c < 0x10000) {
    out[0] = (char)(0xE0 | (c >> 12));
    out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[2] = (char)(0x80 | (c & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (c >> 18));
  out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
  out[3] = (char)(0x80 | (c & 0x3F));
  return 4;
}

// a string from its opening quote. the decoded text is never longer than the JSON text of it.
static int parseString(Parser *ps, const char **text, size_t *len)
{
  ps->p++;  // the "
  const char *close = ps->p;
  while (close < ps->end && *close != '"') close += (*close == '\\' && close + 1 < ps->end) ? 2 : 1;
  if (close >= ps->end) return 0;
  char *out = arenaAlloc(ps->arena, (size_t)(close - ps->p) + 1, 1);
  size_t n = 0;
  while (ps->p < close) {
    unsigned char c = (unsigned char)*ps->p++;
    if (c < 0x20) return 0;
    if (c != '\\') {
      out[n++] = (char)c;
      continue;
    }
    char e = *ps->p++;
    switch (e) {
      case '"': out[n++] = '"'; break;
      case '\\': out[n++] = '\\'; break;
      case '/': out[n++] = '/'; break;
      case 'b': out[n++] = '\b'; break;
      case 'f': out[n++] = '\f'; break;
      case 'n': out[n++] = '\n'; break;
      case 'r': out[n++] = '\r'; break;
      case 't': out[n++] = '\t'; break;
      case 'u': {
        long u = hex4(ps);
        if (u < 0) return 0;
        unsigned long cmp = (unsigned long)u;
        // the first half of a pair, the second must follow
        if (u >= 0xD800 && u <= 0xDBFF) {
          long lo = -1;
          if (ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
            ps->p += 2;
            lo = hex4(ps);
          }
          cmp = (lo >= 0xDC00 && lo <= 0xDFFF) 
            ? 0x10000 + (((unsigned long)u - 0xD800) << 10) + ((unsigned long)lo - 0xDC00) 
            : 0xFFFD;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
          // a second half on its own
          cmp = 0xFFFD;
        }
        // never longer than the 6 or 12 characters it came from
        n += putUtf8(out + n, cmp);
        break;
      }
      default:
        return 0;
    }
  }
  ps->p = close + 1;
  out[n] = '\0';
  *text = out;
  *len = n;
  return 1;
}

// a number as JSON writes them: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
static int parseNumber(Parser *ps, double *value)
{
  const char *start = ps->p, *q = ps->p;
  if (q < ps->end && *q == '-') q++; 
  if (q < ps->end && *q == '0') {
    q++;
  } else if (q < ps->end && *q >= '1' && *q <= '9') {
    while (q < ps->end && *q >= '0' && *q <= '9') q++;
  } else {
    return 0;
  }

  if (q < ps->end && *q == '.') {
    q++;
    if (q >= ps->end || *q > '9' || *q  < '0') return 0;
    while (q < ps->end && *q >= '0' && *q <= '9') q++;
  }

  if (q < ps->end && (*q == 'e' || *q == 'E')) {
    q++;
    if (q < ps->end && (*q == '+' || *q == '-')) q++; 
    if (q >= ps->end || *q > '9' || *q  < '0') return 0;
    while (q < ps->end && *q >= '0' && *q <= '9') q++;
  }

  char buf[64];
  if ((size_t)(q - start) >= sizeof buf) return 0;
  memcpy(buf, start, (size_t)(q - start));
  buf[q - start] = '\0';
  *value = strtod(buf, NULL);
  ps->p = q;
  return 1;
}

static Json *parseValue(Parser *ps);

// the elements of an array or the members of an object, from the [ or {.
static int parseItems(Parser *ps, Json *v, char close)
{
  ps->p++;
  skipSpace(ps);
  if (ps->p < ps->end && *ps->p == close) {
    ps->p++;
    return 1;
  }
  Json **link = &v->first;

  while(1) {
    const char *key = NULL;
    size_t keyLen = 0;
    if (close == '}') {
      skipSpace(ps);
      if (ps->p >= ps->end || *ps->p != '"' || !parseString(ps, &key, &keyLen)) return 0;
      skipSpace(ps);
      if (ps->p >= ps->end || *ps->p != ':') return 0;
      ps->p++;
    }
    Json *item = parseValue(ps);
    if (item == NULL) return 0;
    item->key = key;
    item->keyLen = keyLen;
    *link = item;
    link = &item->next;
    skipSpace(ps);
    if (ps->p < ps->end && *ps->p == ',') {
      ps->p++;
      continue;
    }
    if (ps->p < ps->end && *ps->p == close) {
      ps->p++;
      return 1;
    }
    return 0;
  }
}

static Json *parseValue(Parser *ps)
{
  skipSpace(ps);
  if (ps->p >= ps->end) return NULL;
  Json *v = arenaAlloc(ps->arena, sizeof *v, _Alignof(Json));
  char c = *ps->p;
  if (c == '{' || c == '[') {
    if (++ps->depth > JSON_MAX_DEPTH) return NULL;
    v->type = c == '{' ? JSON_OBJECT : JSON_ARRAY;
    if (!parseItems(ps, v, c == '{' ? '}' : ']')) return NULL;
    ps->depth--;
  } else if (c == '"') {
    v->type = JSON_STRING;
    if (!parseString(ps, &v->text, &v->len)) return NULL;
  } else if (takeWord(ps, "true") || takeWord(ps, "false")) {
    v->type = JSON_BOOL;
    v->boolean = c == 't';
  } else if (takeWord(ps, "null")) {
    v->type = JSON_NULL;
  } else {
    v->type = JSON_NUMBER;
    if (!parseNumber(ps, &v->number)) return NULL;
  }
  return v;
}

Json *jsonParse(Arena *arena, const char *text, size_t len)
{
  Parser ps = { arena, text, text + len, 0 };
  Json *v = parseValue(&ps);
  skipSpace(&ps);
  return (v != NULL && ps.p == ps.end) ? v : NULL;
}

const Json *jsonGet(const Json *v, const char *key)
{
  if (v == NULL || v->type != JSON_OBJECT) return NULL;
  size_t n = strlen(key);
  for (const Json *m = v->first; m != NULL; m = m->next) {
    if (m->keyLen == n && memcmp(m->key, key, n) == 0) return m;
  }
  return NULL;
}

const char *jsonString(const Json *v)
{
  return (v != NULL && v->type == JSON_STRING) ? v->text : NULL;
}

int64_t jsonInt(const Json *v, int64_t fallback)
{
  if (v == NULL || v->type != JSON_NUMBER) return fallback;
  double d = v->number;
  if (d < -9007199254740992.0 || d > 9007199254740992.0 || d != (double)(int64_t)d) return fallback;
  return (int64_t)d;
}

// ---- writing ----

static void reserve(JsonBuf *b, size_t more)
{
  if (b->len + more + 1 <= b->cap) return;
  size_t cap = b->cap ? b->cap : 256;
  while (cap < b->len + more + 1) cap *= 2;
  char *p = realloc(b->data, cap);
  if (p == NULL) {
    fprintf(stderr, "avrasm: out of memory\n");
    abort();
  }
  b->data = p;
  b->cap = cap;
}

static void append(JsonBuf *b, const char *s, size_t n)
{
  reserve(b, n);
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

void jsonBufFree(JsonBuf *b)
{
  free(b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

void jsonRaw(JsonBuf *b, const char *text)
{
  append(b, text, strlen(text));
}

void jsonf(JsonBuf *b, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  reserve(b, (size_t)n);
  va_start(ap, fmt);
  vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
}

// how many bytes the valid UTF-8 sequence at s[0..n) takes. 0 if it is not one.
static size_t utf8Length(const unsigned char *s, size_t n)
{
  size_t need = s[0] >= 0xC2 && s[0] <= 0xDF ? 2 : s[0] >= 0xE0 && s[0] <= 0xEF ? 3 : s[0] >= 0xF0 && s[0] <= 0xF4 ? 4 : 0;
  if (need == 0 || need > n) return 0;
  for (size_t i = 1; i < need; i++) {
    if ((s[i] & 0xC0) != 0x80) return 0;
  }
  return need;
}

void jsonStr(JsonBuf *b, const char *s, size_t len)
{
  const unsigned char *u = (const unsigned char *)s;
  append(b, "\"", 1);
  for (size_t i = 0; i < len;) {
    unsigned char c = u[i];
    
    if (c >= 0x80) {
      size_t n = utf8Length(u + i, len - i);
      if (n == 0) {
        // U+FFFD
        append(b, "\xEF\xBF\xBD", 3);
        i++;
      } else {
        append (b, s + i, n);
        i += n;
      }
      continue;
    }

    if (c == '"' || c == '\\') {
      char esc[2] = { '\\', (char)c };
      append(b, esc, 2);
    } else if (c == '\n') {
      append(b, "\\n", 2);
    } else if (c == '\r') {
      append(b, "\\r", 2);
    } else if (c == '\t') {
      append(b, "\\t", 2);
    } else if (c < 0x20 || c == 0x7F) {
      jsonf(b, "\\u%04x", c);
    } else {
      append(b, s + i, 1);
    }

    i++;
  }
  append(b, "\"", 1);
}

void jsonValue(JsonBuf *b, const Json *v)
{
  if (v == NULL) {
    jsonRaw(b, "null");
    return;
  }

  switch (v->type) {
    case JSON_NULL: jsonRaw(b, "null"); break;
    case JSON_BOOL: jsonRaw(b, v->boolean ? "true" : "false"); break;
    case JSON_NUMBER:
      if (jsonInt(v, 1) == jsonInt(v, 2)) jsonf(b, "%lld", (long long)jsonInt(v, 0));
      else jsonf(b, "%.17g", v->number);
      break;
    case JSON_STRING: jsonStr(b, v->text, v->len); break;
    case JSON_ARRAY:
    case JSON_OBJECT:
      jsonRaw(b, v->type == JSON_ARRAY ? "[" : "{");
      for (const Json *m = v->first; m != NULL; m = m->next) {
        if (m != v->first) jsonRaw(b, ",");
        if (v->type == JSON_OBJECT) {
          jsonStr(b, m->key, m->keyLen);
          jsonRaw(b, ":");
        }
        jsonValue(b, m);
      }
      jsonRaw(b, v->type == JSON_ARRAY ? "]" : "}");
      break;
  }
}