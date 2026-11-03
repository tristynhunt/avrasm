#include "avrasm/source.h"
#include "avrasm/arena.h"

#include <stdint.h>
#include <string.h>

void sourceMapInit(SourceMap *m, Arena *arena)
{
  m->arena = arena;
  m->files = NULL;
  m->count = 0;
  m->cap = 0;
}

static int endsLine(const char *text, uint32_t len, uint32_t i)
{
  if (text[i] == '\n') return 1;
  if (text[i] == '\r' && (i + 1 == len || text[i + 1] != '\n')) return 1;
  return 0;
}

static uint32_t *buildLineStarts(Arena *arena, const char *text, uint32_t len, uint32_t *outCount)
{
  // pass 1: count lines
  uint32_t count = 1;
  for (uint32_t i = 0; i < len; i++) {
    count += endsLine(text, len, i);
  }
  uint32_t *starts = arenaAlloc(arena, sizeof(uint32_t) * count, _Alignof(uint32_t));

  starts[0] = 0;
  uint32_t line = 1;
  for (uint32_t i = 0; i < len; i++) {
    if (endsLine(text, len, i)) starts[line++] = i + 1; 
  }
  *outCount = count;
  
  return starts;
}

FileId sourceMapAdd(SourceMap *m, const char *path, const char *text, size_t len)
{
  if (len >= UINT32_MAX) return FILE_NONE;
  uint32_t n = (uint32_t)len;
  
  // allocate more space
  if (m->count == m->cap) {
    m->cap = m->cap ? m->cap * 2 : 8;
    SourceFile **newFiles = arenaAlloc(m->arena, sizeof(SourceFile *) * m->cap, _Alignof(SourceFile *));
    if (m->count > 0) memcpy(newFiles, m->files, m->count * sizeof *newFiles);
    m->files = newFiles;
  }

  // allocate source file and strings
  SourceFile *file = arenaAlloc(m->arena, sizeof(SourceFile), _Alignof(SourceFile));
  char *textCpy = arenaAlloc(m->arena, n + 1, 1);
  memcpy(textCpy, text, n);
  char *pathCpy = arenaStrdup(m->arena, path);

  // con-struct-ion
  file->text = textCpy;
  file->path = pathCpy;
  file->len = n;
  file->lineStarts = buildLineStarts(m->arena, textCpy, n, &file->lineCount);
  
  m->files[m->count] = file;
  return m->count++;
}

const SourceFile *sourceMapGet(const SourceMap *m, FileId id)
{
  return id >= m->count ? NULL : m->files[id];
}

LineCol sourceLineCol(const SourceFile *f, uint32_t offset)
{
  if (offset > f->len) offset = f->len;

  uint32_t lo = 0, hi = f->lineCount;
  while (lo < hi) {
    uint32_t mid = (hi - lo) / 2 + lo;
    if (f->lineStarts[mid] <= offset) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  uint32_t line = lo - 1, col = offset - f->lineStarts[line];
  return (LineCol){ line, col };
}