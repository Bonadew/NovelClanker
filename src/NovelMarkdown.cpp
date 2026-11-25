#include "NovelMarkdown.h"

#include <cctype>
#include <optional>

namespace NovelMarkdown {

namespace {

using NewAct = NovelStore::NewAct;
using NewChapter = NovelStore::NewChapter;
using NewScene = NovelStore::NewScene;

constexpr std::string_view kActMarker = "<!-- act -->";
constexpr std::string_view kChapterMarker = "<!-- chapter -->";
constexpr std::string_view kScenePrefix = "<!-- scene: ";
constexpr std::string_view kCommentEnd = " -->";
constexpr std::string_view kSceneBreak = "* * *";

// "-->" would end the comment early; it can't appear in a title written into one.
std::string escapeTitle(std::string title)
{
    for (std::size_t at = 0; (at = title.find("-->", at)) != std::string::npos;) {
        title.replace(at, 3, "--&gt;");
    }
    return title;
}

std::string unescapeTitle(std::string title)
{
    for (std::size_t at = 0; (at = title.find("--&gt;", at)) != std::string::npos; at += 3) {
        title.replace(at, 6, "-->");
    }
    return title;
}

std::string trimRight(std::string_view text)
{
    std::size_t end = text.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(0, end));
}

std::string trim(std::string_view text)
{
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    return trimRight(text.substr(begin));
}

bool isBlank(std::string_view line)
{
    return trim(line).empty();
}

std::vector<std::string> splitLines(std::string_view text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string line(text.substr(start, end - start));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
}

// "# Title" -> "Title" for exactly `level` hashes; nullopt if the line isn't such a heading.
std::optional<std::string> heading(const std::string &line, std::size_t level)
{
    if (line.size() <= level || line.compare(0, level, std::string(level, '#')) != 0 || line[level] == '#' ||
        !std::isspace(static_cast<unsigned char>(line[level]))) {
        return std::nullopt;
    }
    return trim(std::string_view(line).substr(level));
}

// "* * *", "***", "---", "___" (three or more, spaces allowed): a scene break in hand-written Markdown.
bool isSceneBreak(const std::string &line)
{
    const std::string t = trim(line);
    if (t.empty() || (t[0] != '*' && t[0] != '-' && t[0] != '_')) {
        return false;
    }
    int marks = 0;
    for (const char c : t) {
        if (c == t[0]) {
            ++marks;
        } else if (c != ' ') {
            return false;
        }
    }
    return marks >= 3;
}

std::optional<std::string> sceneMarker(const std::string &line)
{
    const std::string t = trimRight(line);
    if (t.size() < kScenePrefix.size() + kCommentEnd.size() || t.compare(0, kScenePrefix.size(), kScenePrefix) != 0 ||
        t.compare(t.size() - kCommentEnd.size(), kCommentEnd.size(), kCommentEnd) != 0) {
        return std::nullopt;
    }
    return unescapeTitle(t.substr(kScenePrefix.size(), t.size() - kScenePrefix.size() - kCommentEnd.size()));
}

bool isMarker(const std::string &line)
{
    const std::string t = trimRight(line);
    return t == kActMarker || t == kChapterMarker || sceneMarker(t).has_value();
}

// Builds the structure as lines are read; acts and chapters are created when content needs them.
class Builder
{
  public:
    std::vector<NewAct> acts;

    void addAct(std::string title)
    {
        finishScene(false);
        acts.push_back({title.empty() ? "Act " + std::to_string(acts.size() + 1) : std::move(title), {}, {}});
    }

    void addChapter(std::string title)
    {
        finishScene(false);
        NewAct &act = currentAct();
        if (title.empty()) {
            title = "Chapter " + std::to_string(chapterCount() + 1);
        }
        act.chapters.push_back({std::move(title), {}, {}});
    }

    // `followsScene`: the previous scene in this chapter ends here (its trailing "* * *" is a separator).
    void addScene(std::string title, bool followsScene)
    {
        finishScene(followsScene);
        NewChapter &chapter = currentChapter();
        if (title.empty()) {
            title = "Scene " + std::to_string(chapter.scenes.size() + 1);
        }
        chapter.scenes.push_back({std::move(title), {}, {}});
        m_lines.clear();
        m_inScene = true;
    }

    bool inScene() const
    {
        return m_inScene;
    }

    void addLine(const std::string &line)
    {
        m_lines.push_back(line);
    }

