# avrasm

An assembler for AVR microcontrollers, with a language server for editors and a ~~VS Code extension~~(*coming soon*).

avrasm reads the AVR assembly language that Microchip's AVRASM2 uses and was made to give the same results. Existing programs assemble unchanged into the same `.hex` files. It's stricter where a program's meaning is probably not what was written and says why with errors and warnings that point at the exact place.

It was written primarily for teaching and as a resource when learning and works for any AVR program in that language.

## What it does

- Every AVR instruction, directives, expressions, macros, conditionals, includes and a C-like preprocessor (`#define`, `#if`, ...).
- Device files (`.include "m324Adef.inc"`): the memory sizes, and which instructions the chip has, are checked.
- Intel HEX for the program memory and the EEPROM.
- Clear errors, at the exact column, nothing is silently changed or dropped.
- Warnings for code that assembles but is probably wrong: a routine whose `push` and `pop` do not balance, an interrupt handler that ends in `ret` or changes the flags before saving `SREG`, a main loop that runs off its end or into data, `sts PORTB, r16` (which writes r5), and more.
- `avrasm-lsp`: a language server for any editor that can implement LSP. It gives errors as you type, hover, go to definition, document symbols and completion.

## Install

~~**VS Code**: install **AVR Assembly (avrasm)** from the Marketplace or Open VSX. It has both programs for Windows, macOS and Linux inside, and a Build button.~~ **(IN PROGRESS)**

**The programs**: download them for your system from [Releases](https://github.com/tristynhunt/avrasm/releases): Windows (x64, arm64), macOS (Intel, Apple Silicon), Linux (x64, arm64 and static, for any distribution). Unpack, and put `avrasm` and `avrasm-lsp` in a folder on your `PATH`. On macOS, a program downloaded with a browser is quarantined until you allow it:

```sh
xattr -d com.apple.quarantine avrasm avrasm-lsp
```

**From the source**: a C11 compiler and `make`.

- Linux: gcc or clang.
- macOS: the Xcode command line tools, `xcode-select --install`.
- Windows: [MSYS2](https://www.msys2.org)'s UCRT64 shell, with `pacman -S make mingw-w64-ucrt-x86_64-gcc`.

```sh
make
make test
make install PREFIX=$HOME/.local    # or sudo make install, for /usr/local
```

## Use

```sh
avrasm prog.asm                     # writes prog.hex
avrasm -e prog.eep.hex prog.asm     # and the EEPROM, when the program has EEPROM data
avrasm -I path/to/inc prog.asm      # where .include looks after the folder of prog.asm
```

| Option | |
| --- | --- |
| `-o file` | the program memory as Intel HEX (default: `prog.hex` next to `prog.asm`) |
| `-e file` | the EEPROM as Intel HEX, when the program has EEPROM data |
| `-I dir` | another folder for `.include`, repeatable |
| `-D name[=value]` | define a preprocessor name, as `#define` does (value 1 if left out), repeatable |
| `-fI` | Intel HEX, the only format (accepted so that existing command lines work) |
| `--no-lint` | leave out the warnings about code that is probably wrong |
| `--image` | print the program memory as text |
| `-h`, `--version` | |

The exit status is 0 when the program assembled (with or without warnings), 1 when it has errors (no file is written then, and old ones are removed), 2 for a bad command line or a file that cannot be written.

## Device files

Programs usually start with a line such as `.include "m324Adef.inc"` which includes the names of the chip's registers and bits, and its memory sizes. These files are Microchip's and are not part of avrasm. They come with Microchip Studio (`...\Atmel\Studio\7.0\packs\atmel\ATmega_DFP\<version>\avrasm\inc`), and in the **ATmega_DFP** pack from [Microchip's packs repository](https://packs.download.microchip.com): a `.atpack` is a zip file with them in `avrasm/inc/`. Point `-I` (or the extension's include paths) at that folder.

## Editors

- ~~**VS Code**: the extension.~~
- **Any other editor with LSP**: run `avrasm-lsp` (it talks on stdin and stdout) for AVR assembly files. It takes `{"includePaths": [...], "defines": [...]}` as its initialization options, and again as the `avrasm` section of `workspace/didChangeConfiguration`.

## Documentation

- [The language](docs/language.md): everything avrasm understands.
- [Contributing](CONTRIBUTING.md): building, the tests, and the rules a change follows.
- [Changes](CHANGELOG.md).

## How it is checked

`make test` runs the unit tests of every part from the lexer to the language server (`make SAN=1 test` runs them again with AddressSanitizer and UBSan) and `make lspcheck` talks to `avrasm-lsp` the way an editor does. A GitHub workflow runs them on Linux, macOS and Windows for every push.

While it was written avrasm's output was compared with AVRASM2 2.2.8 for more than 700 programs, with the `.hex` files checked byte for byte. avrasm refuses some programs that AVRASM2 would otherwise accept and assemble fine. This is on purpose however, as avrasm has been developed to be stricter ([the list](docs/language.md#stricter-checks)). A program avrasm accepts always assembles the same way in both.

## License

MIT: see [LICENSE](LICENSE). avrasm is an independent project, not made or endorsed by Microchip. AVR and AVRASM2 are Microchip's trademarks.
