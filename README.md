# NovelWriter

A cross-platform Markdown-style editor for writing novels, built with C++20 and Qt 6 Widgets.
Runs on Windows, Linux, and macOS.

## Using it

- **File → New Novel...** creates a novel (with Act 1 / Chapter 1 / Scene 1), **File → Open Novel...** opens
  one (select the `.novel` folder). The last novel is reopened on startup; a novel path can also be passed on
  the command line.
- A novel is organised as **acts → chapters → scenes**, shown as a tree on the left. The **+ Act**,
  **+ Chapter** and **+ Scene** buttons above it add a node after the current one (missing parents are created,
  e.g. **+ Scene** on an empty act also adds a chapter). New acts and chapters open for renaming right away.
- Rename with double-click or **F2**, delete with **Del** (with confirmation), reorder with **Alt+Up/Down** or
  by dragging. Drag a scene onto another chapter, or a chapter onto another act, to move it there. The
  **Structure** menu and the tree's right-click menu have the same commands.
- Selecting a scene opens it in the editor. Scenes save automatically 1.5 s after you stop typing, when
  switching scenes, and on exit (**Ctrl+S** saves immediately). The status bar shows word counts for the
  scene, chapter and novel.
- **File → Export as Markdown...** writes the whole novel to a single `.md` file: acts and chapters become
  `#`/`##` headings, scenes are separated by `* * *`.

- **Edit → Settings...** sets the address of an [LM Studio](https://lmstudio.ai) server (default
  `http://127.0.0.1:1234`; addresses like `127.0.0.1:1234` or `.../v1` are accepted). The settings view
  connects to the server to list its models (embedding models are left out) in two dropdowns: one for the
  **orchestrator/planner** model and one for the **prose writer** model. Use **Refresh** after starting
  LM Studio's server. Settings are stored per user (QSettings), not in the novel.

## Storage

A novel is a [RocksDB](https://rocksdb.org) database, i.e. a folder (conventionally named `*.novel`).
**Every scene's text is a separate entry**; acts and chapters only hold a title and the ordered ids of their
children, so restructuring never rewrites scene text. Ids are unique across all kinds and never reused;
`<id>` is zero-padded to 20 digits:

| Key                   | Value                                   |
|-----------------------|-----------------------------------------|
| `novel/format`        | format version (`2`)                    |
| `novel/next_id`       | next unused id                          |
| `novel/acts`          | act ids in order, comma-separated       |
| `act/<id>/title`      | act title                               |
| `act/<id>/chapters`   | chapter ids in order                    |
| `chapter/<id>/title`  | chapter title                           |
| `chapter/<id>/scenes` | scene ids in order                      |
| `scene/<id>/title`    | scene title                             |
| `scene/<id>/text`     | scene Markdown                          |

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

Run the tests with `ctest --test-dir build/Release --output-on-failure`. They're self-contained (the LM Studio
tests use a fake server and the settings view runs headless). To also check the settings view against a real
LM Studio, set `NOVELWRITER_LIVE_LMSTUDIO=http://127.0.0.1:1234` and run `settings_test` directly.

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
conanfile.py              Conan dependencies (Qt 6 qtbase incl. Network, RocksDB)
profiles/windows-msvc     Conan profile for Windows (MSVC, C++20, Ninja)
CMakeLists.txt            Build definition
src/main.cpp              Application entry point
src/MainWindow.*          Main window: structure tree + add buttons, scene editor, autosave, word counts
src/NovelTree.*           Act/chapter/scene tree with hierarchy-aware drag and drop
src/MarkdownHighlighter.* Live Markdown syntax highlighting (plain-text document)
src/NovelStore.*          RocksDB novel storage (no Qt dependency)
src/LmStudioClient.*      LM Studio client: fetches models (native REST API, with fallbacks)
src/AiSettings.*          Server address and model choices (QSettings)
src/SettingsDialog.*      Settings view
tests/                    Storage, LM Studio client and settings view tests (run via CTest)
```
