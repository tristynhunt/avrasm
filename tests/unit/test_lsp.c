#include "avrasm/json.h"
#include "avrasm/lsp.h"
#include "check.h"

#include <stdlib.h>
#include <string.h>

static const struct {
  const char *path;
  const char *text;
} disk[] = {
  { "/p/lib.inc", "wait:\n  dec counter\n  brne wait\n  ret\n" },
  { "/p/bad.inc", "nop\nnosuch r1\n" },
  { "/p/inc/consts.inc", ".equ LIMIT = 100\n" },
  { "c:/Users/z/lib.inc", "wait:\n  ret\n" },
  { "C:/Users/z/bad.inc", "nosuch\n" },
};

static char *readDisk(void *user, const char *path, size_t *len)
{
  (void)user;
  for (size_t i = 0; i < sizeof disk / sizeof disk[0]; i++) {
    if (strcmp(path, disk[i].path) != 0) continue;
    *len = strlen(disk[i].text);
    char *copy = malloc(*len + 1);
    memcpy(copy, disk[i].text, *len + 1);
    return copy;
  }
  return NULL;
}

#define MAX_SENT 64
static char *sent[MAX_SENT];
static size_t sentCount;
static Arena arena;

#define REQUIRE_VOID(cond) \
  do { \
    if (!(cond)) { \
      fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #cond); \
      check_failures_++; \
      return; \
    } \
  } while (0)

static void collect(void *user, const char *json, size_t len)
{
  (void)user;
  if (sentCount == MAX_SENT) return;
  char *copy = malloc(len + 1);
  memcpy(copy, json, len);
  copy[len] = '\0';
  sent[sentCount++] = copy;
}

static void forget(void)
{
  for (size_t i = 0; i < sentCount; i++) free(sent[i]);
  sentCount = 0;
}

static void handle(LspServer *s, const char *json)
{
  lspHandle(s, json, strlen(json));
}

static const Json *response(int id)
{
  for (size_t i = 0; i < sentCount; i++) {
    const Json *m = jsonParse(&arena, sent[i], strlen(sent[i]));
    if (m != NULL && jsonInt(jsonGet(m, "id"), -1) == id) return m;
  }
  return NULL;
}

static const Json *published(const char *uri)
{
  const Json *found = NULL;
  for (size_t i = 0; i < sentCount; i++) {
    const Json *m = jsonParse(&arena, sent[i], strlen(sent[i]));
    const Json *p = jsonGet(m, "params");
    const char *method = jsonString(jsonGet(m, "method"));
    if (method != NULL && strcmp(method, "textDocument/publishDiagnostics") == 0 &&
        strcmp(jsonString(jsonGet(p, "uri")), uri) == 0) {
      found = jsonGet(p, "diagnostics");
    }
  }
  return found;
}

static int count(const Json *array)
{
  int n = 0;
  for (const Json *v = array != NULL ? array->first : NULL; v != NULL; v = v->next) n++;
  return n;
}

static void sendText(LspServer *s, const char *method, const char *uri, const char *text)
{
  JsonBuf b = { NULL, 0, 0 };
  jsonf(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"version\":1", method, uri);
  if (strcmp(method, "textDocument/didOpen") == 0) {
    jsonRaw(&b, ",\"text\":");
    jsonStr(&b, text, strlen(text));
    jsonRaw(&b, "}}}");
  } else {
    jsonRaw(&b, "},\"contentChanges\":[{\"text\":");
    jsonStr(&b, text, strlen(text));
    jsonRaw(&b, "}]}}");
  }
  handle(s, b.data);
  jsonBufFree(&b);
}

static void request(LspServer *s, int id, const char *method, const char *uri, int line, int character)
{
  JsonBuf b = { NULL, 0, 0 };
  jsonf(&b, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
            "\"position\":{\"line\":%d,\"character\":%d}}}", id, method, uri, line, character);
  handle(s, b.data);
  jsonBufFree(&b);
}

