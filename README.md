# Doc Generator

In my projects I wanted a way to generate a single markdown file for all of
the code in a library, which is checked into the repo and shipped with the
library. C API doc generators are very complicated and hard to use. This is
a single program which does exactly what I need.

This command-line utility walks one or more C headers or source files and
extracts their docstrings. The output includes per-file sections, structured summaries
for macros, types, and functions, and code snippets reproduced as fenced code
blocks.

The tool is powered by libclang, allowing it to parse real-world C projects
while respecting macros, typedefs, and other language constructs. It is
designed to be run directly against a project header from a terminal, making
it easy to regenerate documentation as part of a build or release pipeline.

**WARNING** - this tool is 100% LLM generated because it's tooling and I don't really
care about the code quality. I make no guarantees about the workability of this
code to the very broad range of possible C projects.

## Requirements

- macOS or Linux with a working C toolchain
- LLVM/Clang with libclang development headers available (Homebrew LLVM works out-of-the-box)

The bundled `build.sh` script automatically prefers a Homebrew LLVM installation 
(`/opt/homebrew/opt/llvm`) and falls back to the system `clang` if that toolchain 
is not available.

## Building

```sh
./build.sh
```

On success the script writes a `doc_gen` binary in the repository root.

If you need a custom toolchain, set `CLANG` and `LLVM_CONFIG` in your environment before running the script, for example:

```sh
LLVM_CONFIG=/usr/local/opt/llvm/bin/llvm-config CLANG=/usr/local/opt/llvm/bin/clang ./build.sh
```

## Usage

```sh
./doc_gen [options] <file.c|file.h>... [-- <clang-args...>]
```

Common options:

- `-h`, `--help` – Print usage information and exit.
- `--ignore PATTERN` – Skip any symbol whose name matches `PATTERN`. Patterns support `*` (match many characters) and `?` (match a single character). You can pass the flag multiple times to ignore several patterns.

### Example

```sh
./doc_gen --ignore "__GNU" library_main.h library_utils.h
```

The Markdown document is written to standard output. Redirect it to a file if you want to save the result:

```sh
./doc_gen --ignore "__GNU" library_main.h library_utils.h > API_DOCS.md
```

### Passing custom Clang arguments

If your project requires specific include paths or defines, supply them after a literal `--`. Everything following the separator is forwarded to libclang untouched.

```sh
./doc_gen my_header.h -- -Ithird_party/include -DMY_FEATURE=1
```

## License

This project is release into the public domain under the CC0. See LICENSE.md
