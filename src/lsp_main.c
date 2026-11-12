// the language server. JSON-RPC on stdin and stdout, each message after a
// "Content-Length: N" header and an empty line as LSP frames them. everything
// else is in lsp.c. messages for a person go to stderr which editors show in the logs.
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "avrasm/lsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LSP_MAX_MESSAGE (64u << 20)

static void sendMessage(void *user, const char *json, size_t len)
{
  (void)user;
  printf("Content-Length: %zu\r\n\r\n", len);
  fwrite(json, 1, len, stdout);
  fflush(stdout);
}

// does line start with a prefix, ignoring case
static int startsWithNoCase(const char *line, const char *prefix)
{
  for (; *prefix != '\0'; line++, prefix++) {
    char a = *line, b = *prefix;
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return 0;
  }
  return 1;
}

// reads the header lines up to the empty one. returns the Content-Length.
// -1 at the end of the input or when the header has none.
static long readHeader(void)
{
  char line[256];
  long length = -1;
  while(1) {
    if (fgets(line, sizeof line, stdin) == NULL) return -1;
    if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) return length;
    if (startsWithNoCase(line, "Content-Length:")) length = strtol(line + 15, NULL, 10);
  }
}

int main(void)
{
#ifdef _WIN32
  // Content-Length counts bytes. in text mode windows would turn "\r\n" into "\n" on the way in
  // and "\n" into "\r\n" on the way out and the counts would be wrong.
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  LspServer *s = lspCreate(avrDiskVfs(), sendMessage, NULL);
  while (lspExitCode(s) < 0) {
    long len = readHeader();
    if (len < 0 || (unsigned long)len > LSP_MAX_MESSAGE) {
      if (len >= 0) fprintf(stderr, "avrasm-lsp: a message of %ld bytes is too big\n", len);
      break;
    }
    char *body = malloc((size_t)len + 1);
    if (body == NULL || fread(body, 1, (size_t)len, stdin) != (size_t)len) {
      free(body);
      break;
    }
    lspHandle(s, body, (size_t)len);
    free(body);
  }
  int code = lspExitCode(s);
  lspFree(s);
  // the input ended without "exit"
  return code < 0 ? 1 : code;
}