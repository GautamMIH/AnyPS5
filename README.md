# About

Tool for automatic executables porting to Linux and Windows.

Includes a [relinker](core/relinker) that converts executable to the target system's native format and implementations of [system prx libraries](core/libs/prx) suitable for dynamic linking. No emulation or separate runtime process.

Releases will be published after the first full successful launch of at least one game.

## Status

Execution reaches `_start`, [stack unwinding](core/libs/prx/libc/src/exception/Unwind.cpp) and exception handling tables are built, reaches main. Unsupported or unexpected states strictly throw `std::runtime_error`. `what()` is printed to stderr and the process terminates.

The [shader recompiler](core/shader/recompiler/Recompiler.cpp) successfully produces validated via [Spirv-Tools](3rdparty/SPIRV-Tools) SPIR-V.

The real game reaches the logo, main menu, and [gameplay](https://gist.github.com/user-attachments/assets/81d28e9b-c237-4545-b2ca-720071129816) with audio.

[Technical debt of the project](docs/TechnicalDebt.md), [code style conventions](docs/CONVENTIONS.md)

## Build

The relinker uses only the C++20 standard library and should build with any conforming compiler.

[libc.prx](core/libs/prx/libc) implementations contain compiler-specific code. Linux builds work with GCC; on Windows, MinGW-w64 GCC 15.2.0 (`winlibs-gcc15`, `x86_64-ucrt-posix-seh`) is currently required.

The project targets maximum compiler portability. Support for additional compilers will be addressed after the first successful game launch.

## Usage

Build the `relinker` and `libs` targets, then relink a decrypted game dump:

```
relinker --game <game-dump-dir> <output-dir>
```

The relinker accepts plain ELF files as well as PS4/PS5 SELF containers with decrypted, uncompressed segments. It relinks `eboot.bin` into `eboot.elf` and every game-shipped `.prx`/`.sprx` module (outside `sce_sys`) into a native shared library in `<output-dir>/libs`. A module that AnyPS5 implements itself is replaced by the AnyPS5 library of the same name. The summary lists failed modules and libraries that nothing provides yet. `<output-dir>/app0` links to the dump, so the game runs from `<output-dir>`:

```
cd <output-dir> && ./eboot.elf
```

Imports are bound per PS library through ELF symbol versions. Imports that no library provides are bound to the generated `libs/aps5_unresolved.prx`; `<output-dir>/unresolved.txt` lists them grouped by PS library with the modules that use them. Calling an unimplemented or unresolved function throws; set `ANYPS5_REPORT_UNIMPLEMENTED=1` to log each one once and continue, which collects everything a game reaches in one run.

`--libs <dir>` selects the AnyPS5 prx directory when it is not found next to the relinker. A single module can still be relinked with `relinker <input> <output>`; library modules become shared objects with their exports, `DT_SONAME` and init/fini entry points.

## Disclaimer

This project is intended for interoperability, research, preservation, and compatibility purposes. It does not include, distribute, or require copyrighted software, firmware, cryptographic keys, or proprietary libraries. Users are responsible for ensuring that any binaries used with this project are obtained and used in accordance with applicable laws and their respective license terms.

## License

This project is licensed under the GNU General Public License version 2 only.
