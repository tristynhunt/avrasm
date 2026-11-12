#ifndef AVRASM_LSP_H
#define AVRASM_LSP_H

#include <stddef.h>

#include "avrasm/ctx.h"

// the language server. 
// it takes one JSON-RPC message at a time and hands each message it sends back to 'send'.
//
// it assembles every open document as the editor has it (saved or not), and publishes the
// diagnostics. a document that another open document includes is shown as part of that program,
// so that a file of routines included by main.asm is checked with mains symbols.

typedef struct LspServer LspServer;

typedef void (*LspSend)(void *user, const char *json, size_t len);

// 'disk' reads the files that are not open in the editor
LspServer *lspCreate(AvrVfs disk, LspSend send, void *user);

void lspFree(LspServer *s);

// handles one message including the JSON text of a request, a notification or a response.
void lspHandle(LspServer *s, const char *json, size_t len);

// -1 until the editor sends "exit". after it the exit status is 0 if "shutdown" came first or 1 if not.
int lspExitCode(const LspServer *s);

#endif