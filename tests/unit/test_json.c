#include "avrasm/json.h"
#include "check.h"

#include <string.h>

static Arena arena;

static const Json *parse(const char *text)
{
  return jsonParse(&arena, text, strlen(text));
}

static void roundTrip(const char *in, const char *out)
{
  const Json *v = parse(in);
  if (v == NULL) {
    fprintf(stderr, "cannot parse %s\n", in);
    check_failures_++;
    return;
  }
  JsonBuf b = { NULL, 0, 0 };
  jsonValue(&b, v);
  CHECK_STR(b.data, out);
  jsonBufFree(&b);
}

static void testParse(void)
{
  const Json *v = parse(" {\"id\": 7, \"method\": \"initialize\", \"params\": {\"a\": [1, -2.5, 3e2, true, false, null]}} ");
  if (v == NULL) {
    check_failures_++;
    return;
  }
  CHECK_INT(v->type, JSON_OBJECT);
  CHECK_INT(jsonInt(jsonGet(v, "id"), -1), 7);
  CHECK_STR(jsonString(jsonGet(v, "method")), "initialize");
  const Json *a = jsonGet(jsonGet(v, "params"), "a");
  CHECK(a != NULL && a->type == JSON_ARRAY);
  const Json *e = a->first;
  CHECK_INT(jsonInt(e, 0), 1);
  e = e->next;
  CHECK(e->number == -2.5);
  CHECK_INT(jsonInt(e, 99), 99);
  e = e->next;
  CHECK_INT(jsonInt(e, 0), 300);
  e = e->next;
  CHECK(e->type == JSON_BOOL && e->boolean == 1);
  e = e->next;
  CHECK(e->type == JSON_BOOL && e->boolean == 0);
  e = e->next;
  CHECK(e->type == JSON_NULL && e->next == NULL);
  CHECK(jsonGet(v, "missing") == NULL);
  CHECK(jsonGet(a, "id") == NULL);
  CHECK(jsonString(jsonGet(v, "id")) == NULL);
  CHECK(jsonGet(NULL, "id") == NULL);
  CHECK(parse("[]")->first == NULL);
  CHECK(parse("{}")->first == NULL);
}

static void testStrings(void)
{
  const Json *v = parse("\"a\\\"b\\\\c\\/d\\n\\t\\u00e9\\ud83d\\ude00\"");
  CHECK(v != NULL);
  CHECK_STR(jsonString(v), "a\"b\\c/d\n\t\xC3\xA9\xF0\x9F\x98\x80");
  CHECK_INT(v->len, 15);
  CHECK_STR(jsonString(parse("\"\\ud800x\"")), "\xEF\xBF\xBDx");
  CHECK_STR(jsonString(parse("\"\\udc00\"")), "\xEF\xBF\xBD");
  v = parse("\"a\\u0000b\"");
  CHECK_INT(v->len, 3);
  CHECK(memcmp(v->text, "a\0b", 3) == 0);
  CHECK(jsonGet(parse("{\"k\\u0041\": 1}"), "kA") != NULL);
}

static void testBad(void)
{
  static const char *const bad[] = {
    "", "   ", "{", "[1,]", "{\"a\":1,}", "{a:1}", "01", "1.", ".5", "-", "1e", "+1", "tru", "nul",
    "\"abc", "\"a\nb\"", "\"\\x\"", "\"\\u12\"", "1 2", "[1 2]", "{\"a\" 1}", "{\"a\":}", "'a'",
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    if (parse(bad[i]) != NULL) {
      fprintf(stderr, "parsed, but should not: %s\n", bad[i]);
      check_failures_++;
    }
  }
  char deep[200];
  memset(deep, '[', 65);
  memset(deep + 65, ']', 65);
  CHECK(jsonParse(&arena, deep, 130) == NULL);
  CHECK(jsonParse(&arena, deep + 1, 128) != NULL);
}

static void testWrite(void)
{
  JsonBuf b = { NULL, 0, 0 };
  jsonStr(&b, "a\"b\\c\nd\x01\x7F", 9);
  CHECK_STR(b.data, "\"a\\\"b\\\\c\\nd\\u0001\\u007f\"");
  jsonBufFree(&b);
  jsonStr(&b, "\xC3\xA9 \xFF \xC3", 6);
  CHECK_STR(b.data, "\"\xC3\xA9 \xEF\xBF\xBD \xEF\xBF\xBD\"");
  jsonBufFree(&b);
  jsonf(&b, "{\"line\":%d}", 12);
  jsonRaw(&b, ",");
  jsonValue(&b, NULL);
  CHECK_STR(b.data, "{\"line\":12},null");
  jsonBufFree(&b);
  roundTrip(" { \"a\" : [ 1 , -2 , 3.5 , true , null ] , \"b\" : \"x\\\"y\" } ", "{\"a\":[1,-2,3.5,true,null],\"b\":\"x\\\"y\"}");
  roundTrip("\"id-7\"", "\"id-7\"");
}

int main(void)
{
  arenaInit(&arena, 0);
  testParse();
  testStrings();
  testBad();
  testWrite();
  arenaFree(&arena);
  return TEST_RESULT();
}