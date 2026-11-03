#ifndef AVRASM_SOURCE_H
#define AVRASM_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include "avrasm/arena.h"

// everything that points at source text does it with a span
// the span is a file plus a byte range, so line and col numbers
// are never stored.


typedef uint32_t FileId;
#define FILE_NONE ((FileId)UINT32_MAX)

typedef struct Span
{
  FileId file;
  uint32_t start;
  uint32_t end;
} Span;

typedef struct LineCol
{
  uint32_t line;
  uint32_t col;
} LineCol;

typedef struct SourceFile
{
  const char *path;
  const char *text;
  uint32_t len;
  const uint32_t *lineStarts;
  uint32_t lineCount;
} SourceFile;

typedef struct SourceMap
{
  Arena *arena;
  SourceFile **files;
  uint32_t count;
  uint32_t cap;
} SourceMap;

void sourceMapInit(SourceMap *m, Arena *arena);

// copies path and text (len bytes) into the arena, builds the line table
// and returns the new file's id. ids are 0, 1, 2, ... in the order files
// are added.
FileId sourceMapAdd(SourceMap *m, const char *path, const char *text, size_t len);

// NULL if id is not a file in this map (including FILE_NONE). the returned
// pointer stays valid until the arena is freed, even if more files are added.
const SourceFile *sourceMapGet(const SourceMap *m, FileId id);

// line and col of a byte offset. an offset of f->len is valid
// and lands on the last line. bigger is clamped
LineCol sourceLineCol(const SourceFile *f, uint32_t offset);

#endif