static LspServer *start(const char *initParams)
{
  LspServer *s = lspCreate((AvrVfs){ readDisk, NULL }, collect, NULL);
  JsonBuf b = { NULL, 0, 0 };
  jsonf(&b, "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":%s}", initParams);
  handle(s, b.data);
  jsonBufFree(&b);
  handle(s, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
  return s;
}

static void stop(LspServer *s)
{
  lspFree(s);
  forget();
}

static void checkRange(const Json *d, int line, int start, int end)
{
  const Json *r = jsonGet(d, "range");
  CHECK_INT(jsonInt(jsonGet(jsonGet(r, "start"), "line"), -1), line);
  CHECK_INT(jsonInt(jsonGet(jsonGet(r, "start"), "character"), -1), start);
  CHECK_INT(jsonInt(jsonGet(jsonGet(r, "end"), "line"), -1), line);
  CHECK_INT(jsonInt(jsonGet(jsonGet(r, "end"), "character"), -1), end);
}

static void testLifecycle(void)
{
  LspServer *s = start("{}");
  const Json *caps = jsonGet(jsonGet(response(0), "result"), "capabilities");
  CHECK(jsonGet(caps, "hoverProvider") != NULL && jsonGet(caps, "hoverProvider")->boolean);
  CHECK_INT(jsonInt(jsonGet(jsonGet(caps, "textDocumentSync"), "change"), 0), 1);
  CHECK_INT(sentCount, 1);
  forget();
  handle(s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/formatting\",\"params\":{}}");
  CHECK_INT(jsonInt(jsonGet(jsonGet(response(2), "error"), "code"), 0), -32601);
  handle(s, "{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\",\"params\":{\"id\":2}}");
  CHECK_INT(sentCount, 1);
  handle(s, "{\"jsonrpc\":");
  const Json *m = jsonParse(&arena, sent[1], strlen(sent[1]));
  CHECK_INT(jsonInt(jsonGet(jsonGet(m, "error"), "code"), 0), -32700);
  CHECK(jsonGet(m, "id") != NULL && jsonGet(m, "id")->type == JSON_NULL);
  handle(s, "{\"jsonrpc\":\"2.0\",\"id\":\"s\",\"method\":\"shutdown\"}");
  CHECK(strstr(sent[2], "\"id\":\"s\",\"result\":null") != NULL);
  request(s, 3, "textDocument/hover", "file:///p/a.asm", 0, 0);
  CHECK_INT(jsonInt(jsonGet(jsonGet(response(3), "error"), "code"), 0), -32600);
  CHECK_INT(lspExitCode(s), -1);
  handle(s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
  CHECK_INT(lspExitCode(s), 0);
  stop(s);

  s = start("{}");
  handle(s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
  CHECK_INT(lspExitCode(s), 1);
  stop(s);
}

static void testDiagnostics(void)
{
  const char *uri = "file:///p/main.asm";
  LspServer *s = start("{}");
  sendText(s, "textDocument/didOpen", uri, "nop\nldi r16, nosuch\n");
  const Json *d = published(uri);
  REQUIRE_VOID(count(d) == 1);
  CHECK_INT(jsonInt(jsonGet(d->first, "severity"), 0), 1);
  CHECK(strstr(jsonString(jsonGet(d->first, "message")), "nosuch") != NULL);
  checkRange(d->first, 1, 9, 15);
  forget();
  sendText(s, "textDocument/didChange", uri, "nop\nldi r16, 1\nrjmp 0\n");
  CHECK(published(uri) != NULL && count(published(uri)) == 0);

  sendText(s, "textDocument/didChange", uri, ".db \"\xC3\xA9\xF0\x9F\x98\x80\", nosuch\n");
  d = published(uri);
  REQUIRE_VOID(count(d) == 2);
  CHECK_INT(jsonInt(jsonGet(d->first, "severity"), 0), 2);
  checkRange(d->first->next, 0, 11, 17);
  stop(s);
}

static void testIncludes(void)
{
  const char *main = "file:///p/main.asm", *lib = "file:///p/lib.inc", *bad = "file:///p/bad.inc";
  LspServer *s = start("{}");
  sendText(s, "textDocument/didOpen", lib, "wait:\n  dec counter\n  brne wait\n  ret\n  rjmp wiat\n");
  CHECK_INT(count(published(lib)), 2);
  forget();

  sendText(s, "textDocument/didOpen", main, ".def counter = r16\nrcall wait\n.include \"bad.inc\"\n.include \"lib.inc\"\n");
  CHECK_INT(count(published(main)), 0);
  const Json *d = published(lib);
  REQUIRE_VOID(count(d) == 1);
  CHECK(strstr(jsonString(jsonGet(d->first, "message")), "wiat") != NULL);
  checkRange(d->first, 4, 7, 11);
  d = published(bad);
  REQUIRE_VOID(count(d) == 1);
  checkRange(d->first, 1, 0, 6);
  forget();
  sendText(s, "textDocument/didChange", main, ".def counter = r16\nrcall wait\n.include \"lib.inc\"\n");
  CHECK(published(bad) != NULL && count(published(bad)) == 0);
  forget();

  request(s, 5, "textDocument/definition", main, 1, 8);
  const Json *loc = jsonGet(response(5), "result");
  CHECK_STR(jsonString(jsonGet(loc, "uri")), lib);
  checkRange(loc, 0, 0, 4);
  forget();

  handle(s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\",\"params\":{\"textDocument\":{\"uri\":\"file:///p/main.asm\"}}}");
  CHECK_INT(count(published(lib)), 2);
  CHECK(published(main) == NULL);
  stop(s);
}

static void testRequests(void)
{
  const char *uri = "file:///p/main.asm";
  LspServer *s = start("{}");
  sendText(s, "textDocument/didOpen", uri, ".equ N = 5\nstart:\n  ldi r16, N\n  rjmp start\n.db \"\xF0\x9F\x98\x80\xF0\x9F\x98\x80\", N\n");
  request(s, 1, "textDocument/hover", uri, 2, 11);
  const char *text = jsonString(jsonGet(jsonGet(jsonGet(response(1), "result"), "contents"), "value"));
  CHECK(text != NULL && strstr(text, ".equ N = 5") != NULL && strstr(text, "value 5 (0x5)") != NULL);
  request(s, 2, "textDocument/hover", uri, 2, 3);
  text = jsonString(jsonGet(jsonGet(jsonGet(response(2), "result"), "contents"), "value"));
  CHECK(text != NULL && strstr(text, "**ldi** Rd, K") != NULL && strstr(text, "word 0x0000") != NULL &&
        strstr(text, "`05` `e0`") != NULL);
  request(s, 3, "textDocument/hover", uri, 4, 12);
  text = jsonString(jsonGet(jsonGet(jsonGet(response(3), "result"), "contents"), "value"));
  CHECK(text != NULL && strstr(text, "value 5") != NULL);
  request(s, 4, "textDocument/hover", uri, 2, 6);
  CHECK(jsonGet(response(4), "result")->type == JSON_NULL);

  request(s, 5, "textDocument/definition", uri, 3, 9);
  const Json *loc = jsonGet(response(5), "result");
  CHECK_STR(jsonString(jsonGet(loc, "uri")), uri);
  checkRange(loc, 1, 0, 5);

  request(s, 6, "textDocument/documentSymbol", uri, 0, 0);
  const Json *syms = jsonGet(response(6), "result");
  REQUIRE_VOID(count(syms) == 2);
  CHECK_STR(jsonString(jsonGet(syms->first, "name")), "N");
  CHECK_INT(jsonInt(jsonGet(syms->first, "kind"), 0), 14);
  CHECK_STR(jsonString(jsonGet(syms->first->next, "name")), "start");
  CHECK_INT(jsonInt(jsonGet(syms->first->next, "kind"), 0), 12);

  request(s, 7, "textDocument/completion", uri, 0, 0);
  int found = 0;
  for (const Json *item = jsonGet(response(7), "result")->first; item != NULL; item = item->next) {
    const char *label = jsonString(jsonGet(item, "label"));
    found |= (strcmp(label, "ldi") == 0) | (strcmp(label, ".equ") == 0) << 1 | (strcmp(label, "start") == 0) << 2 |
             (strcmp(label, "N") == 0) << 3;
  }
  CHECK_INT(found, 15);
  stop(s);
}

static void testSettings(void)
{
  const char *uri = "file:///p/main.asm";
  LspServer *s = start("{\"rootUri\":\"file:///p\",\"initializationOptions\":{\"includePaths\":[\"inc\"],\"defines\":[\"FAST\"]}}");
  sendText(s, "textDocument/didOpen", uri, ".include \"consts.inc\"\n#ifdef FAST\nldi r16, LIMIT\n#else\nnosuch\n#endif\nrjmp 0\n");
  CHECK_INT(count(published(uri)), 0);
  forget();
  handle(s, "{\"jsonrpc\":\"2.0\",\"method\":\"workspace/didChangeConfiguration\",\"params\":{\"settings\":{\"avrasm\":{\"defines\":[]}}}}");
  CHECK_INT(count(published(uri)), 1);
  forget();
  handle(s, "{\"jsonrpc\":\"2.0\",\"method\":\"workspace/didChangeConfiguration\",\"params\":{\"settings\":{\"avrasm\":{\"includePaths\":[]}}}}");
  const Json *d = published(uri);
  CHECK(count(d) >= 1 && strstr(jsonString(jsonGet(d->first, "message")), "consts.inc") != NULL);
  stop(s);
}

static void testWindowsPaths(void)
{
  const char *uri = "file:///c%3A/Users/z/main.asm";
  LspServer *s = start("{\"rootUri\":\"file:///c%3A/Users/z\"}");
  sendText(s, "textDocument/didOpen", uri, ".include \"lib.inc\"\n.include \"C:\\Users\\z\\bad.inc\"\nrcall wait\n");
  CHECK_INT(count(published(uri)), 0);
  const Json *d = published("file:///c%3A/Users/z/bad.inc");
  CHECK_INT(count(d), 1);
  request(s, 1, "textDocument/definition", uri, 2, 8);
  CHECK_STR(jsonString(jsonGet(jsonGet(response(1), "result"), "uri")), "file:///c%3A/Users/z/lib.inc");
  stop(s);
}

int main(void)
{
  arenaInit(&arena, 0);
  testLifecycle();
  testDiagnostics();
  testIncludes();
  testRequests();
  testSettings();
  testWindowsPaths();
  arenaFree(&arena);
  return TEST_RESULT();
}