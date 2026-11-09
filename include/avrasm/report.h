#ifndef AVRASM_REPORT_H
#define AVRASM_REPORT_H

#include <stdio.h>

#include "avrasm/diag.h"
#include "avrasm/source.h"

// prints every diagnostic in the order it was emitted using the same format
// gcc errors use.
void reportDiags(FILE *out, const SourceMap *sources, const DiagList *diags);

#endif