    // Ends the open scene: its text is the collected lines without trailing blank lines (and without the "* * *"
    // separator that the export puts before the next scene of the chapter).
    void finishScene(bool beforeNextScene, bool trimLeading = false)
    {
        if (!m_inScene) {
            return;
        }
        const auto dropTrailingBlank = [this] {
            while (!m_lines.empty() && isBlank(m_lines.back())) {
                m_lines.pop_back();
            }
        };
        dropTrailingBlank();
        if (beforeNextScene && !m_lines.empty() && trim(m_lines.back()) == kSceneBreak) {
            m_lines.pop_back();
            dropTrailingBlank();
        }
        std::size_t first = 0;
        while (trimLeading && first < m_lines.size() && isBlank(m_lines[first])) {
            ++first;
        }
        std::string text;
        for (std::size_t i = first; i < m_lines.size(); ++i) {
            if (i > first) {
                text += '\n';
            }
            text += m_lines[i];
        }
        acts.back().chapters.back().scenes.back().text = std::move(text);
        m_lines.clear();
        m_inScene = false;
    }

  private:
    NewAct &currentAct()
    {
        if (acts.empty()) {
            acts.push_back({"Act 1", {}, {}});
        }
        return acts.back();
    }

    NewChapter &currentChapter()
    {
        NewAct &act = currentAct();
        if (act.chapters.empty()) {
            act.chapters.push_back({"Chapter " + std::to_string(chapterCount() + 1), {}, {}});
        }
        return act.chapters.back();
    }

    std::size_t chapterCount() const
    {
        std::size_t n = 0;
        for (const NewAct &act : acts) {
            n += act.chapters.size();
        }
        return n;
    }

    std::vector<std::string> m_lines;
    bool m_inScene = false;
};

// Exported by NovelClanker: the comments mark every act, chapter and scene.
std::vector<NewAct> readMarked(const std::vector<std::string> &lines)
{
    Builder b;
    enum class Expect { Nothing, ActHeading, ChapterHeading } expect = Expect::Nothing;
    for (const std::string &line : lines) {
        const std::string t = trimRight(line);
        if (t == kActMarker) {
            b.finishScene(false);
            expect = Expect::ActHeading;
            continue;
        }
        if (t == kChapterMarker) {
            b.finishScene(false);
            expect = Expect::ChapterHeading;
            continue;
        }
        if (const auto title = sceneMarker(t)) {
            b.addScene(*title, b.inScene());
            expect = Expect::Nothing;
            continue;
        }
        if (expect != Expect::Nothing) {
            if (isBlank(line)) {
                continue;
            }
            const std::size_t level = expect == Expect::ActHeading ? 1 : 2;
            std::string title = heading(line, level).value_or(trim(line));
            expect == Expect::ActHeading ? b.addAct(std::move(title)) : b.addChapter(std::move(title));
            expect = Expect::Nothing;
            continue;
        }
        if (b.inScene()) {
            b.addLine(line);
        } else if (!isBlank(line)) {
            b.addScene({}, false); // text outside any scene (edited by hand): keep it in a scene of its own
            b.addLine(line);
        }
    }
    b.finishScene(false);
    return b.acts;
}

// Plain Markdown: headings and scene breaks give the structure.
std::vector<NewAct> readPlain(const std::vector<std::string> &lines)
{
    Builder b;
    bool newScene = true; // the next text starts a scene
    for (const std::string &line : lines) {
        if (auto title = heading(line, 1)) {
            b.finishScene(false, true);
            b.addAct(std::move(*title));
            newScene = true;
        } else if (auto chapterTitle = heading(line, 2)) {
            b.finishScene(false, true);
            b.addChapter(std::move(*chapterTitle));
            newScene = true;
        } else if (isSceneBreak(line)) {
            b.finishScene(false, true);
            newScene = true;
        } else if (newScene) {
            if (!isBlank(line)) {
                b.addScene({}, false);
                b.addLine(line);
                newScene = false;
            }
        } else {
            b.addLine(line);
        }
    }
    b.finishScene(false, true);
    return b.acts;
}

} // namespace

std::string write(const NovelStore &store)
{
    std::string out;
    for (const auto &act : store.acts()) {
        out += std::string(kActMarker) + "\n# " + act.title + "\n\n";
        for (const auto &chapter : act.chapters) {
            out += std::string(kChapterMarker) + "\n## " + chapter.title + "\n\n";
            for (std::size_t s = 0; s < chapter.scenes.size(); ++s) {
                std::string text = store.sceneText(chapter.scenes[s].id);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
                    text.pop_back();
                }
                if (s > 0) {
                    out += std::string(kSceneBreak) + "\n\n";
                }
                out += std::string(kScenePrefix) + escapeTitle(chapter.scenes[s].title) + std::string(kCommentEnd) + "\n";
                out += text.empty() ? "\n" : text + "\n\n";
            }
        }
    }
    return out;
}

std::vector<NovelStore::NewAct> read(std::string_view markdown)
{
    if (markdown.size() >= 3 && markdown.substr(0, 3) == "\xEF\xBB\xBF") {
        markdown.remove_prefix(3); // UTF-8 byte order mark
    }
    const std::vector<std::string> lines = splitLines(markdown);
    for (const std::string &line : lines) {
        if (isMarker(line)) {
            return readMarked(lines);
        }
    }
    return readPlain(lines);
}

} // namespace NovelMarkdown
