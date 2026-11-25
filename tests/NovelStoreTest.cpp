// Tests for NovelStore. Usage: novelstore_test <scratch-dir>
// The scratch directory is wiped and recreated on every run.

#include "NovelStore.h"

#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
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

// @relation(DAT-1, DAT-2, DAT-3, scope=function)
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

// @relation(DAT-8, scope=function)
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

// @relation(FR-SYN-1, DAT-5, DAT-6, scope=function)
void testSynopsis(const fs::path &dir)
{
    // Stored byte-for-byte: CRLF, tabs, trailing/leading whitespace, non-breaking spaces, emoji, NUL.
    const std::string synopsis =
        std::string("  A tale\r\n\tof two\xC2\xA0" "cities \xF0\x9F\x8F\x99\n\n  ") + '\0' + "end ";
    {
        NovelStore store(dir, NovelStore::Mode::Create);
        EXPECT(store.synopsis().empty()); // optional entry
        store.setSynopsis(synopsis);
        EXPECT(store.synopsis() == synopsis);
    }
    NovelStore store(dir, NovelStore::Mode::Open);
    EXPECT(store.synopsis() == synopsis);
    EXPECT(store.synopsis().size() == synopsis.size());
}

std::string planOutline(const NovelStore &store)
{
    std::string out;
    for (const auto &plan : store.plans()) {
        out += (out.empty() ? "" : " ") + plan.title + "[" + (plan.model.empty() ? "hand" : plan.model) + "]";
    }
    return out;
}

// @relation(FR-PLN-1, FR-PLN-4, DAT-2, scope=function)
void testPlans(const fs::path &dir)
{
    Id byHand = 0, generated = 0, second = 0;
    {
        NovelStore store(dir, NovelStore::Mode::Create);
        EXPECT(store.plans().empty());
        const Id act = store.addAct(0, "Act");
        byHand = store.addPlan("My plan", "# Mine\n");
        generated = store.addPlan("Plan 2", "# Generated\n\n## Act 1\n", "qwen/qwen3.5-9b");
        second = store.addPlan("Plan 3", "", "other-model");
        EXPECT(planOutline(store) == "My plan[hand] Plan 2[qwen/qwen3.5-9b] Plan 3[other-model]");
        EXPECT(byHand > act && generated > byHand && second > generated); // ids shared with the structure
        EXPECT(!store.kindOf(byHand));                                    // plans aren't structure nodes

        store.setPlanText(generated, "# Generated, then edited by hand\n");
        store.renamePlan(generated, "Three acts");
        EXPECT(store.planText(byHand) == "# Mine\n");
        EXPECT(store.planText(second).empty());
        EXPECT(throws<std::invalid_argument>([&] { store.planText(act); }));
        EXPECT(throws<std::invalid_argument>([&] { store.renamePlan(12345, "x"); }));
    }
    {
        NovelStore store(dir, NovelStore::Mode::Open);
        // The generating model is remembered across edits and reopening.
        EXPECT(planOutline(store) == "My plan[hand] Three acts[qwen/qwen3.5-9b] Plan 3[other-model]");
        EXPECT(store.planText(generated) == "# Generated, then edited by hand\n");
        store.removePlan(byHand);
        EXPECT(planOutline(store) == "Three acts[qwen/qwen3.5-9b] Plan 3[other-model]");
        EXPECT(throws<std::invalid_argument>([&] { store.planText(byHand); }));
        EXPECT(store.addPlan("New", "") > second); // ids never reused
    }
    NovelStore store(dir, NovelStore::Mode::Open);
    EXPECT(planOutline(store) == "Three acts[qwen/qwen3.5-9b] Plan 3[other-model] New[hand]");
}

