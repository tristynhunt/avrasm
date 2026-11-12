# The language

Everything avrasm understands. It is the AVR assembly language of Microchip's AVRASM2 so existing programs assemble unchanged and give the same bytes with a few things being [checked more strictly](#stricter-checks). The instructions themselves are not listed here as Microchip's *AVR Instruction Set Manual* is the reference for those, and avrasm takes every instruction of every AVR.

## Lines and comments

One statement per line with an optional label, then an instruction or a directive, then its operands, separated by commas.

```asm
loop:   ldi r16, 0x10   ; a comment to the end of the line
        rjmp loop       // this one too
/* a block comment,
   over several lines */
```

A backslash at the very end of a line joins the next line to it (not inside a `;` comment). Upper and lower case are the same for every name: `LDI`, `Loop` and `R16` are `ldi`, `loop` and `r16`. File names in `.include` are as the file system has them, on Linux `M324ADEF.INC` is not `m324Adef.inc`.

## Names

Letters, digits and `_` which doesn't start with a digit. A name cannot be a mnemonic, a register (`r0` to `r31`, `X`, `Y`, `Z`), a function, or `pc`.

- **A label**, `name:`, is the address where it stands either as a word address in `.cseg` or a byte address in `.dseg` and `.eseg`. Only one label is allowed per line. A label defined inside a macro belongs to that one call of the macro and are not shared globally.
- `.equ`, `.set`, `.def` and macros make the other kinds of names (below).
- `pc` in an expression is the address of the instruction it is in.

## Numbers, characters and strings

| | |
| --- | --- |
| `42` | decimal |
| `0x2A`, `$2A` | hexadecimal |
| `0b101010` | binary |
| `052` | octal (a leading zero) |
| `0b1100_1010`, `1_000` | `_` between digits is ignored |
| `'A'` | a character: its code, 65 |
| `'\n'` | an escape: `\n` `\r` `\t` `\a` `\b` `\f` `\v` `\\` `\'`, `\x41` (one or two hex digits), `\101` (octal, up to 255). Any other character after `\` stands for itself |
| `"text"` | a string only allowed in `.db`. Is its bytes as they are |

**Strings have no escapes**: `"a\n"` is the three bytes `a`, `\`, `n`. avrasm warns when a backslash in a string looks like an escape (`\n`, `\t`, `\0`, `\\`, ...) and says what to write instead (for a line feed, `.db "text", 10`).

Numbers are 64-bit so a value used for a 32-bit thing must fit in 32 bits. Floating-point numbers are refused.

## Expressions

C's operators with C's precedence, from the loosest:

| | |
| --- | --- |
| `a ? b : c` | choice |
| `\|\|` | or (1 or 0) |
| `&&` | and (1 or 0) |
| `\|` | bitwise or |
| `^` | bitwise exclusive or |
| `&` | bitwise and |
| `==` `!=` | equal, not equal (1 or 0) |
| `<` `<=` `>` `>=` | comparisons (1 or 0) |
| `<<` `>>` | shifts (0 to 63) |
| `+` `-` | |
| `*` `/` `%` | |
| `-a` `~a` `!a` | negate, bitwise not, logical not |

And the functions:

| | |
| --- | --- |
| `low(x)`, `byte1(x)` | bits 0 to 7 |
| `high(x)`, `byte2(x)` | bits 8 to 15 |
| `byte3(x)`, `byte4(x)` | bits 16 to 23, 24 to 31 |
| `lwrd(x)`, `hwrd(x)` | bits 0 to 15, 16 to 31 |
| `page(x)` | bits 16 to 21 |
| `exp2(x)` | 2 to the power x (x from 0 to 31) |
| `log2(x)` | the highest set bit (x from 1) |
| `abs(x)` | |
| `strlen("text")` | the length of a string |
| `defined(name)` | 1 if the name is defined so far, else 0 (in `.if` and `#if`) |

An operand that must fit something is checked, so `ldi r16, 256` is an error with 8-bit immediates taking -128 to 255.

## Registers

`r0` to `r31`. `X`, `Y` and `Z` in `ld`, `st`, `ldd`, `std`, `lpm` and `spm`, with `+` after or `-` before (`X+`, `-Y`, `Z+q`). `adiw`, `sbiw` and `movw` take the low register of a pair or the pair itself (`r25:r24`). The device files give `XL`, `XH` to `ZH` as names for r26 to r31.

## Segments and addresses

| | |
| --- | --- |
| `.cseg` | program memory (flash), addresses are in words |
| `.dseg` | data memory (SRAM), addresses in bytes and it starts where the device's RAM starts |
| `.eseg` | EEPROM, addresses in bytes |
| `.org address` | where the next thing goes in the current segment |

Each segment has its own counter which is kept when you switch back to it. Two things written to the same address are an error. With a device file a program that does not fit the device's memory is an error.

## Data

| | |
| --- | --- |
| `.db 1, 'a', "text"` | bytes. In `.cseg` an odd number of them gets a zero byte after (with a warning) |
| `.dw 0x1234, label` | 16-bit words, low byte first |
| `.dd value` | 32-bit |
| `.dq value` | 64-bit |
| `.byte n` | reserve n bytes in `.dseg` (or `.eseg`) with nothing in them |

