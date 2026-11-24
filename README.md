# NovelWriter

A cross-platform Markdown-style editor for writing novels, built with C++20 and Qt 6 Widgets.
Runs on Windows, Linux, and macOS.

## Using it

- **File → New Novel...** creates a novel, **File → Open Novel...** opens one (select the `.novel` folder).
  The last novel is reopened on startup; a novel path can also be passed on the command line.
- The chapter list on the left shows each chapter's title: its first line, minus `#` heading markers.
  Add, delete and reorder chapters from the **Chapter** menu, or drag chapters in the list.
- Chapters save automatically 1.5 s after you stop typing, when switching chapters, and on exit
  (**Ctrl+S** saves immediately).
- **File → Export as Markdown...** writes the whole novel to a single `.md` file.

## Storage

A novel is a [RocksDB](https://rocksdb.org) database, i.e. a folder (conventionally named `*.novel`).
Each chapter is a separate entry holding that chapter's Markdown; the reading order is a separate entry, so
reordering never rewrites chapter text:

| Key                     | Value                                              |
|-------------------------|----------------------------------------------------|
| `novel/format`          | format version (`1`)                               |
| `novel/next_chapter_id` | next unused chapter id                             |
| `novel/chapter_order`   | chapter ids in reading order, comma-separated      |
| `chapter/<id>`          | chapter Markdown (`<id>` zero-padded to 20 digits) |

Writes are synced to disk and multi-key changes are atomic (RocksDB write batches). RocksDB locks an open
novel, so only one NovelWriter can have it open at a time. See `src/NovelStore.h`.

## Requirements

- A C++20 compiler (MSVC 2022+, GCC 11+, Clang 14+ / Apple Clang)
- CMake 3.23+
- [Conan 2](https://conan.io) (`pip install conan`)
- Linux only: Qt needs X11/OpenGL system packages. Let Conan install them by adding
  `-c tools.system.package_manager:mode=install` to `conan install`.

## Building

The first `conan install` builds Qt and RocksDB from source (ConanCenter has no matching prebuilt binaries),
which can take an hour or more. The result is cached in `~/.conan2`, so later installs are fast.

### Windows (MSVC + Ninja)

A ready-made profile is in `profiles/windows-msvc` (Visual Studio 2026 / MSVC 19.5, C++20, Ninja).
Edit `compiler.version` if you use another Visual Studio version (194 = VS 2022). The profile also builds
RocksDB with UTF-8 file names so novels can live in folders with non-ASCII names.

```bat
conan install . -pr:a ./profiles/windows-msvc --build=missing

:: Ninja needs the MSVC environment; conanbuild.bat activates it (vcvars)
build\Release\generators\conanbuild.bat
cmake --preset conan-release
cmake --build --preset conan-release
```

Each build copies the Qt DLLs and plugins next to the executable (via `windeployqt`), so
`build\Release\NovelWriter.exe` starts directly: double-click it, or run it from an IDE or debugger.

If your Windows user folder contains a space (e.g. `C:\Users\First Last`), move Conan's package storage to a
path without spaces first, since several dependency build tools break on spaces. Add this to
`~/.conan2/global.conf`:

```
core.cache:storage_path = E:/conan2/p
```

### Linux / macOS

```sh
conan profile detect            # one-time: create a default profile for your compiler
conan install . -s compiler.cppstd=20 --build=missing
cmake --preset conan-release
cmake --build --preset conan-release
./build/Release/NovelWriter      # macOS: open build/Release/NovelWriter.app
```

Run the storage tests with `ctest --test-dir build/Release --output-on-failure`.

For a Debug build, run `conan install` again with `-s build_type=Debug` and use the `conan-debug` presets.

### Distributable app

```sh
cmake --install build/Release --prefix dist
```

On Windows, `dist/` becomes a self-contained folder (Qt and the MSVC runtime included) that runs on machines
without any SDKs. On macOS, the install step runs `macdeployqt` on `dist/NovelWriter.app` to bundle Qt (not yet
tested). Linux packaging (e.g. AppImage) isn't set up yet; run the binary from the build tree there.

## Project layout

```
conanfile.py              Conan dependencies (Qt 6 qtbase, RocksDB)
profiles/windows-msvc     Conan profile for Windows (MSVC, C++20, Ninja)
CMakeLists.txt            Build definition
src/main.cpp              Application entry point
src/MainWindow.*          Main window: chapter list, editor, autosave, menus, word counts
src/MarkdownHighlighter.* Live Markdown syntax highlighting (plain-text document)
src/NovelStore.*          RocksDB novel storage (no Qt dependency)
tests/NovelStoreTest.cpp  Storage tests (run via CTest)
```
