// Tests for NovelStore. Usage: novelstore_test <scratch-dir>
// The scratch directory is wiped and recreated on every run.

#include "NovelStore.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Id = NovelStore::Id;
using Kind = NovelStore::Kind;

namespace {

int g_failures = 0;

void expect(bool ok, const char *what, int line)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, what);
        ++g_failures;
    }
}

#define EXPECT(cond) expect((cond), #cond, __LINE__)

template <typename E> bool throws(const std::function<void()> &f)
{
    try {
        f();
    } catch (const E &) {
        return true;
    }
    return false;
}

// Compact description of the structure, e.g. "A1[C1(s1,s2)] A2[]".
std::string outline(const NovelStore &store)
{
    std::string out;
    for (const auto &act : store.acts()) {
        if (!out.empty()) {
            out += ' ';
        }
        out += act.title + '[';
        for (std::size_t c = 0; c < act.chapters.size(); ++c) {
            const auto &chapter = act.chapters[c];
            out += (c ? "," : "") + chapter.title + '(';
            for (std::size_t s = 0; s < chapter.scenes.size(); ++s) {
                out += (s ? "," : "") + chapter.scenes[s].title;
            }
            out += ')';
        }
        out += ']';
    }
    return out;
}

void testStructure(const fs::path &dir)
{
    const std::string unicodeText = "Text with åäö and emoji \xF0\x9F\x93\x96.\n";
    Id a1 = 0, a2 = 0, c1 = 0, c2 = 0, c3 = 0, s1 = 0, s2 = 0, s3 = 0;
    {
        NovelStore store(dir, NovelStore::Mode::Create);
        EXPECT(store.acts().empty());

        a2 = store.addAct(0, "A2");
        a1 = store.addAct(0, "A1");
        c1 = store.addChapter(a1, 0, "C1");
        c2 = store.addChapter(a1, 99, "C2"); // position clamps to the end
        c3 = store.addChapter(a2, 0, "C3");
        s2 = store.addScene(c1, 0, "s2", unicodeText);
        s1 = store.addScene(c1, 0, "s1");
        s3 = store.addScene(c3, 0, "s3", "three");
        EXPECT(outline(store) == "A1[C1(s1,s2),C2()] A2[C3(s3)]");

        EXPECT(store.kindOf(a1) == Kind::Act && store.kindOf(c2) == Kind::Chapter && store.kindOf(s3) == Kind::Scene);
        EXPECT(!store.kindOf(12345));
        EXPECT(store.parentOf(s2) == c1 && store.parentOf(c3) == a2 && !store.parentOf(a1));

        EXPECT(store.sceneText(s1).empty());
        EXPECT(store.sceneText(s2) == unicodeText);
        store.setSceneText(s1, "# Opening\n\nIt was a dark night.\n");
        store.rename(c2, "C2 renamed");
        EXPECT(outline(store) == "A1[C1(s1,s2),C2 renamed()] A2[C3(s3)]");

        // Moves: scene to another chapter (and act), chapter to another act, acts reordered.
        store.move(s2, c3, 0);
        EXPECT(outline(store) == "A1[C1(s1),C2 renamed()] A2[C3(s2,s3)]");
        store.move(c3, a1, 1);
        EXPECT(outline(store) == "A1[C1(s1),C3(s2,s3),C2 renamed()] A2[]");
        store.move(s3, c3, 0); // within the same chapter
        EXPECT(outline(store) == "A1[C1(s1),C3(s3,s2),C2 renamed()] A2[]");
        store.move(a2, 0, 0);
        EXPECT(outline(store) == "A2[] A1[C1(s1),C3(s3,s2),C2 renamed()]");

        // Wrong-kind targets are rejected and leave the structure untouched.
        EXPECT(throws<std::invalid_argument>([&] { store.move(s1, a1, 0); }));
        EXPECT(throws<std::invalid_argument>([&] { store.move(c1, c3, 0); }));
        EXPECT(throws<std::invalid_argument>([&] { store.move(a1, a2, 0); }));
        EXPECT(throws<std::invalid_argument>([&] { store.addScene(a1, 0, "x"); }));
        EXPECT(throws<std::invalid_argument>([&] { store.sceneText(c1); }));
        EXPECT(outline(store) == "A2[] A1[C1(s1),C3(s3,s2),C2 renamed()]");

        // RocksDB holds a lock: a second store on the same novel must fail.
        EXPECT(throws<StorageError>([&] { NovelStore second(dir, NovelStore::Mode::Open); }));
    }

    // The database must live at exactly this path (catches mangled non-ASCII names).
    EXPECT(fs::exists(dir / "CURRENT"));

    NovelStore store(dir, NovelStore::Mode::Open);
    EXPECT(outline(store) == "A2[] A1[C1(s1),C3(s3,s2),C2 renamed()]");
    EXPECT(store.sceneText(s1) == "# Opening\n\nIt was a dark night.\n");
    EXPECT(store.sceneText(s2) == unicodeText);
    EXPECT(store.sceneText(s3) == "three");

    // Ids are never reused.
    const Id s4 = store.addScene(c2, 0, "s4");
    EXPECT(s4 > s3 && s4 > c3 && s4 > a1);

    // Removing a chapter removes its scenes; removing an act removes everything below it.
    store.remove(c3);
    EXPECT(outline(store) == "A2[] A1[C1(s1),C2 renamed(s4)]");
    EXPECT(throws<std::invalid_argument>([&] { store.sceneText(s2); }));
    store.remove(s1);
    EXPECT(outline(store) == "A2[] A1[C1(),C2 renamed(s4)]");
    store.remove(a1);
    EXPECT(outline(store) == "A2[]");
    EXPECT(!store.kindOf(s4) && !store.kindOf(c2));
}

void testRemovedEntriesAreGone(const fs::path &dir)
{
    // After deleting everything and reopening, the novel must load cleanly (no dangling references).
    Id act = 0;
    {
        NovelStore store(dir, NovelStore::Mode::Create);
        act = store.addAct(0, "Act");
        const Id chapter = store.addChapter(act, 0, "Chapter");
        store.addScene(chapter, 0, "Scene", "text");
        store.remove(chapter);
    }
    NovelStore store(dir, NovelStore::Mode::Open);
    EXPECT(outline(store) == "Act[]");
}

void testErrors(const fs::path &root)
{
    const fs::path existing = root / "existing.novel";
    { NovelStore store(existing, NovelStore::Mode::Create); }
    EXPECT(throws<StorageError>([&] { NovelStore store(existing, NovelStore::Mode::Create); }));

    EXPECT(throws<StorageError>([&] { NovelStore store(root / "missing.novel", NovelStore::Mode::Open); }));
    EXPECT(!fs::exists(root / "missing.novel")); // opening must not create anything

    const fs::path notNovel = root / "not-a-novel";
    fs::create_directories(notNovel);
    EXPECT(throws<StorageError>([&] { NovelStore store(notNovel, NovelStore::Mode::Open); }));
    EXPECT(fs::is_empty(notNovel)); // and must not litter the folder
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }
    const fs::path root = argv[1];
    fs::remove_all(root);
    fs::create_directories(root);

    try {
        testStructure(root / "plain.novel");
        // Non-ASCII folder name (needs UTF-8 filename support in RocksDB on Windows).
        testStructure(root / fs::path(u8"Mitt Äventyr 小説.novel"));
        testRemovedEntriesAreGone(root / "removed.novel");
        testErrors(root);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++g_failures;
    }

    if (g_failures == 0) {
        std::puts("All NovelStore tests passed.");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
