#include "avrasm/report.h"

#include <stdio.h>

#include "avrasm/diag.h"
#include "avrasm/source.h"

static const char *severityName(DiagSeverity s)
{
  switch (s) {
    case DIAG_ERROR: return "error";
    case DIAG_WARNING: return "warning";
    case DIAG_INFO: return "note";
  }
  return "internal error";
}

void reportDiags(FILE *out, const SourceMap *sources, const DiagList *diags)
{
  for (Diag *d = diags->head; d != NULL; d = d->next) {
    const SourceFile *f = sourceMapGet(sources, d->span.file);
    if (f == NULL) {
      fprintf(out, "%s: %s\n", severityName(d->severity), d->message);
      continue;
    }
    LineCol lc = sourceLineCol(f, d->span.start);
    fprintf(out, "%s:%u:%u: %s: %s\n", f->path, lc.line + 1, lc.col + 1, severityName(d->severity), d->message);
  }
}