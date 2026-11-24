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
using Ids = std::vector<NovelStore::ChapterId>;

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

bool throwsStorageError(const std::function<void()> &f)
{
    try {
        f();
    } catch (const StorageError &) {
        return true;
    }
    return false;
}

void testCreateEditReopen(const fs::path &dir)
{
    NovelStore::ChapterId one = 0, two = 0, three = 0;
    {
        NovelStore store(dir, NovelStore::Mode::Create);
        EXPECT(store.chapters().empty());

        one = store.insertChapter(0, "# One\n");
        three = store.insertChapter(1, "# Three\n");
        two = store.insertChapter(1, "# Two\n");
        EXPECT((store.chapters() == Ids{one, two, three}));
        EXPECT(store.insertChapter(99, "# Four\n") != 0); // position clamps to the end
        EXPECT(store.chapters().size() == 4);
        store.removeChapter(store.chapters().back());

        store.setChapterText(two, "# Two\n\nEdited text with åäö and emoji \xF0\x9F\x93\x96.\n");
        store.moveChapter(2, 0); // Three, One, Two
        EXPECT((store.chapters() == Ids{three, one, two}));
        store.moveChapter(0, 5); // out of range: ignored
        EXPECT((store.chapters() == Ids{three, one, two}));

        // RocksDB holds a lock: a second store on the same novel must fail.
        EXPECT(throwsStorageError([&] { NovelStore second(dir, NovelStore::Mode::Open); }));
    }

    // The database must live at exactly this path (catches mangled non-ASCII names).
    EXPECT(fs::exists(dir / "CURRENT"));

    NovelStore store(dir, NovelStore::Mode::Open);
    EXPECT((store.chapters() == Ids{three, one, two}));
    EXPECT(store.chapterText(two) == "# Two\n\nEdited text with åäö and emoji \xF0\x9F\x93\x96.\n");
    EXPECT(store.chapterText(one) == "# One\n");

    // Ids are never reused, even after deleting the newest chapter.
    const auto four = store.insertChapter(3, "");
    EXPECT(four > three && four > two);
    EXPECT(store.chapterText(four).empty());

    store.removeChapter(one);
    EXPECT((store.chapters() == Ids{three, two, four}));
    EXPECT(throwsStorageError([&] { store.chapterText(one); }));
}

void testErrors(const fs::path &root)
{
    const fs::path existing = root / "existing.novel";
    { NovelStore store(existing, NovelStore::Mode::Create); }
    EXPECT(throwsStorageError([&] { NovelStore store(existing, NovelStore::Mode::Create); }));

    EXPECT(throwsStorageError([&] { NovelStore store(root / "missing.novel", NovelStore::Mode::Open); }));
    EXPECT(!fs::exists(root / "missing.novel")); // opening must not create anything

    const fs::path notNovel = root / "not-a-novel";
    fs::create_directories(notNovel);
    EXPECT(throwsStorageError([&] { NovelStore store(notNovel, NovelStore::Mode::Open); }));
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
        testCreateEditReopen(root / "plain.novel");
        // Non-ASCII folder name (needs UTF-8 filename support in RocksDB on Windows).
        testCreateEditReopen(root / fs::path(u8"Mitt Äventyr 小説.novel"));
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
