#include "avrasm/diag.h"
#include "avrasm/arena.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

void diagListInit(DiagList *l, Arena *arena)
{
  l->arena = arena;
  l->count = 0;
  l->errorCount = 0;
  l->head = NULL;
  l->tail = NULL;
}

void diagEmitV(DiagList *l, DiagSeverity severity, Span span, const char *fmt, va_list ap)
{
  va_list copy;
  va_copy(copy, ap);
  int len = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);

  char *msg;
  if (len < 0) {
    msg = arenaStrdup(l->arena, fmt);
  } else {
    msg = arenaAlloc(l->arena, len + 1, _Alignof(char));
    (void)vsnprintf(msg, len + 1, fmt, ap);
  }

  Diag *diag = arenaAlloc(l->arena, sizeof(Diag), _Alignof(Diag));
  diag->message = msg;
  diag->next = NULL;
  diag->severity = severity;
  diag->span = span;

  if (l->count != 0) l->tail->next = diag;
  else l->head = diag;
  l->tail = diag;
  l->count++;
  if (severity == DIAG_ERROR) l->errorCount++;
}

void diagEmit(DiagList *l, DiagSeverity severity, Span span, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  diagEmitV(l, severity, span, fmt, ap);
  va_end(ap);
}

int diagListHasErrors(const DiagList *l)
{
  return l->errorCount > 0;
}