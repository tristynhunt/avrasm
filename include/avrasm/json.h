#ifndef AVRASM_JSON_H
#define AVRASM_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "avrasm/arena.h"
#include "avrasm/diag.h"  // AVRASM_PRINTF

// JSON for the language server. a parser that builds a tree in an arena and a writer
// that appends to a growing buffer. only what JSON-RPC needs. nothing here knows about LSP.

typedef enum JsonType {
  JSON_NULL,
  JSON_BOOL,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT
} JsonType;

typedef struct Json {
  JsonType type;
  int boolean;         // JSON_BOOL
  double number;       // JSON_NUMBER
  const char *text;    // JSON_STRING: the decoded text (UTF-8), NUL-terminated
  size_t len;          // JSON_STRING: its length in bytes (a \u0000 inside makes it longer than strlen)
  const char *key;     // a member of an object: its decoded name, NULL in an array or at the top
  size_t keyLen;
  struct Json *first;  // JSON_ARRAY and JSON_OBJECT: the first element or member, NULL if empty
  struct Json *next;   // the next element or member of the same array or object
} Json;

// parses text, which must be exactly one JSON value (white space is fine).
// returns NULL if it is not or if it nests deeper than 64 arrays and objects.
Json *jsonParse(Arena *arena, const char *text, size_t len);

// the member of object 'v' called 'key'. NULL if v is NULL, is not an object, or has no such member.
const Json *jsonGet(const Json *v, const char *key);

// the text of a string. NULL if v is NULL or not a string.
const char *jsonString(const Json *v);

// a number as an integer. 'fallback' if v is NULL, not a number, or not a whole number that a
// double holds exactly (up to 2^53).
int64_t jsonInt(const Json *v, int64_t fallback);

// ---- writing ----

// text that grows as it is written to. data is NUL-terminated once
// anything has been written. out of memory is fatal as in the arena.
typedef struct JsonBuf {
  char *data;
  size_t len;
  size_t cap;
} JsonBuf;

void jsonBufFree(JsonBuf *b);

// appends text as it is.
void jsonRaw(JsonBuf *b, const char *text);

// appends printf-style as it is (for numbers and fixed JSON, never for text from outside).
void jsonf(JsonBuf *b, const char *fmt, ...) AVRASM_PRINTF(2, 3);

// appends 's' as a JSON string being quoted and escaped. bytes that are not valid UTF-8 become
// U+FFFD so the output is valid UTF-8 whatever the source file holds.
void jsonStr(JsonBuf *b, const char *s, size_t len);

// appends a parsed value as JSON again (the id of a request goes back in its response this way).
void jsonValue(JsonBuf *b, const Json *v);

#endif