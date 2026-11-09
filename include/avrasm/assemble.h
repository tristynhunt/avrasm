#ifndef AVRASM_ASSEMBLE_H
#define AVRASM_ASSEMBLE_H

#include "avrasm/ctx.h"
#include "avrasm/image.h"
#include "avrasm/source.h"

// assemble the already loaded source file 'file' into 'image'.
// every message goes to ctx-diags. returns 1 if there was no error.
// the image is only meaningful if no errors occured.
int assemble(AvrCtx *ctx, FileId file, Image *image);

// the size of the program mem image in bytes for now.
// this will be set later with device directives
#define ASSEMBLE_DEFAULT_IMAGE_SIZE (1u << 17)

#endif