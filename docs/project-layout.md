# Project layout

What lives where in the repository. Paths are relative to the repository root.

```
docs/requirements.sdoc    Software requirements specification (StrictDoc)
docs/storage-format.md    Novel storage format (RocksDB keys, format versions)
docs/project-layout.md    This file
strictdoc_config.py       StrictDoc project configuration
tools/requirements-docs.txt  Documentation tooling (StrictDoc), installed into .venv
LICENSE                   GNU General Public License v3 (the project is GPL-3.0-or-later)
build.bat                 Windows: builds everything and creates the double-clickable dist\ folder
build.sh                  Linux: builds everything and creates the self-contained dist/ folder
conanfile.py              Conan dependencies (Qt 6 qtbase incl. Network, RocksDB)
profiles/windows-msvc     Conan profile for Windows (MSVC, C++20, Ninja)
CMakeLists.txt            Build definition
src/main.cpp              Application entry point
src/MainWindow.*          Main window: Manuscript/Synopsis/Plan tabs, structure tree, editors, autosave
src/NovelTree.*           Act/chapter/scene tree with hierarchy-aware drag and drop
src/MarkdownHighlighter.* Live Markdown syntax highlighting (plain-text document)
src/NovelStore.*          RocksDB novel storage (no Qt dependency)
src/LmStudioClient.*      LM Studio client: fetches models (native REST API, with fallbacks)
src/AiSettings.*          Server address and model choices (QSettings)
src/SettingsDialog.*      Settings view
src/ModelCombo.*          Model dropdowns (shared by Settings and Synopsis views)
src/LmChat.*              Streamed chat completions (OpenAI-compatible), reasoning removal
src/PlanPrompt.*          The prompt that turns a synopsis into a plan
src/PlanChat.*            Plan refinement chat: prompt, reply parsing, history
src/PlanChatPanel.*       The chat box under the plan
src/PlanStructure.*       Reads acts/chapters/scenes (with briefs) out of a plan
src/ProsePrompt.*         Prose writer prompts: write a scene from its brief, expand a passage
tests/                    Storage, LM Studio, chat, plan parsing, prose prompts, settings and main window
                          tests (CTest)
```

The sources are built as three libraries plus the executable (see `CMakeLists.txt`), so tests can use them
without the application:

- **novelstore**: `NovelStore`, RocksDB storage without any Qt dependency.
- **novelai**: LM Studio client, chat completions, prompts, plan parsing and AI settings (Qt Core/Network only).
- **novelui**: the windows and widgets.
