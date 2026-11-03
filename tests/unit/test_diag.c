#include "avrasm/diag.h"
#include "check.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void wrapped(DiagList *l, Span span, const char *fmt, ...) AVRASM_PRINTF(3, 4);
static void wrapped(DiagList *l, Span span, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  diagEmitV(l, DIAG_ERROR, span, fmt, ap);
  va_end(ap);
}

static const Diag *nth(const DiagList *l, uint32_t i)
{
  const Diag *d = l->head;
  while (d != NULL && i > 0) {
    d = d->next;
    i--;
  }
  return d;
}

int main(void)
{
  Arena arena;
  arenaInit(&arena, 256);
  DiagList list;
  diagListInit(&list, &arena);

  CHECK(list.head == NULL);
  CHECK(list.tail == NULL);
  CHECK_INT(list.count, 0);
  CHECK(!diagListHasErrors(&list));

  Span at = { 0, 4, 7 };
  diagEmit(&list, DIAG_WARNING, at, "unused %s r%d", "register", 16);
  CHECK_INT(list.count, 1);
  CHECK_INT(list.errorCount, 0);
  CHECK(!diagListHasErrors(&list));
  REQUIRE(list.head != NULL);
  CHECK(list.head == list.tail);
  CHECK_INT(list.head->severity, DIAG_WARNING);
  CHECK_INT(list.head->span.file, 0);
  CHECK_INT(list.head->span.start, 4);
  CHECK_INT(list.head->span.end, 7);
  CHECK_STR(list.head->message, "unused register r16");
  CHECK(list.head->next == NULL);

  Span none = { FILE_NONE, 0, 0 };
  diagEmit(&list, DIAG_ERROR, none, "cannot open '%s'", "missing.inc");
  CHECK_INT(list.count, 2);
  CHECK_INT(list.errorCount, 1);
  CHECK(diagListHasErrors(&list));
  REQUIRE(nth(&list, 0) != NULL && nth(&list, 1) != NULL);
  CHECK_STR(nth(&list, 0)->message, "unused register r16");
  CHECK_STR(nth(&list, 1)->message, "cannot open 'missing.inc'");
  CHECK_INT(nth(&list, 1)->span.file, FILE_NONE);
  CHECK(list.tail == nth(&list, 1));
  CHECK(nth(&list, 2) == NULL);

  diagEmit(&list, DIAG_INFO, at, "previous definition was here");
  CHECK_INT(list.count, 3);
  CHECK_INT(list.errorCount, 1);

  diagEmit(&list, DIAG_ERROR, at, "%lld is out of range %d..%d (100%%)", -129LL, -128, 127);
  CHECK_STR(list.tail->message, "-129 is out of range -128..127 (100%)");
  diagEmit(&list, DIAG_WARNING, at, "%s", "");
  CHECK_STR(list.tail->message, "");

  char longText[5001];
  memset(longText, 'x', 5000);
  longText[5000] = '\0';
  diagEmit(&list, DIAG_ERROR, at, "[%s]", longText);
  REQUIRE(list.tail->message != NULL);
  REQUIRE(strlen(list.tail->message) == 5002);
  CHECK(list.tail->message[0] == '[' && list.tail->message[5001] == ']');
  CHECK(list.tail->message[2500] == 'x');

  char scratch[16] = "first";
  diagEmit(&list, DIAG_WARNING, at, "%s", scratch);
  const Diag *copied = list.tail;
  strcpy(scratch, "other");
  CHECK_STR(copied->message, "first");

  wrapped(&list, at, "%s has %d bytes", "buf", 42);
  CHECK_STR(list.tail->message, "buf has 42 bytes");
  CHECK_INT(list.tail->severity, DIAG_ERROR);

  DiagList many;
  diagListInit(&many, &arena);
  for (int i = 0; i < 1000; i++) {
    diagEmit(&many, i % 2 ? DIAG_ERROR : DIAG_WARNING, at, "d%d", i);
  }
  CHECK_INT(many.count, 1000);
  CHECK_INT(many.errorCount, 500);
  int orderOk = 1;
  uint32_t seen = 0;
  for (const Diag *d = many.head; d != NULL; d = d->next) {
    char want[16];
    snprintf(want, sizeof want, "d%u", seen);
    if (strcmp(d->message, want) != 0) orderOk = 0;
    seen++;
  }
  CHECK(orderOk);
  CHECK_INT(seen, 1000);
  CHECK(many.tail != NULL && strcmp(many.tail->message, "d999") == 0);

  arenaFree(&arena);
  return TEST_RESULT();
}