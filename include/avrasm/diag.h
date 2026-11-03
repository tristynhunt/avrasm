#ifndef AVRASM_DIAG_H
#define AVRASM_DIAG_H

#include <stdarg.h>
#include <stdint.h>

#include "avrasm/arena.h"
#include "avrasm/source.h"

// lets the compiler check printf style args at every call site
#if defined(__GNUC__) || defined(__clang__)
#define AVRASM_PRINTF(fmtArg, firstArg) __attribute__((format(printf, fmtArg, firstArg)))
#else
#define AVRASM_PRINTF(fmtArg, firstArg)
#endif

typedef enum DiagSeverity
{
  DIAG_ERROR,
  DIAG_WARNING,
  DIAG_INFO
} DiagSeverity;

typedef struct Diag
{
  DiagSeverity severity;
  Span span;
  const char *message;
  struct Diag *next;
} Diag;

typedef struct DiagList
{
  Arena *arena;
  Diag *head;
  Diag *tail;
  uint32_t count;
  uint32_t errorCount;
} DiagList;

void diagListInit(DiagList *l, Arena *arena);

// formats the message printf style into arena and appends a new Diag at the end
void diagEmit(DiagList *l, DiagSeverity severity, Span span, const char *fmt, ...)
  AVRASM_PRINTF(4, 5);

// same but for wrappers which already hold a va_list
void diagEmitV(DiagList *l, DiagSeverity severity, Span span, const char *fmt, va_list ap)
  AVRASM_PRINTF(4, 0);

// true if at least one DIAG_ERROR was emitted
int diagListHasErrors(const DiagList *l);

#endif