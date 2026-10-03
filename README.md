# About

Tool for automatic executables porting to Linux and Windows.

Includes a [relinker](core/relinker) that converts executable to the target system's native format and implementations of [system prx libraries](core/libs/prx) suitable for dynamic linking. No emulation or separate runtime process.

[Usage](docs/user/USAGE.md), [Build instructions](docs/dev/BUILD.md), [Technical debt of the project](docs/dev/TechnicalDebt.md), [code style conventions](docs/dev/CONVENTIONS.md), [contributing](CONTRIBUTING.md)

## Status

[![libraries](https://boykopovar.github.io/AnyPS5/badge-libraries.svg)](https://boykopovar.github.io/AnyPS5/) [![shaders](https://boykopovar.github.io/AnyPS5/badge-shaders.svg)](https://boykopovar.github.io/AnyPS5/)

[![progress map](https://boykopovar.github.io/AnyPS5/progress.svg)](https://boykopovar.github.io/AnyPS5/)

<sub>* System libraries: percentage of the functions known to the project so far (declared in [core/libs/prx](core/libs/prx)), not of every PS5 system function. The total grows as more functions are declared.</sub>

[List of verified games](docs/user/COMPATIBILITY.md)

Dreaming Sarah (2D platformer) runs at a stable 60 fps on a GTX 1050 Ti / i5-7500 3.4GHz.

Unsupported or unexpected states strictly throw `std::runtime_error`. `what()` is printed to stderr and the process terminates.

The [shader recompiler](core/shader/recompiler/Recompiler.cpp) successfully produces SPIR-V (validated via [Spirv-Tools](3rdparty/SPIRV-Tools) when built with `ANYPS5_ENABLE_SPIRV_TOOLS`).

[Technical debt of the project](docs/dev/TechnicalDebt.md), [code style conventions](docs/dev/CONVENTIONS.md), [contributing](CONTRIBUTING.md)

## Build

The relinker uses only the C++20 standard library and should build with any conforming compiler.

On Intel hosts, pass `--to-intel` to the relinker to lower supported AMD-only instructions in the executable and bundled `sce_module`/`sce_modules` PRX files. Unsupported instructions or stub jumps outside the x86-64 relative branch range produce an error.

[libc.prx](core/libs/prx/libc) implementations contain compiler-specific code. Linux builds work with GCC; on Windows, MinGW-w64 GCC 15.2.0 (`winlibs-gcc15`, `x86_64-ucrt-posix-seh`) is currently required.

The project targets maximum compiler portability (but now it is not implemented).

## Usage

Build the `relinker` and `libs` targets, then relink a decrypted game dump:

```
relinker --game <game-dump-dir> <output-dir>
```

The relinker accepts plain ELF files as well as PS4/PS5 SELF containers with decrypted, uncompressed segments. It relinks `eboot.bin` into `eboot.elf` and every game-shipped `.prx`/`.sprx` module (outside `sce_sys`) into a native shared library in `<output-dir>/libs`. When AnyPS5 also provides a module the game ships, the relinker picks one copy: `fakelib/` placeholders always give way to AnyPS5; for a PS5 system library (`libc`, `libkernel*`, `libSce*`) the AnyPS5 library is used unless the game's copy resolves more of the game's imports; game middleware (for example FMOD) is replaced only by an AnyPS5 library that resolves every import the game uses. The summary lists each choice. The summary lists failed modules and libraries that nothing provides yet. `<output-dir>/app0` links to the dump, so the game runs from `<output-dir>`:

```
cd <output-dir> && ./eboot.elf
```

Imports are bound per PS library through ELF symbol versions. Imports that no library provides are bound to the generated `libs/aps5_unresolved.prx`; `<output-dir>/unresolved.txt` lists them grouped by PS library with the modules that use them. Calling an unimplemented or unresolved function throws; set `ANYPS5_REPORT_UNIMPLEMENTED=1` to log each one once and continue, which collects everything a game reaches in one run.

`--libs <dir>` selects the AnyPS5 prx directory when it is not found next to the relinker. A single module can still be relinked with `relinker <input> <output>`; library modules become shared objects with their exports, `DT_SONAME` and init/fini entry points.

## Compatibility

See the [game compatibility list](docs/user/COMPATIBILITY.md) for tested games and known issues.

## Input mapping

SDL-mapped game controllers are supported, including analog sticks and triggers. Keyboard and mouse controls can be configured with an `anyps5-input.ini` file. See [input mapping](docs/user/INPUT_MAPPING.md) for the supported devices and configuration format.

## Disclaimer

This project is intended for interoperability, research, preservation, and compatibility purposes. It does not include, distribute, or require copyrighted software, firmware, cryptographic keys, or proprietary libraries. Users are responsible for ensuring that any binaries used with this project are obtained and used in accordance with applicable laws and their respective license terms.

## License

This project is licensed under the GNU General Public License version 2 only.
