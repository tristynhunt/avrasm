# Contributing

Issues and pull requests are welcome. Here is how the project is built and checked and the rules a change follows.

## Building and testing

```sh
make                 # build/avrasm, build/avrasm-lsp and build/libavrasm.a
make test            # every unit test in tests/unit
make SAN=1 test      # the same with AddressSanitizer and UBSan (after make clean)
make lspcheck        # talk to avrasm-lsp through stdin and stdout, as an editor does
make test-lexer      # one test: tests/unit/test_lexer.c
```

Any C11 compiler works (`make CC=clang`). On Windows, use MSYS2's UCRT64 shell. `SAN=1` needs gcc or clang on Linux, or clang on macOS.

## Where things are

| | |
| --- | --- |
| `src/lexer.c`, `tokline.c` | text to tokens, and lines of tokens |
| `src/expr.c`, `operand.c` | expressions and operands |
| `src/reader.c` | includes, macros, conditionals and the preprocessor |
| `src/assemble.c` | the two passes: addresses, then bytes |
| `src/instr.c`, `device.c` | the instruction table and encoder, device files |
| `src/image.c`, `hex.c` | memory images and Intel HEX |
| `src/lint.c` | the warnings about code that is probably wrong |
| `src/json.c`, `outline.c`, `lsp.c` | the language server |
| `src/main.c`, `lsp_main.c` | the two programs |

## The rules

- **Never silently wrong.** If what a line says and what it will do may differ it is a warning at least and an error when its meaning is wrong. Every message says what is wrong and, where it can, what to write instead.
- **Compatible.** A program avrasm accepts must mean the same byte for byte as in Microchip's AVRASM2, the language avrasm reads. Refusing something AVRASM2 accepts is fine when there is a reason (and the reason goes in the error message), accepting something it doesn't is not fine.
- **Every change has a test**, in `tests/unit`: a table row where the file has a table.
- **The style of the code around it**: 2 spaces, `camelCase` functions and fields, `PascalCase` types, `UPPER_SNAKE` macros, function braces on their own line, and C11 with no compiler extensions (except behind a check as `AVRASM_PRINTF` does).  *im very picky about code formatting :3*
