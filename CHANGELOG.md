# Changes

## [1.0.0] - 2026-11-13

The first public version.

- The assembler: every AVR instruction, directives, expressions, macros, conditionals, includes and the preprocessor. Device files with the memory sizes and instruction set checked. Intel HEX for program memory and EEPROM.
- Warnings for code that assembles but is probably wrong (`--no-lint` leaves them out).
- `avrasm-lsp`, a language server with diagnostics as you type, hover, go to definition, document symbols and completion.
- Builds on Linux, macOS and Windows with `make install`.
