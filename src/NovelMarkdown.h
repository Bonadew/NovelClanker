#pragma once

#include "NovelStore.h"

#include <string>
#include <string_view>
#include <vector>

// A whole novel as one Markdown file, and back.
//
// Export writes readable Markdown: acts as "# " headings, chapters as "## ", and scenes separated by "* * *".
// Each act, chapter and scene is also preceded by an HTML comment that Markdown viewers don't show
// ("<!-- act -->", "<!-- chapter -->", "<!-- scene: Title -->"). With these, import rebuilds the structure
// exactly: scene titles come back, and headings or "* * *" lines inside a scene's text stay part of the
// scene.
//
// Markdown without the comments (an older export, or written by hand) is read by its headings instead: "# "
// starts an act, "## " a chapter, and a scene-break line ("* * *", "***", "---") a new scene, named "Scene
// N". Text before the first heading goes into "Act 1" / "Chapter 1".
//
// Briefs are not exported, so imported novels have none.
namespace NovelMarkdown {

std::string write(const NovelStore &store);

// The structure and scene text in `markdown` (UTF-8; CRLF line ends are accepted). Empty if there is nothing.
std::vector<NovelStore::NewAct> read(std::string_view markdown);

} // namespace NovelMarkdown
