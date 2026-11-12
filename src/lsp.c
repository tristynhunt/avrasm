#include "avrasm/lsp.h"

#include "avrasm/assemble.h"
#include "avrasm/ctx.h"
#include "avrasm/image.h"
#include "avrasm/instr.h"
#include "avrasm/json.h"
#include "avrasm/outline.h"
#include "avrasm/source.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LSP_MAX_SETTINGS 64

// LSPs numbers for kinds of things from the specification
enum {
  SEVERITY_ERROR = 1,
  SEVERITY_WARNING = 2,
  SEVERITY_INFORMATION = 3
};
enum {
  SYMBOL_METHOD = 6,
  SYMBOL_FUNCTION = 12,
  SYMBOL_VARIABLE = 13,
  SYMBOL_CONSTANT = 14
};
enum {
  COMPLETION_METHOD = 2,
  COMPLETION_FUNCTION = 3,
  COMPLETION_VARIABLE = 6,
  COMPLETION_KEYWORD = 14,
  COMPLETION_CONSTANT = 21
};
enum {
  ERROR_PARSE = -32700,
  ERROR_INVALID_REQUEST = -32600,
  ERROR_METHOD_NOT_FOUND = -32601
};

// one assembly of one open document as the main file
typedef struct Analysis {
  AvrCtx ctx;
  Image flash;
  Image eeprom;
  Outline outline;
} Analysis;

typedef struct Doc {
  char *uri;      // as the editor wrote it
  char *path;     // the file it names normalised
  char *text;
  size_t len;
  Analysis *own;  // this document assembled as the main file
  uint32_t root;  // the document whose program shows this one. itself or one that includes it
} Doc;

struct LspServer {
  AvrVfs disk;
  LspSend send;
  void *user;
  Doc *docs;
  uint32_t docCount;
  uint32_t docCap;
  char **published;  // the URIs that have diagnostics in the editor now
  uint32_t publishedCount;
  char *rootPath;    // the workspace folder or NULL. relative include paths start there
  char *includeDirs[LSP_MAX_SETTINGS];
  uint32_t includeCount;
  char *defines[LSP_MAX_SETTINGS];
  uint32_t defineCount;
  int shutdown;      // "shutdown" has come
  int exitCode;      // -1 until "exit"
};

// ---- small helpers ----

static void *xmalloc(size_t n)
{
  void *p = malloc(n ? n : 1);
  if (p == NULL) {
    fprintf(stderr, "avrasm-lsp: out of memory\n");
    abort();
  }
  return p;
}