// @relation(FR-IMP-3, FR-IMP-5, DAT-3, scope=function)
void testBriefsAndImport(const fs::path &dir)
{
    using NA = NovelStore::NewAct;
    using NC = NovelStore::NewChapter;
    using NS = NovelStore::NewScene;
    const std::vector<NA> planned{
        NA{"Act 1: Setup", "Mira's world.",
           {NC{"Chapter 1: The Letter", "Discovery.", {NS{"The Letter", "POV: Mira. She finds it."}, NS{"The Council", ""}}},
            NC{"Chapter 2: Silence", "", {}}}},
        NA{"Act 2", "", {NC{"Chapter 3: Storm", "", {NS{"Waves", "She nearly drowns."}}}}}};
    Id existingScene = 0;
    {
        NovelStore store(dir, NovelStore::Mode::Create);
        const Id act = store.addAct(0, "Old act");
        const Id chapter = store.addChapter(act, 0, "Old chapter");
        existingScene = store.addScene(chapter, 0, "Old scene", "Old text.");
        store.setBrief(chapter, "An old brief");
        EXPECT(store.acts()[0].chapters[0].brief == "An old brief");
        store.setBrief(chapter, "");
        EXPECT(store.acts()[0].chapters[0].brief.empty());

        // Append after the existing structure.
        const auto first = store.importStructure(planned, false);
        EXPECT(outline(store) == "Old act[Old chapter(Old scene)] Act 1: Setup[Chapter 1: The Letter(The Letter,The Council),"
                                 "Chapter 2: Silence()] Act 2[Chapter 3: Storm(Waves)]");
        EXPECT(first && store.kindOf(*first) == NovelStore::Kind::Scene && *first > existingScene);
        EXPECT(store.acts()[1].brief == "Mira's world.");
        EXPECT(store.acts()[1].chapters[0].scenes[0].brief == "POV: Mira. She finds it.");
        EXPECT(store.sceneText(*first).empty());
        EXPECT(store.sceneText(existingScene) == "Old text.");
    }
    {
        NovelStore store(dir, NovelStore::Mode::Open); // everything persisted, briefs included
        EXPECT(store.acts().size() == 3);
        EXPECT(store.acts()[1].chapters[0].brief == "Discovery.");
        EXPECT(store.acts()[2].chapters[0].scenes[0].brief == "She nearly drowns.");
        EXPECT(store.acts()[1].chapters[0].scenes[1].brief.empty());

        // Replace everything.
        const auto first = store.importStructure({NA{"New", "", {NC{"Ch", "", {NS{"S", "brief"}}}}}}, true);
        EXPECT(outline(store) == "New[Ch(S)]");
        EXPECT(throws<std::invalid_argument>([&] { store.sceneText(existingScene); }));
        EXPECT(first && store.acts()[0].chapters[0].scenes[0].id == *first);
        EXPECT(store.importStructure({}, false) == std::nullopt); // nothing to add
        store.remove(store.acts()[0].id);
    }
    NovelStore store(dir, NovelStore::Mode::Open); // replaced and removed nodes left nothing behind
    EXPECT(store.acts().empty());
}

// @relation(FR-TPL-1, FR-PLN-4, DAT-6, scope=function)
void testPlanTemplates(const fs::path &dir)
{
    Id staged = 0, hand = 0;
    {
        NovelStore store(dir, NovelStore::Mode::Create);
        EXPECT(store.planTemplate().empty()); // default template
        store.setPlanTemplate("inner-stages");
        staged = store.addPlan("Plan 1", "# Plan", "model-a", "inner-stages");
        hand = store.addPlan("Plan 2", "# Mine");
        EXPECT(store.plans()[0].templateId == "inner-stages" && store.plans()[1].templateId.empty());
    }
    {
        NovelStore store(dir, NovelStore::Mode::Open);
        EXPECT(store.planTemplate() == "inner-stages");
        EXPECT(store.plans()[0].templateId == "inner-stages" && store.plans()[0].model == "model-a");
        EXPECT(store.plans()[1].templateId.empty());
        store.setPlanTemplate(""); // back to the default
        store.removePlan(staged);
    }
    NovelStore store(dir, NovelStore::Mode::Open);
    EXPECT(store.planTemplate().empty());
    EXPECT(store.plans().size() == 1 && store.plans()[0].id == hand);
}