`.db`, `.dw`, `.dd` and `.dq` give values, so they go in `.cseg` or `.eseg`, in `.dseg` they are an error (there is nothing to put the values into: use `.byte`). 

## Names for values and registers

| | |
| --- | --- |
| `.equ NAME = expression` | a constant |
| `.set NAME = expression` | a variable of the assembler which can be set again. Each use sees the value it had at that point |
| `.def name = r16` | a name for a register |
| `.undef name` | the end of a `.def` (before giving the name another register) |

## Macros

```asm
.macro addi16           ; @0, @1: the register pair, @2: the value
    subi @0, low(-@2)
    sbci @1, high(-@2)
.endm                   ; or .endmacro

    addi16 r24, r25, 1000
```

`@0` to `@9` are the arguments of the call. A macro is defined before it is called. Labels in the body are private to each call.

## Conditions

```asm
.if FREQ > 8000000      ; also .ifdef NAME, .ifndef NAME
    ...
.elif FREQ > 1000000
    ...
.else
    ...
.endif
```

The condition sees what the program has defined **up to that line**.

## Including files

| | |
| --- | --- |
| `.include "file.inc"` | from the folder of the main file then the `-I` folders |
| `.include <file.inc>` | only from the `-I` folders |
| `.include "C:\avr\inc\file.inc"` | an absolute path only there (`\` and `/` are the same) |

A path is relative to the main file and not to the file that has the `.include`.

## Messages and the end

| | |
| --- | --- |
| `.message "text"` | prints it |
| `.warning "text"` | a warning |
| `.error "text"` | an error |
| `.exit` | the end of this file, the rest of it is not read |

## The device

`.device ATmega324A` names the chip and the device file (`.include "m324Adef.inc"`) gives its memory sizes and core. With a device file avrasm checks that the program fits the flash, the SRAM and the EEPROM, and that every instruction exists on that chip (no `mul` on an ATtiny, no `jmp` on an 8 KB part). `.device` without a device file only gets a warning that nothing can be checked.

## The preprocessor

As in C, before anything else on the line:

| | |
| --- | --- |
| `#define NAME text`, `#define NAME(a, b) text` | replaced wherever the name appears |
| `#undef NAME` | |
| `#if`, `#elif`, `#else`, `#endif`, `#ifdef`, `#ifndef` | with `defined(NAME)` |
| `#include "file"` | as `.include` |
| `#message`, `#warning`, `#error` | |
| `#pragma` | the device files' `#pragma AVRPART` lines are read, others get a warning that they are ignored |

`-D NAME` or `-D NAME=text` on the command line is a `#define` before the first line.

## Accepted, and doing nothing

`.list`, `.nolist`, `.listmac`: avrasm writes no listing (*yet*). `.overlap`, `.nooverlap` always error as two things at one address are always an error.

## Stricter checks

Some programs have a meaning that is probably not what was written. Other assemblers (AVRASM2 among them) take them keeping the low bits and wrapping round or carrying on after a warning. However, avrasm refuses them with an error that says what is wrong:

- **Values that do not fit**: a number of more than 64 bits, a value of more than 32 bits, `.dd` beyond 32 bits, `.dw` beyond 16, `.db` and 8-bit immediates beyond a byte (`ldi r16, ~0x80` is -129: write `low(~0x80)`), shift counts over 63, `exp2` over 31, `log2` of 0.
- **Floating point**: floating-point numbers, and `int`, `frac`, `q7` and `q15`.
- **Memory**: a program that does not fit the device, a jump past the end of the flash, an `.org` below the start of RAM or negative, a negative `.byte`, values in `.dseg` and RAM variables on a device without RAM are all errors.
- **Names**: more than one label on a line (`a: b: nop`), a `.def` that gives a name another register (`.undef` it first), `.equ` of a name that is only defined later, a `.set` name used above its first `.set`, a second `.device` with another name.
- **Instructions the chip does not have**, such as the XMEGA instructions `xch` and `des` on an ATmega.
- **Macros**: a call with more arguments than the macro uses, `@0` inside a name.
- **Strings**: `""` (empty) and a string compared in `#if`.
- **Files**: names are case-sensitive where the file system is.

A program that avrasm accepts always means the same in AVRASM2.

## Warnings for code that is probably wrong

After a program assembles without errors avrasm follows it the way the processor would run it from the reset address and the interrupt vectors and warns about:

- a skip (`sbrs`, `sbrc`, `sbis`, `sbic`, `cpse`) right before a macro call of more than one instruction has only its first instruction is skipped
- `lds` or `sts` with an address below 0x20, which is a register and not an I/O register (`sts PORTB, r16` writes r5)
- execution that runs past the end of the code or into `.db` data and jumps or calls to where there is no code
- code that nothing runs (only when the program has no `ijmp` or `icall`)
- a `ret` with bytes pushed and not popped (or popped and not pushed) and a loop that pushes more than it pops
- an interrupt handler that ends in `ret` instead of `reti` or one that changes the status flags before saving `SREG`
- the reset code running into a `ret`

`--no-lint` leaves these out but they also never change the output files.