static char *xstrndup(const char *s, size_t n)
{
  char *p = xmalloc(n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

static char *xstrdup(const char *s)
{
  return xstrndup(s, strlen(s));
}

// adds a mallocd string to a mallocd list
static void listPush(char ***list, uint32_t *count, char *item)
{
  char **bigger = realloc(*list, (*count + 1) * sizeof **list);
  if (bigger == NULL) {
    fprintf(stderr, "avrasm-lsp: out of memory\n");
    abort();
  }
  bigger[(*count)++] = item;
  *list = bigger;
}

static int listHas(char *const *list, uint32_t count, const char *item)
{
  for (uint32_t i = 0; i < count; i++) {
    if (strcmp(list[i], item) == 0) return 1;
  }
  return 0;
}

static void listFree(char **list, uint32_t count)
{
  for (uint32_t i = 0; i < count; i++) free(list[i]);
  free(list);
}

// ---- paths and URIs ----

// does the path start with a windows drive "C:"
static int hasDrive(const char *path)
{
  char c = (char)(path[0] | 0x20);
  return c >= 'a' && c <= 'z' && path[1] == ':';
}

// "/usr/..." or "C:/..." (or "C:\\...").
static int isAbsolutePath(const char *path)
{
  return path[0] == '/' || path[0] == '\\' || (hasDrive(path) && (path[2] == '/' || path[2] == '\\'));
}

// the path with its "." and ".." parts and doubled slashes taken out without looking at the disk
// (as the file may not exist yet and links are not followed). "/a/./b//../c" is "/a/c".
static char *normalPath(const char *raw)
{
  size_t n = strlen(raw);
  char *path = xmalloc(n + 1);
  for (size_t i = 0; i <= n; i++) path[i] = raw[i] == '\\' ? '/' : raw[i];
  char *out = xmalloc(n + 2);
  size_t len = 0;
  size_t floor = 0;
  if (hasDrive(path)) {
    out[len++] = (char)(path[0] | 0x20);
    out[len++] = ':';
    floor = len;
  }
  if (path[len] == '/') out[len++] = '/', floor = len;
  const char *p = path + len;
  while(1) {
    while (*p == '/') p++;
    if (*p == '\0') break;
    const char *q = p;
    while (*q != '\0' && *q != '/') q++;
    size_t part = (size_t)(q - p);
    if (part == 1 && p[0] == '.') {
      // nothing
    } else if (part == 2 && p[0] == '.' && p[1] == '.' && len > floor) {
      while (len > floor && out[len - 1] != '/') len--;   // the last part
      if (len > floor) len--;                             // and the slash before it
    } else {
      if (len > 0 && out[len - 1] != '/') out[len++] = '/';
      memcpy(out + len, p, part);
      len += part;
      // a relative path that starts above "."
      if (part == 2 && p[0] == '.' && p[1] == '.') floor = len;
    }
    p = q;
  }
  if (len == 0) out[len++] = '.';
  out[len] = '\0';
  free(path);
  return out;
}

static int hexValue(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// the path of a file:// URI, percent-decoded and normalised. NULL for any other kind of URI.
static char *uriToPath(const char *uri)
{
  if (strncmp(uri, "file://", 7) != 0) return NULL;
  const char *p = strchr(uri + 7, '/');  // after the host which is empty for local files
  if (p == NULL) return NULL;
  char *path = xmalloc(strlen(p) + 1);
  size_t n = 0;
  for (; *p != '\0'; p++) {
    if (*p == '%' && hexValue(p[1]) >= 0 && hexValue(p[2]) >= 0) {
      path[n++] = (char)(hexValue(p[1]) * 16 + hexValue(p[2]));
      p += 2;
    } else {
      path[n++] = *p;
    }
  }
  path[n] = '\0';
  // file:///c%3A/Users/... is the windows path c:/Users/... no '/' before the drive
  char *normal = normalPath(path[0] == '/' && hasDrive(path + 1) ? path + 1 : path);
  free(path);
  return normal;
}

// a file:// URI for an absolute path. every byte that is not a letter,
// a digit or one of -._~/ is percent-encoded.
static char *pathToUri(const char *path)
{
  size_t n = strlen(path);
  char *uri = xmalloc(8 + 3 * n + 1);
  size_t len = 0;
  memcpy(uri, "file://", 7);
  len = 7;
  if (hasDrive(path)) uri[len++] = '/';
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)path[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~/", c) != NULL) {
      uri[len++] = (char)c;
    } else {
      len += (size_t)snprintf(uri + len, 4, "%%%02X", c);
    }
  }
  uri[len] = '\0';
  return uri;
}

static Doc *findDoc(LspServer *s, const char *uri)
{
  for (uint32_t i = 0; i < s->docCount; i++) {
    if (uri != NULL && strcmp(s->docs[i].uri, uri) == 0) return &s->docs[i];
  }
  return NULL;
}

static const Doc *findDocByPath(const LspServer *s, const char *path)
{
  for (uint32_t i = 0; i < s->docCount; i++) {
    if (strcmp(s->docs[i].path, path) == 0) return &s->docs[i];
  }
  return NULL;
}

// the URI to show a file under the open documents own URI if it is open so the editor matches it.
static char *uriOfPath(const LspServer *s, const char *path)
{
  const Doc *d = findDocByPath(s, path);
  return d != NULL ? xstrdup(d->uri) : pathToUri(path);
}

// ---- positions: the editor counts columns in UTF-16 code units, the source map in bytes ----

// the length of the character at s[0..n), a valid UTF-8 sequence, or 1 for a byte that is not
// part of one (the editor shows that byte as one U+FFFD).
static size_t charLength(const unsigned char *s, size_t n)
{
  size_t need = s[0] < 0x80 ? 1 : s[0] >= 0xC2 && s[0] <= 0xDF 
    ? 2 : s[0] >= 0xE0 && s[0] <= 0xEF ? 3 : s[0] >= 0xF0 && s[0] <= 0xF4 ? 4 : 1;
  if (need > n) return 1;
  for (size_t i = 1; i < need; i++) {
    if ((s[i] & 0xC0) != 0x80) return 1;
  }
  return need;
}

// how many UTF-16 units the text s[0..n) is, 2 for a 4-byte character, 1 for any other.
static uint32_t utf16Length(const char *s, size_t n)
{
  uint32_t units = 0;
  for (size_t i = 0; i < n;) {
    size_t c = charLength((const unsigned char *)s + i, n - i);
    units += c == 4 ? 2 : 1;
    i += c;
  }
  return units;
}

// the byte offset in the document of an LSP position. 
// a line or column past the end gives the end of the document or of the line.
static size_t offsetAt(const Doc *d, int64_t line, int64_t character)
{
  size_t start = 0;
  for (int64_t l = 0; l < line && start < d->len; start++) {
    if (d->text[start] == '\n') l++;
  }
  size_t end = start;
  while (end < d->len && d->text[end] != '\n') end++;
  size_t i = start;
  for (int64_t units = 0; i < end && units < character;) {
    size_t c = charLength((const unsigned char *)d->text + i, end - i);
    units += c == 4 ? 2 : 1;
    i += c;
  }
  return i;
}

static void writePosition(JsonBuf *b, const SourceFile *f, uint32_t offset)
{
  LineCol lc = sourceLineCol(f, offset);
  uint32_t start = f->lineStarts[lc.line];
  if (offset > f->len) offset = f->len;
  jsonf(b, "{\"line\":%u,\"character\":%u}", lc.line, utf16Length(f->text + start, offset - start));
}

static void writeRange(JsonBuf *b, const SourceFile *f, Span span)
{
  jsonRaw(b, "{\"start\":");
  writePosition(b, f, span.start);
  jsonRaw(b, ",\"end\":");
  writePosition(b, f, span.end);
  jsonRaw(b, "}");
}

// {"uri":...,"range":...} of a span in an analysis.
static void writeLocation(JsonBuf *b, const LspServer *s, const Analysis *an, Span span)
{
  const SourceFile *f = sourceMapGet(&an->ctx.sources, span.file);
  char *path = normalPath(f->path);
  char *uri = uriOfPath(s, path);
  jsonRaw(b, "{\"uri\":");
  jsonStr(b, uri, strlen(uri));
  jsonRaw(b, ",\"range\":");
  writeRange(b, f, span);
  jsonRaw(b, "}");
  free(uri);
  free(path);
}

// ---- assembling ----

// the vfs of every assembly, an open document as the editor has it and anything else from the disk
static char *readFile(void *user, const char *path, size_t *len)
{
  LspServer *s = user;
  char *want = normalPath(path);
  const Doc *d = findDocByPath(s, want);
  free(want);
  if (d == NULL) return s->disk.read(s->disk.user, path, len);
  *len = d->len;
  char *copy = xmalloc(d->len);
  memcpy(copy, d->text, d->len);
  return copy;
}

static Analysis *analyse(LspServer *s, const Doc *d)
{
  Analysis *an = xmalloc(sizeof *an);
  ctxInit(&an->ctx, (AvrVfs){ readFile, s });
  FileId id = sourceMapAdd(&an->ctx.sources, d->path, d->text, d->len);
  imageInit(&an->flash, &an->ctx.arena, ASSEMBLE_DEFAULT_IMAGE_SIZE);
  imageInit(&an->eeprom, &an->ctx.arena, ASSEMBLE_DEFAULT_EEPROM_SIZE);
  outlineInit(&an->outline, &an->ctx.arena);
  AsmOptions opt = { (const char *const *)s->includeDirs, s->includeCount, 
    (const char *const *)s->defines, s->defineCount, &an->outline };
  assembleOpt(&an->ctx, id, &an->flash, &an->eeprom, &opt);
  return an;
}

static void analysisFree(Analysis *an)
{
  if (an == NULL) return;
  ctxFree(&an->ctx);
  free(an);
}

// the file id of a path in an analysis. FILE_NONE if the program does not include it.
static FileId fileOf(const Analysis *an, const char *path)
{
  for (FileId f = 0; f < an->ctx.sources.count; f++) {
    char *p = normalPath(an->ctx.sources.files[f]->path);
    int same = strcmp(p, path) == 0;
    free(p);
    if (same) return f;
  }
  return FILE_NONE;
}

// publishes the diagnostics that one program has for one of its files. matches[f] says which
// file ids are that file (an include of the same file twice is two ids). the main file also gets
// the diagnostics that have no place. returns how many it published.
static uint32_t publishFile(LspServer *s, const Analysis *an, const char *uri, const char *matches, int isMain)
{
  JsonBuf b = { NULL, 0, 0 };
  // {"jsonrpc":"2.0","method":...,"params":{"uri":...,"diagnostics":[...]}}
  jsonRaw(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
  jsonStr(&b, uri, strlen(uri));
  jsonRaw(&b, ",\"diagnostics\":[");
  uint32_t count = 0;
  for (const Diag *d = an->ctx.diags.head; d != NULL; d = d->next) {
    // {"range":R,"severity":S,"source":"avrasm","message":M}
    int here = d->span.file == FILE_NONE ? isMain : matches[d->span.file];
    if (!here) continue;
    if (count++ > 0) jsonRaw(&b, ",");
    jsonRaw(&b, "{\"range\":");
    if (d->span.file == FILE_NONE) jsonRaw(&b, "{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":0}}");
    else writeRange(&b, sourceMapGet(&an->ctx.sources, d->span.file), d->span);
    jsonf(&b, ",\"severity\":%d,\"source\":\"avrasm\",\"message\":",
      d->severity == DIAG_ERROR ? SEVERITY_ERROR : d->severity == DIAG_WARNING ? SEVERITY_WARNING : SEVERITY_INFORMATION);
    jsonStr(&b, d->message, strlen(d->message));
    jsonRaw(&b, "}");
  }
  jsonRaw(&b, "]}}");
  s->send(s->user, b.data, b.len);
  jsonBufFree(&b);
  return count;
}

// publishes the diagnostics of every program that is shown (the root documents) file by file.
// a file that two programs include is shown with the first ones.
// a file that had diagnostics before and is in no program now is cleared.
static void publishAll(LspServer *s)
{
  // the paths published this time
  char **done = NULL;
  uint32_t doneCount = 0;
  // the URIs published this time
  char **sent = NULL;
  uint32_t sentCount = 0;
  // the URIs published this time with at least one diagnostic
  char **nonEmpty = NULL;
  uint32_t nonEmptyCount = 0;
  for (uint32_t r = 0; r < s->docCount; r++) {
    if (s->docs[r].root != r) continue;
    const Analysis *an = s->docs[r].own;
    char *matches = xmalloc(an->ctx.sources.count);
    for (FileId f = 0; f < an->ctx.sources.count; f++) {
      char *path = normalPath(an->ctx.sources.files[f]->path);
      if (listHas(done, doneCount, path)) {
        free(path);
        continue;
      }
      for (FileId g = 0; g < an->ctx.sources.count; g++) {
        char *other = normalPath(an->ctx.sources.files[g]->path);
        matches[g] = strcmp(other, path) == 0;
        free(other);
      }
      char *uri = uriOfPath(s, path);
      int hasDiag = 0;
      for (const Diag *d = an->ctx.diags.head; d != NULL && !hasDiag; d = d->next) {
        hasDiag = d->span.file == FILE_NONE ? f == 0 : matches[d->span.file];
      }
      if (hasDiag || findDocByPath(s, path) != NULL || listHas(s->published, s->publishedCount, uri)) {
        if (publishFile(s, an, uri, matches, f == 0) > 0) listPush(&nonEmpty, &nonEmptyCount, xstrdup(uri));
        listPush(&sent, &sentCount, xstrdup(uri));
      }
      free(uri);
      listPush(&done, &doneCount, path);
    }
    free(matches);
  }
  for (uint32_t i = 0; i < s->publishedCount; i++) {
    if (listHas(sent, sentCount, s->published[i])) continue;
    JsonBuf b = { NULL, 0, 0 };
    jsonRaw(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
    jsonStr(&b, s->published[i], strlen(s->published[i]));
    jsonRaw(&b, ",\"diagnostics\":[]}}");
    s->send(s->user, b.data, b.len);
    jsonBufFree(&b);
  }
  listFree(done, doneCount);
  listFree(sent, sentCount);
  listFree(s->published, s->publishedCount);
  s->published = nonEmpty;
  s->publishedCount = nonEmptyCount;
}

// assembles every open document again, works out which program shows each, and publishes.
static void refresh(LspServer *s)
{
  for (uint32_t i = 0; i < s->docCount; i++) {
    analysisFree(s->docs[i].own);
    s->docs[i].own = analyse(s, &s->docs[i]);
  }

  // a document is shown as part of the program of an open document
  // that includes it (and that it does not include back).
  for (uint32_t i = 0; i < s->docCount; i++) {
    s->docs[i].root = i;
    for (uint32_t j = 0; j < s->docCount; j++) {
      if (j != i && fileOf(s->docs[j].own, s->docs[i].path) != FILE_NONE &&
          fileOf(s->docs[i].own, s->docs[j].path) == FILE_NONE) {
        s->docs[i].root = j;
        break;
      }
    }
  }
  // ..and of the top of the chain, main.asm includes lcd.asm, which includes delay.asm.
  for (uint32_t i = 0; i < s->docCount; i++) {
    for (uint32_t k = 0; k < s->docCount && s->docs[s->docs[i].root].root != s->docs[i].root; k++) {
      s->docs[i].root = s->docs[s->docs[i].root].root;
    }
  }
  publishAll(s);
}

// ---- settings ----

static void setList(LspServer *s, char **list, uint32_t *count, const Json *array, int isPath)
{
  if (array == NULL || array->type != JSON_ARRAY) return;
  for (uint32_t i = 0; i < *count; i++) free(list[i]);
  *count = 0;
  for (const Json *v = array->first; v != NULL && *count < LSP_MAX_SETTINGS; v = v->next) {
    const char *text = jsonString(v);
    if (text == NULL || text[0] == '\0') continue;
    if (isPath && !isAbsolutePath(text) && s->rootPath != NULL) {
      size_t n = strlen(s->rootPath) + 1 + strlen(text) + 1;
      char *joined = xmalloc(n);
      snprintf(joined, n, "%s/%s", s->rootPath, text);
      list[(*count)++] = joined;
    } else {
      list[(*count)++] = xstrdup(text);
    }
  }
}

// {"includePaths": [...], "defines": [...]}: from the initialize request, and from
// workspace/didChangeConfiguration as "settings": {"avrasm": {...}}.
static void applySettings(LspServer *s, const Json *settings)
{
  setList(s, s->includeDirs, &s->includeCount, jsonGet(settings, "includePaths"), 1);
  setList(s, s->defines, &s->defineCount, jsonGet(settings, "defines"), 0);
}

// ---- requests ----

static const Analysis *viewOf(const LspServer *s, const Doc *d)
{
  return s->docs[d->root].own;
}

static int isWordChar(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// the document and the name at the position in the params of a request. the name is
// d->text[*start .. *end). NULL if the document is not open or there is no name there.
static Doc *nameAt(LspServer *s, const Json *params, size_t *start, size_t *end)
{
  Doc *d = findDoc(s, jsonString(jsonGet(jsonGet(params, "textDocument"), "uri")));
  if (d == NULL) return NULL;
  const Json *pos = jsonGet(params, "position");
  size_t at = offsetAt(d, jsonInt(jsonGet(pos, "line"), 0), jsonInt(jsonGet(pos, "character"), 0));
  size_t a = at, e = at;
  while (a > 0 && isWordChar(d->text[a - 1])) a--;
  while (e < d->len && isWordChar(d->text[e])) e++;
  if (a == e) return NULL;
  *start = a;
  *end = e;
  return d;
  return NULL;
}

// what a names value is in a few words. for hover, completion and document symbols.
static void describeValue(JsonBuf *b, const OutlineName *n)
{
  long long v = (long long)n->value;
  switch (n->kind) {
    case OUTLINE_LABEL:
      if (n->seg == 'c') jsonf(b, "label in program memory: word address 0x%04llX (byte address 0x%04llX)", v, v * 2);
      else jsonf(b, "label in %s: address 0x%04llX", n->seg == 'd' ? "data memory" : "EEPROM", v);
      break;
    case OUTLINE_EQU:
    case OUTLINE_SET:
      jsonf(b, "%s %lld", n->kind == OUTLINE_EQU ? "value" : "last value", v);
      if (v >= 0) jsonf(b, " (0x%llX)", v);
      break;
    case OUTLINE_DEF:
      jsonf(b, ".def: register r%lld", v);
      break;
    case OUTLINE_MACRO:
      if (v == 0) jsonRaw(b, "macro, no arguments");
      else if (v == 1) jsonRaw(b, "macro, 1 argument (@0)");
      else jsonf(b, "macro, %lld arguments (@0 to @%lld)", v, v - 1);
      break;
    case OUTLINE_DEFINE:
      jsonRaw(b, "#define");
      break;
  }
}

// the line a span is on without the white space at its start and the line end.
static void lineOf(const Analysis *an, Span span, const char **text, size_t *len)
{
  const SourceFile *f = sourceMapGet(&an->ctx.sources, span.file);
  LineCol lc = sourceLineCol(f, span.start);
  uint32_t a = f->lineStarts[lc.line];
  uint32_t e = a;
  while (e < f->len && f->text[e] != '\n' && f->text[e] != '\r') e++;
  while (a < e && (f->text[a] == ' ' || f->text[a] == '\t')) a++;
  *text = f->text + a;
  *len = e - a;
}

static void hoverName(const Analysis *an, const OutlineName *n, JsonBuf *md)
{
  if (n->defined.file != FILE_NONE) {
    const char *line;
    size_t len;
    lineOf(an, n->defined, &line, &len);
    jsonRaw(md, "```avrasm\n");
    jsonf(md, "%.*s", (int)len, line);
    jsonRaw(md, "\n```\n");
  } else {
    jsonRaw(md, "Defined on the command line (-D).\n\n");
  }
  describeValue(md, n);
}

// an instruction. how it is written and what it became where it is (the first place for a line
// of a macro that is called more than once).
static void hoverInstr(const Analysis *an, const Doc *d, const InstrDesc *in, size_t at, JsonBuf *md)
{
  jsonf(md, "**%s** %s: %d word%s", in->name, instrSyntax(in), in->words, in->words == 1 ? "" : "s");
  FileId file = fileOf(an, d->path);
  const OutlineCode *found = NULL;
  uint32_t count = 0;
  for (uint32_t i = 0; i < an->outline.codeCount; i++) {
    const OutlineCode *c = &an->outline.code[i];
    if (c->head.file != file || at < c->head.start || at >= c->head.end || c->isData) continue;
    if (count++ == 0) found = c;
  }
  if (found == NULL || !an->flash.used[found->pc * 2]) return;
  long long pc = (long long)found->pc;
  jsonf(md, "\n\nat word 0x%04llX (byte 0x%04llX):", pc, pc * 2);
  for (uint32_t i = 0; i < found->words * 2 && found->pc * 2 + i < an->flash.size; i++) {
    jsonf(md, " `%02x`", an->flash.bytes[found->pc * 2 + i]);
  }
  if (count > 1) jsonf(md, " (the first of %u places this macro line is used)", count);
}

static void onHover(LspServer *s, const Json *params, JsonBuf *out)
{
  size_t a, e;
  Doc *d = nameAt(s, params, &a, &e);
  if (d == NULL) {
    jsonRaw(out, "null");
    return;
  }
  const Analysis *an = viewOf(s, d);
  JsonBuf md = { NULL, 0, 0 };
  const OutlineName *n = outlineFind(&an->outline, d->text + a, e - a);
  const InstrDesc *in = instrFind(d->text + a, e - a);
  if (n != NULL) hoverName(an, n, &md);
  else if (in != NULL) hoverInstr(an, d, in, a, &md);
  if (md.len == 0) {
    jsonRaw(out, "null");
  } else {
    jsonRaw(out, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
    jsonStr(out, md.data, md.len);
    jsonRaw(out, "}}");
  }
  jsonBufFree(&md);
}

static void onDefinition(LspServer *s, const Json *params, JsonBuf *out)
{
  size_t a, e;
  Doc *d = nameAt(s, params, &a, &e);
  const OutlineName *n = d != NULL ? outlineFind(&viewOf(s, d)->outline, d->text + a, e - a) : NULL;
  if (n == NULL || n->defined.file == FILE_NONE) jsonRaw(out, "null");
  else writeLocation(out, s, viewOf(s, d), n->defined);
}

static int symbolKind(const OutlineName *n)
{
  switch (n->kind) {
    case OUTLINE_LABEL: return n->seg == 'c' ? SYMBOL_FUNCTION : SYMBOL_VARIABLE;
    case OUTLINE_EQU: return SYMBOL_CONSTANT;
    case OUTLINE_MACRO: return SYMBOL_METHOD;
    case OUTLINE_DEFINE: return SYMBOL_CONSTANT;
    default: return SYMBOL_VARIABLE;
  }
}

// the names this document defines in the order they come.
static void onDocumentSymbol(LspServer *s, const Json *params, JsonBuf *out)
{
  Doc *d = findDoc(s, jsonString(jsonGet(jsonGet(params, "textDocument"), "uri")));
  jsonRaw(out, "[");
  if (d != NULL) {
    const Analysis *an = viewOf(s, d);
    FileId file = fileOf(an, d->path);
    uint32_t count = 0;
    for (uint32_t i = 0; i < an->outline.nameCount; i++) {
      const OutlineName *n = &an->outline.names[i];
      if (n->defined.file == FILE_NONE || n->defined.file != file) continue;
      const SourceFile *f = sourceMapGet(&an->ctx.sources, file);
      if (count++ > 0) jsonRaw(out, ",");
      jsonRaw(out, "{\"name\":");
      jsonStr(out, n->name, n->len);
      jsonRaw(out, ",\"detail\":\"");
      describeValue(out, n);  // plain text, nothing that needs escaping
      jsonf(out, "\",\"kind\":%d,\"range\":", symbolKind(n));
      writeRange(out, f, n->defined);
      jsonRaw(out, ",\"selectionRange\":");
      writeRange(out, f, n->defined);
      jsonRaw(out, "}");
    }
  }
  jsonRaw(out, "]");
}

static const char *const directives[] = {
  ".byte", ".cseg", ".db", ".dd", ".def", ".device", ".dq", ".dseg", ".dw", ".elif", ".else", ".endif", ".endm",
  ".endmacro", ".equ", ".error", ".eseg", ".exit", ".if", ".ifdef", ".ifndef", ".include", ".list", ".listmac",
  ".macro", ".message", ".nolist", ".nooverlap", ".org", ".overlap", ".set", ".undef", ".warning",
};

static void completionItem(JsonBuf *out, int *first, const char *label, size_t len, int kind, const char *detail)
{
  if (!*first) jsonRaw(out, ",");
  *first = 0;
  jsonRaw(out, "{\"label\":");
  jsonStr(out, label, len);
  jsonf(out, ",\"kind\":%d", kind);
  if (detail != NULL && detail[0] != '\0') {
    jsonRaw(out, ",\"detail\":");
    jsonStr(out, detail, strlen(detail));
  }
  jsonRaw(out, "}");
}

// every mnemonic, every directive and every name the program knows. the editor filters them by
// what has been typed.
static void onCompletion(LspServer *s, const Json *params, JsonBuf *out)
{
  int first = 1;
  jsonRaw(out, "[");
  const InstrDesc *in;
  for (size_t i = 0; (in = instrAt(i)) != NULL; i++) {
    completionItem(out, &first, in->name, strlen(in->name), COMPLETION_KEYWORD, instrSyntax(in));
  }
  for (size_t i = 0; i < sizeof directives / sizeof directives[0]; i++) {
    completionItem(out, &first, directives[i], strlen(directives[i]), COMPLETION_KEYWORD, NULL);
  }
  Doc *d = findDoc(s, jsonString(jsonGet(jsonGet(params, "textDocument"), "uri")));
  if (d != NULL) {
    const Outline *o = &viewOf(s, d)->outline;
    for (uint32_t i = 0; i < o->nameCount; i++) {
      const OutlineName *n = &o->names[i];
      int kind = n->kind == OUTLINE_LABEL ? COMPLETION_FUNCTION : n->kind == OUTLINE_MACRO ? COMPLETION_METHOD
        : n->kind == OUTLINE_EQU || n->kind == OUTLINE_DEFINE ? COMPLETION_CONSTANT : COMPLETION_VARIABLE;
      JsonBuf detail = { NULL, 0, 0 };
      describeValue(&detail, n);
      completionItem(out, &first, n->name, n->len, kind, detail.data);
      jsonBufFree(&detail);
    }
  }
  jsonRaw(out, "]");
}

static void onInitialize(LspServer *s, const Json *params, JsonBuf *out)
{
  const char *rootUri = jsonString(jsonGet(params, "rootUri"));
  const Json *folders = jsonGet(params, "workspaceFolders");
  if (folders != NULL && folders->type == JSON_ARRAY && folders->first != NULL) {
    rootUri = jsonString(jsonGet(folders->first, "uri"));
  }
  free(s->rootPath);
  s->rootPath = rootUri != NULL ? uriToPath(rootUri) : NULL;
  applySettings(s, jsonGet(params, "initializationOptions"));
  jsonRaw(out, 
    "{\"capabilities\":{"
    "\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
    "\"hoverProvider\":true,"
    "\"definitionProvider\":true,"
    "\"documentSymbolProvider\":true,"
    "\"completionProvider\":{}"
    "},\"serverInfo\":{\"name\":\"avrasm-lsp\"}}"
  );
}

static void onShutdown(LspServer *s, const Json *params, JsonBuf *out)
{
  (void)params;
  s->shutdown = 1;
  jsonRaw(out, "null");
}

// ---- notifications ----

static void onDidOpen(LspServer *s, const Json *params)
{
  const Json *td = jsonGet(params, "textDocument");
  const char *uri = jsonString(jsonGet(td, "uri"));
  const Json *text = jsonGet(td, "text");
  char *path = uri != NULL ? uriToPath(uri) : NULL;
  if (path == NULL || text == NULL || text->type != JSON_STRING || findDoc(s, uri) != NULL) {
    free(path);
    return;
  }
  if (s->docCount == s->docCap) {
    s->docCap = s->docCap ? s->docCap * 2 : 8;
    Doc *bigger = realloc(s->docs, s->docCap * sizeof *s->docs);
    if (bigger == NULL) {
      fprintf(stderr, "avrasm-lsp: out of memory\n");
      abort();
    }
    s->docs = bigger;
  }
  Doc *d = &s->docs[s->docCount++];
  d->uri = xstrdup(uri);
  d->path = path;
  d->text = xstrndup(text->text, text->len);
  d->len = text->len;
  d->own = NULL;
  d->root = 0;
  refresh(s);
}

// the whole text comes each time (the server asked for "change": 1, full sync) and the last change wins.
static void onDidChange(LspServer *s, const Json *params)
{
  Doc *d = findDoc(s, jsonString(jsonGet(jsonGet(params, "textDocument"), "uri")));
  const Json *changes = jsonGet(params, "contentChanges");
  if (d == NULL || changes == NULL || changes->type != JSON_ARRAY) return;
  const Json *text = NULL;
  for (const Json *c = changes->first; c != NULL; c = c->next) {
    if (jsonString(jsonGet(c, "text")) != NULL) text = jsonGet(c, "text");
  }
  if (text == NULL) return;
  free(d->text);
  d->text = xstrndup(text->text, text->len);
  d->len = text->len;
  refresh(s);
}

static void onDidClose(LspServer *s, const Json *params)
{
  Doc *d = findDoc(s, jsonString(jsonGet(jsonGet(params, "textDocument"), "uri")));
  if (d == NULL) return;
  free(d->uri);
  free(d->path);
  free(d->text);
  analysisFree(d->own);
  *d = s->docs[--s->docCount];
  refresh(s);
}

static void onDidChangeConfiguration(LspServer *s, const Json *params)
{
  applySettings(s, jsonGet(jsonGet(params, "settings"), "avrasm"));
  refresh(s);
}

static void onExit(LspServer *s, const Json *params)
{
  (void)params;
  s->exitCode = s->shutdown ? 0 : 1;
}

// ---- dispatch ----

typedef struct Method {
  const char *name;
  void (*request)(LspServer *s, const Json *params, JsonBuf *result);  // writes the result
  void (*notify)(LspServer *s, const Json *params);
} Method;

static const Method methods[] = {
  { "initialize", onInitialize, NULL },
  { "shutdown", onShutdown, NULL },
  { "textDocument/hover", onHover, NULL },
  { "textDocument/definition", onDefinition, NULL },
  { "textDocument/documentSymbol", onDocumentSymbol, NULL },
  { "textDocument/completion", onCompletion, NULL },
  { "textDocument/didOpen", NULL, onDidOpen },
  { "textDocument/didChange", NULL, onDidChange },
  { "textDocument/didClose", NULL, onDidClose },
  { "workspace/didChangeConfiguration", NULL, onDidChangeConfiguration },
  { "exit", NULL, onExit },
};

static void respondError(LspServer *s, const Json *id, int code, const char *message)
{
  JsonBuf b = { NULL, 0, 0 };
  jsonRaw(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
  jsonValue(&b, id);
  jsonf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
  jsonStr(&b, message, strlen(message));
  jsonRaw(&b, "}}");
  s->send(s->user, b.data, b.len);
  jsonBufFree(&b);
}

LspServer *lspCreate(AvrVfs disk, LspSend send, void *user)
{
  LspServer *s = xmalloc(sizeof *s);
  memset(s, 0, sizeof *s);
  s->disk = disk.read != NULL ? disk : avrDiskVfs();
  s->send = send;
  s->user = user;
  s->exitCode = -1;
  return s;
}

void lspFree(LspServer *s)
{
  if (s == NULL) return;
  for (uint32_t i = 0; i < s->docCount; i++) {
    free(s->docs[i].uri);
    free(s->docs[i].path);
    free(s->docs[i].text);
    analysisFree(s->docs[i].own);
  }
  free(s->docs);
  listFree(s->published, s->publishedCount);
  for (uint32_t i = 0; i < s->includeCount; i++) free(s->includeDirs[i]);
  for (uint32_t i = 0; i < s->defineCount; i++) free(s->defines[i]);
  free(s->rootPath);
  free(s);
}

void lspHandle(LspServer *s, const char *json, size_t len)
{
  Arena arena;
  arenaInit(&arena, 0);
  const Json *msg = jsonParse(&arena, json, len);
  const Json *id = jsonGet(msg, "id");
  const char *name = jsonString(jsonGet(msg, "method"));
  const Json *params = jsonGet(msg, "params");
  const Method *m = NULL;
  for (size_t i = 0; name != NULL && i < sizeof methods / sizeof methods[0]; i++) {
    if (strcmp(methods[i].name, name) == 0) m = &methods[i];
  }
  if (msg == NULL || msg->type != JSON_OBJECT) {
    respondError(s, NULL, ERROR_PARSE, "the message is not a JSON object");
  } else if (name == NULL) {
    // a response to a request of ours so the server sends none
  } else if (id == NULL) {
    // other notifications ($/cancelRequest ...)
    if (m != NULL && m->notify != NULL) m->notify(s, params);
  } else if (s->shutdown) {
    respondError(s, id, ERROR_INVALID_REQUEST, "the server has been shut down");
  } else if (m == NULL || m->request == NULL) {
    respondError(s, id, ERROR_METHOD_NOT_FOUND, name);
  } else {
    JsonBuf result = { NULL, 0, 0 };
    m->request(s, params, &result);
    JsonBuf b = { NULL, 0, 0 };
    jsonRaw(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    jsonValue(&b, id);
    jsonRaw(&b, ",\"result\":");
    jsonRaw(&b, result.data);
    jsonRaw(&b, "}");
    s->send(s->user, b.data, b.len);
    jsonBufFree(&b);
    jsonBufFree(&result);
  }
  arenaFree(&arena);
}

int lspExitCode(const LspServer *s)
{
  return s->exitCode;
}