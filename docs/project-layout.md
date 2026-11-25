# Project layout

What lives where in the repository. Paths are relative to the repository root.

```
docs/requirements.sdoc    Software requirements specification (StrictDoc)
docs/storage-format.md    Novel storage format (RocksDB keys, format version, book details)
docs/project-layout.md    This file
dictionaries/             Spelling dictionaries (US and UK English, from LibreOffice) with their licenses
strictdoc_config.py       StrictDoc project configuration
tools/requirements-docs.txt  Documentation tooling (StrictDoc), installed into .venv
LICENSE                   GNU General Public License v3 (the project is GPL-3.0-or-later)
build.bat                 Windows: builds everything and creates the double-clickable dist\ folder
build.sh                  Linux: builds everything and creates the self-contained dist/ folder
conanfile.py              Conan dependencies (Qt, RocksDB, md4c, zlib, Hunspell); collects license files
profiles/windows-msvc     Conan profile for Windows (MSVC, C++20, Ninja)
CMakeLists.txt            Build definition
src/main.cpp              Application entry point
src/MainWindow.*          Main window: Manuscript/Synopsis/Plan tabs, structure tree, editors, autosave, menus
src/NovelTree.*           Act/chapter/scene tree with hierarchy-aware drag and drop
src/MarkdownHighlighter.* Live Markdown syntax highlighting and spelling underlines (plain-text document)
src/SpellChecker.*        Spell checking with Hunspell: dictionaries, personal dictionary, word splitting
src/Theme.*               Light, dark and system appearance
src/NovelSetupDialog.*    Novel Setup: book details for publishing, checked against Amazon KDP's rules
src/NovelStore.*          RocksDB novel storage (no Qt dependency)
src/NovelMarkdown.*       Markdown export and import of a whole novel (no Qt dependency)
src/Epub.*                EPUB 3 export (no Qt dependency; md4c for Markdown -> XHTML)
src/ZipWriter.*           Minimal ZIP writer (zlib) for EPUB files
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
tests/                    CTest suites: storage, Markdown, EPUB (+ optional EPUBCheck), LM Studio, chat, plan
                          parsing, prose prompts, settings, spelling and main window
```

The sources are built as four libraries plus the executable (see `CMakeLists.txt`), so tests can use them
without the application:

- **novelstore**: `NovelStore` and `NovelMarkdown`, storage and Markdown export/import without any Qt dependency.
- **novelepub**: EPUB export (md4c, zlib), without Qt.
- **novelai**: LM Studio client, chat completions, prompts, plan parsing, AI settings and the spell checker (Qt
  Core/Network only).
- **novelui**: the windows and widgets.