// @relation(FR-CHT-5, DAT-6, scope=function)
void testPlanChat(const fs::path &dir)
{
    Id plan = 0, other = 0;
    {
        NovelStore store(dir, NovelStore::Mode::Create);
        plan = store.addPlan("Plan 1", "# Plan", "m");
        other = store.addPlan("Plan 2", "# Other");
        EXPECT(store.planChat(plan).empty());
        store.setPlanChat(plan, R"([{"role":"user","content":"Shorter act 1"}])");
        EXPECT(throws<std::invalid_argument>([&] { store.planChat(999); }));
    }
    {
        NovelStore store(dir, NovelStore::Mode::Open);
        EXPECT(store.planChat(plan) == R"([{"role":"user","content":"Shorter act 1"}])");
        EXPECT(store.planChat(other).empty()); // each plan has its own conversation
        store.setPlanChat(plan, "");           // cleared
        EXPECT(store.planChat(plan).empty());
        store.setPlanChat(other, "[]");
        store.removePlan(other);
        const Id again = store.addPlan("Plan 3", "");
        EXPECT(store.planChat(again).empty());
    }
}

// @relation(FR-SYN-2, DAT-6, scope=function)
void testPlanModelOverride(const fs::path &dir)
{
    {
        NovelStore store(dir, NovelStore::Mode::Create);
        EXPECT(store.planModelOverride().empty()); // default: use the app setting
        store.setPlanModelOverride("big-planner-70b");
    }
    {
        NovelStore store(dir, NovelStore::Mode::Open);
        EXPECT(store.planModelOverride() == "big-planner-70b");
        store.setPlanModelOverride(""); // back to the setting
    }
    NovelStore store(dir, NovelStore::Mode::Open);
    EXPECT(store.planModelOverride().empty());
}

// Writes a novel in format 2 (single "novel/plan" entry) directly with RocksDB.
void writeFormat2Novel(const fs::path &dir, const std::string &plan)
{
    rocksdb::Options options;
    options.create_if_missing = true;
    options.compression = rocksdb::kLZ4Compression;
    std::unique_ptr<rocksdb::DB> db;
    const auto u8 = dir.u8string();
    rocksdb::Status s = rocksdb::DB::Open(options, std::string(u8.begin(), u8.end()), &db);
    EXPECT(s.ok());
    rocksdb::WriteBatch batch;
    batch.Put("novel/format", "2");
    batch.Put("novel/next_id", "4");
    batch.Put("novel/acts", "1");
    batch.Put("act/00000000000000000001/title", "Act 1");
    batch.Put("act/00000000000000000001/chapters", "2");
    batch.Put("chapter/00000000000000000002/title", "Chapter 1");
    batch.Put("chapter/00000000000000000002/scenes", "3");
    batch.Put("scene/00000000000000000003/title", "Scene 1");
    batch.Put("scene/00000000000000000003/text", "Once.");
    batch.Put("novel/synopsis", "A synopsis.");
    if (!plan.empty()) {
        batch.Put("novel/plan", plan);
    }
    EXPECT(db->Write({}, &batch).ok());
    db->Close();
}

// @relation(DAT-7, scope=function)
void testUpgradeFromFormat2(const fs::path &root)
{
    {
        const fs::path dir = root / "format2.novel";
        writeFormat2Novel(dir, "# Old plan\n");
        {
            NovelStore store(dir, NovelStore::Mode::Open);
            EXPECT(planOutline(store) == "Plan 1[hand]"); // how it was made wasn't recorded
            EXPECT(store.planText(store.plans().at(0).id) == "# Old plan\n");
            EXPECT(store.plans().at(0).id == 4); // next free id
            EXPECT(store.synopsis() == "A synopsis.");
            EXPECT(outline(store) == "Act 1[Chapter 1(Scene 1)]");
            EXPECT(store.addAct(1, "Act 2") == 5);
        }
        NovelStore store(dir, NovelStore::Mode::Open); // now format 3: opens without upgrading again
        EXPECT(planOutline(store) == "Plan 1[hand]");
        EXPECT(outline(store) == "Act 1[Chapter 1(Scene 1)] Act 2[]");
    }
    {
        const fs::path dir = root / "format2-noplan.novel";
        writeFormat2Novel(dir, "");
        NovelStore store(dir, NovelStore::Mode::Open);
        EXPECT(store.plans().empty());
        EXPECT(store.addPlan("P", "") == 4);
    }
}

// @relation(FR-NOV-1, FR-NOV-2, scope=function)
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
        testSynopsis(root / "synopsis.novel");
        testPlans(root / "plans.novel");
        testPlanModelOverride(root / "override.novel");
        testPlanChat(root / "chat.novel");
        testPlanTemplates(root / "templates.novel");
        testBriefsAndImport(root / "import.novel");
        testUpgradeFromFormat2(root);
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
