#include "NovelStore.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace {

using Id = NovelStore::Id;
using Kind = NovelStore::Kind;

constexpr std::string_view kFormatKey = "novel/format";
constexpr std::string_view kFormatVersion = "1";
constexpr std::string_view kNextIdKey = "novel/next_id";
constexpr std::string_view kActsKey = "novel/acts";
constexpr std::string_view kSynopsisKey = "novel/synopsis";
constexpr std::string_view kDetailsPrefix = "novel/details/";
constexpr std::string_view kPlansKey = "novel/plans";
constexpr std::string_view kPlanModelKey = "novel/plan_model";
constexpr std::string_view kPlanTemplateKey = "novel/plan_template";

rocksdb::Slice slice(std::string_view s)
{
  return {s.data(), s.size()};
}

void check(const rocksdb::Status &status, std::string_view what)
{
  if (!status.ok()) {
    throw StorageError(std::string(what) + ": " + status.ToString());
  }
}

// e.g. key("scene", 7, "text") -> "scene/00000000000000000007/text"
std::string key(const char *kind, Id id, const char *field)
{
  char buf[64];
  std::snprintf(buf, sizeof buf, "%s/%020llu/%s", kind, static_cast<unsigned long long>(id), field);
  return buf;
}

Id parseId(std::string_view text)
{
  Id value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || end != text.data() + text.size()) {
    throw StorageError("Corrupt novel: invalid number '" + std::string(text) + "'");
  }
  return value;
}

template <typename Node> std::string encodeIds(const std::vector<Node> &nodes)
{
  std::string out;
  for (const Node &node : nodes) {
    if (!out.empty()) {
      out += ',';
    }
    out += std::to_string(node.id);
  }
  return out;
}

std::vector<Id> decodeIds(std::string_view text)
{
  std::vector<Id> ids;
  while (!text.empty()) {
    const auto comma = text.find(',');
    ids.push_back(parseId(text.substr(0, comma)));
    text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
  }
  return ids;
}

template <typename T> auto clampedPos(const std::vector<T> &v, std::size_t position)
{
  return v.begin() + static_cast<std::ptrdiff_t>(std::min(position, v.size()));
}

template <typename T> auto at(std::vector<T> &v, std::size_t index)
{
  return v.begin() + static_cast<std::ptrdiff_t>(index);
}

// RocksDB takes paths as std::string. Pass UTF-8 so non-ASCII folder names work (on Windows this relies
// on RocksDB being built with WITH_WINDOWS_UTF8_FILENAMES, see profiles/windows-msvc).
std::string toUtf8(const std::filesystem::path &path)
{
  const std::u8string u8 = path.u8string();
  return {u8.begin(), u8.end()};
}

std::string getOptional(rocksdb::DB &db, std::string_view k, std::string_view what)
{
  std::string value;
  const auto status = db.Get({}, slice(k), &value);
  if (status.IsNotFound()) {
    return {};
  }
  check(status, what);
  return value;
}

rocksdb::WriteOptions durableWrites()
{
  rocksdb::WriteOptions options;
  options.sync = true; // a novel is precious; fsync every write (writes are small and infrequent)
  return options;
}

} // namespace

NovelStore::NovelStore(const std::filesystem::path &dir, Mode mode) : m_path(dir)
{
  // RocksDB creates the directory and LOCK/LOG files even when opening fails, so check for a database
  // first rather than littering an arbitrary folder the user picked.
  std::error_code ec;
  if (mode == Mode::Open && !std::filesystem::exists(dir / "CURRENT", ec)) {
    throw StorageError("Not a NovelClanker novel: " + toUtf8(dir));
  }

  rocksdb::Options options;
  options.OptimizeForSmallDb();
  options.compression = rocksdb::kLZ4Compression;
  options.create_if_missing = mode == Mode::Create;
  options.error_if_exists = mode == Mode::Create;
  check(rocksdb::DB::Open(options, toUtf8(dir), &m_db), "Cannot open novel");

  if (mode == Mode::Create) {
    rocksdb::WriteBatch batch;
    batch.Put(slice(kFormatKey), slice(kFormatVersion));
    batch.Put(slice(kNextIdKey), slice(std::to_string(m_nextId)));
    batch.Put(slice(kActsKey), slice(""));
    batch.Put(slice(kPlansKey), slice(""));
    check(m_db->Write(durableWrites(), &batch), "Cannot initialize novel");
    return;
  }

  const auto get = [this](std::string_view k) {
    std::string value;
    const auto status = m_db->Get({}, slice(k), &value);
    if (status.IsNotFound()) {
      throw StorageError("Corrupt novel: missing entry '" + std::string(k) + "'");
    }
    check(status, "Cannot read novel");
    return value;
  };

  std::string format;
  const auto status = m_db->Get({}, slice(kFormatKey), &format);
  if (status.IsNotFound()) {
    throw StorageError("Not a NovelClanker novel: " + toUtf8(dir));
  }
  check(status, "Cannot read novel");
  // Only the current format is read; anything else is refused before reading (or changing) it.
  if (format != kFormatVersion) {
    throw StorageError("Unsupported novel format version '" + format + "' (this version reads format " +
                       std::string(kFormatVersion) + ")");
  }
  m_nextId = parseId(get(kNextIdKey));

  for (Id actId : decodeIds(get(kActsKey))) {
    const auto brief = [this](const char *kind, Id id) {
      return getOptional(*m_db, key(kind, id, "brief"), "Cannot read novel");
    };
    Act act{actId, get(key("act", actId, "title")), {}, brief("act", actId)};
    for (Id chapterId : decodeIds(get(key("act", actId, "chapters")))) {
      Chapter chapter{chapterId, get(key("chapter", chapterId, "title")), {}, brief("chapter", chapterId)};
      for (Id sceneId : decodeIds(get(key("chapter", chapterId, "scenes")))) {
        chapter.scenes.push_back({sceneId, get(key("scene", sceneId, "title")), brief("scene", sceneId)});
      }
      act.chapters.push_back(std::move(chapter));
    }
    m_acts.push_back(std::move(act));
  }
  for (Id planId : decodeIds(get(kPlansKey))) {
    m_plans.push_back({planId, get(key("plan", planId, "title")),
                       getOptional(*m_db, key("plan", planId, "model"), "Cannot read plan"),
                       getOptional(*m_db, key("plan", planId, "template"), "Cannot read plan")});
  }
}

NovelStore::~NovelStore()
{
  if (m_db) {
    m_db->Close();
  }
}

std::optional<NovelStore::Location> NovelStore::locate(Id id) const
{
  for (std::size_t a = 0; a < m_acts.size(); ++a) {
    const Act &act = m_acts[a];
    if (act.id == id) {
      return Location{Kind::Act, a};
    }
    for (std::size_t c = 0; c < act.chapters.size(); ++c) {
      const Chapter &chapter = act.chapters[c];
      if (chapter.id == id) {
        return Location{Kind::Chapter, a, c};
      }
      for (std::size_t s = 0; s < chapter.scenes.size(); ++s) {
        if (chapter.scenes[s].id == id) {
          return Location{Kind::Scene, a, c, s};
        }
      }
    }
  }
  return std::nullopt;
}

NovelStore::Location NovelStore::require(Id id, std::optional<Kind> kind) const
{
  const auto location = locate(id);
  if (!location || (kind && location->kind != *kind)) {
    throw std::invalid_argument("NovelStore: unknown id or wrong node kind: " + std::to_string(id));
  }
  return *location;
}

std::optional<NovelStore::Kind> NovelStore::kindOf(Id id) const
{
  const auto location = locate(id);
  return location ? std::optional(location->kind) : std::nullopt;
}

std::optional<NovelStore::Id> NovelStore::parentOf(Id id) const
{
  const auto location = locate(id);
  if (!location || location->kind == Kind::Act) {
    return std::nullopt;
  }
  const Act &act = m_acts[location->act];
  return location->kind == Kind::Chapter ? act.id : act.chapters[location->chapter].id;
}

NovelStore::Id NovelStore::addAct(std::size_t position, std::string_view title)
{
  const Id id = m_nextId;
  std::vector<Act> acts = m_acts;
  acts.insert(clampedPos(acts, position), Act{id, std::string(title), {}, {}});

  rocksdb::WriteBatch batch;
  batch.Put(key("act", id, "title"), slice(title));
  batch.Put(key("act", id, "chapters"), "");
  batch.Put(slice(kActsKey), encodeIds(acts));
  batch.Put(slice(kNextIdKey), std::to_string(id + 1));
  check(m_db->Write(durableWrites(), &batch), "Cannot add act");

  m_acts = std::move(acts);
  m_nextId = id + 1;
  return id;
}

NovelStore::Id NovelStore::addChapter(Id actId, std::size_t position, std::string_view title)
{
  const Location where = require(actId, Kind::Act);
  const Id id = m_nextId;
  std::vector<Act> acts = m_acts;
  auto &chapters = acts[where.act].chapters;
  chapters.insert(clampedPos(chapters, position), Chapter{id, std::string(title), {}, {}});

  rocksdb::WriteBatch batch;
  batch.Put(key("chapter", id, "title"), slice(title));
  batch.Put(key("chapter", id, "scenes"), "");
  batch.Put(key("act", actId, "chapters"), encodeIds(chapters));
  batch.Put(slice(kNextIdKey), std::to_string(id + 1));
  check(m_db->Write(durableWrites(), &batch), "Cannot add chapter");

  m_acts = std::move(acts);
  m_nextId = id + 1;
  return id;
}

NovelStore::Id NovelStore::addScene(Id chapterId, std::size_t position, std::string_view title,
                                    std::string_view text)
{
  const Location where = require(chapterId, Kind::Chapter);
  const Id id = m_nextId;
  std::vector<Act> acts = m_acts;
  auto &scenes = acts[where.act].chapters[where.chapter].scenes;
  scenes.insert(clampedPos(scenes, position), Scene{id, std::string(title), {}});

  rocksdb::WriteBatch batch;
  batch.Put(key("scene", id, "title"), slice(title));
  batch.Put(key("scene", id, "text"), slice(text));
  batch.Put(key("chapter", chapterId, "scenes"), encodeIds(scenes));
  batch.Put(slice(kNextIdKey), std::to_string(id + 1));
  check(m_db->Write(durableWrites(), &batch), "Cannot add scene");

  m_acts = std::move(acts);
  m_nextId = id + 1;
  return id;
}

void NovelStore::rename(Id id, std::string_view title)
{
  const Location where = require(id);
  static constexpr const char *kKindNames[] = {"act", "chapter", "scene"};
  check(m_db->Put(durableWrites(), key(kKindNames[static_cast<int>(where.kind)], id, "title"), slice(title)),
        "Cannot rename");

  Act &act = m_acts[where.act];
  switch (where.kind) {
  case Kind::Act:
    act.title = title;
    break;
  case Kind::Chapter:
    act.chapters[where.chapter].title = title;
    break;
  case Kind::Scene:
    act.chapters[where.chapter].scenes[where.scene].title = title;
    break;
  }
}

void NovelStore::setBrief(Id id, std::string_view brief)
{
  const Location where = require(id);
  static constexpr const char *kKindNames[] = {"act", "chapter", "scene"};
  const std::string k = key(kKindNames[static_cast<int>(where.kind)], id, "brief");
  check(brief.empty() ? m_db->Delete(durableWrites(), k) : m_db->Put(durableWrites(), k, slice(brief)),
        "Cannot save brief");
  Act &act = m_acts[where.act];
  switch (where.kind) {
  case Kind::Act:
    act.brief = brief;
    break;
  case Kind::Chapter:
    act.chapters[where.chapter].brief = brief;
    break;
  case Kind::Scene:
    act.chapters[where.chapter].scenes[where.scene].brief = brief;
    break;
  }
}

std::optional<NovelStore::Id> NovelStore::importStructure(const std::vector<NewAct> &newActs, bool replace)
{
  rocksdb::WriteBatch batch;
  std::vector<Act> acts = replace ? std::vector<Act>{} : m_acts;
  if (replace) {
    for (const Act &act : m_acts) {
      for (const Chapter &chapter : act.chapters) {
        for (const Scene &scene : chapter.scenes) {
          for (const char *field : {"title", "text", "brief"}) {
            batch.Delete(key("scene", scene.id, field));
          }
        }
        for (const char *field : {"title", "scenes", "brief"}) {
          batch.Delete(key("chapter", chapter.id, field));
        }
      }
      for (const char *field : {"title", "chapters", "brief"}) {
        batch.Delete(key("act", act.id, field));
      }
    }
  }

  Id next = m_nextId;
  std::optional<Id> firstScene;
  const auto putBrief = [&](const char *kind, Id id, const std::string &brief) {
    if (!brief.empty()) {
      batch.Put(key(kind, id, "brief"), brief);
    }
  };
  for (const NewAct &newAct : newActs) {
    Act act{next++, newAct.title, {}, newAct.brief};
    batch.Put(key("act", act.id, "title"), act.title);
    putBrief("act", act.id, act.brief);
    for (const NewChapter &newChapter : newAct.chapters) {
      Chapter chapter{next++, newChapter.title, {}, newChapter.brief};
      batch.Put(key("chapter", chapter.id, "title"), chapter.title);
      putBrief("chapter", chapter.id, chapter.brief);
      for (const NewScene &newScene : newChapter.scenes) {
        Scene scene{next++, newScene.title, newScene.brief};
        batch.Put(key("scene", scene.id, "title"), scene.title);
        batch.Put(key("scene", scene.id, "text"), newScene.text);
        putBrief("scene", scene.id, scene.brief);
        if (!firstScene) {
          firstScene = scene.id;
        }
        chapter.scenes.push_back(std::move(scene));
      }
      batch.Put(key("chapter", chapter.id, "scenes"), encodeIds(chapter.scenes));
      act.chapters.push_back(std::move(chapter));
    }
    batch.Put(key("act", act.id, "chapters"), encodeIds(act.chapters));
    acts.push_back(std::move(act));
  }
  batch.Put(slice(kActsKey), encodeIds(acts));
  batch.Put(slice(kNextIdKey), std::to_string(next));
  check(m_db->Write(durableWrites(), &batch), "Cannot create the structure");

  m_acts = std::move(acts);
  m_nextId = next;
  return firstScene;
}

std::string NovelStore::sceneText(Id scene) const
{
  require(scene, Kind::Scene);
  std::string text;
  check(m_db->Get({}, key("scene", scene, "text"), &text), "Cannot read scene");
  return text;
}

void NovelStore::setSceneText(Id scene, std::string_view markdown)
{
  require(scene, Kind::Scene);
  check(m_db->Put(durableWrites(), key("scene", scene, "text"), slice(markdown)), "Cannot save scene");
}

std::string NovelStore::synopsis() const
{
  return getOptional(*m_db, kSynopsisKey, "Cannot read synopsis");
}

void NovelStore::setSynopsis(std::string_view text)
{
  check(m_db->Put(durableWrites(), slice(kSynopsisKey), slice(text)), "Cannot save synopsis");
}

namespace {
std::string detailKey(std::string_view name)
{
  const bool valid = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
  if (!valid) {
    throw std::invalid_argument("Invalid detail name: " + std::string(name));
  }
  return std::string(kDetailsPrefix) + std::string(name);
}
} // namespace

std::string NovelStore::detail(std::string_view name) const
{
  return getOptional(*m_db, detailKey(name), "Cannot read the novel's details");
}

std::map<std::string, std::string> NovelStore::details() const
{
  std::map<std::string, std::string> result;
  std::unique_ptr<rocksdb::Iterator> it(m_db->NewIterator({}));
  for (it->Seek(slice(kDetailsPrefix)); it->Valid() && it->key().starts_with(slice(kDetailsPrefix));
       it->Next()) {
    result.emplace(it->key().ToString().substr(kDetailsPrefix.size()), it->value().ToString());
  }
  check(it->status(), "Cannot read the novel's details");
  return result;
}

void NovelStore::setDetails(const std::map<std::string, std::string> &values)
{
  rocksdb::WriteBatch batch;
  for (const auto &[name, value] : values) {
    if (value.empty()) {
      batch.Delete(detailKey(name));
    } else {
      batch.Put(detailKey(name), value);
    }
  }
  check(m_db->Write(durableWrites(), &batch), "Cannot save the novel's details");
}

std::string NovelStore::planModelOverride() const
{
  return getOptional(*m_db, kPlanModelKey, "Cannot read the plan model");
}

void NovelStore::setPlanModelOverride(std::string_view model)
{
  const auto status = model.empty() ? m_db->Delete(durableWrites(), slice(kPlanModelKey))
                                    : m_db->Put(durableWrites(), slice(kPlanModelKey), slice(model));
  check(status, "Cannot save the plan model");
}

std::string NovelStore::planTemplate() const
{
  return getOptional(*m_db, kPlanTemplateKey, "Cannot read the plan template");
}

void NovelStore::setPlanTemplate(std::string_view templateId)
{
  const auto status = templateId.empty()
                          ? m_db->Delete(durableWrites(), slice(kPlanTemplateKey))
                          : m_db->Put(durableWrites(), slice(kPlanTemplateKey), slice(templateId));
  check(status, "Cannot save the plan template");
}

std::size_t NovelStore::requirePlan(Id plan) const
{
  for (std::size_t i = 0; i < m_plans.size(); ++i) {
    if (m_plans[i].id == plan) {
      return i;
    }
  }
  throw std::invalid_argument("NovelStore: unknown plan id: " + std::to_string(plan));
}

NovelStore::Id NovelStore::addPlan(std::string_view title, std::string_view markdown, std::string_view model,
                                   std::string_view templateId)
{
  const Id id = m_nextId;
  std::vector<Plan> plans = m_plans;
  plans.push_back({id, std::string(title), std::string(model), std::string(templateId)});

  rocksdb::WriteBatch batch;
  batch.Put(key("plan", id, "title"), slice(title));
  batch.Put(key("plan", id, "text"), slice(markdown));
  if (!model.empty()) {
    batch.Put(key("plan", id, "model"), slice(model));
  }
  if (!templateId.empty()) {
    batch.Put(key("plan", id, "template"), slice(templateId));
  }
  batch.Put(slice(kPlansKey), encodeIds(plans));
  batch.Put(slice(kNextIdKey), std::to_string(id + 1));
  check(m_db->Write(durableWrites(), &batch), "Cannot add plan");

  m_plans = std::move(plans);
  m_nextId = id + 1;
  return id;
}

void NovelStore::renamePlan(Id plan, std::string_view title)
{
  const std::size_t index = requirePlan(plan);
  check(m_db->Put(durableWrites(), key("plan", plan, "title"), slice(title)), "Cannot rename plan");
  m_plans[index].title = title;
}

std::string NovelStore::planText(Id plan) const
{
  requirePlan(plan);
  std::string text;
  check(m_db->Get({}, key("plan", plan, "text"), &text), "Cannot read plan");
  return text;
}

void NovelStore::setPlanText(Id plan, std::string_view markdown)
{
  requirePlan(plan);
  check(m_db->Put(durableWrites(), key("plan", plan, "text"), slice(markdown)), "Cannot save plan");
}

std::string NovelStore::planChat(Id plan) const
{
  requirePlan(plan);
  return getOptional(*m_db, key("plan", plan, "chat"), "Cannot read the plan chat");
}

void NovelStore::setPlanChat(Id plan, std::string_view chat)
{
  requirePlan(plan);
  const std::string k = key("plan", plan, "chat");
  const auto status =
      chat.empty() ? m_db->Delete(durableWrites(), k) : m_db->Put(durableWrites(), k, slice(chat));
  check(status, "Cannot save the plan chat");
}

void NovelStore::removePlan(Id plan)
{
  const std::size_t index = requirePlan(plan);
  std::vector<Plan> plans = m_plans;
  plans.erase(plans.begin() + static_cast<std::ptrdiff_t>(index));

  rocksdb::WriteBatch batch;
  batch.Delete(key("plan", plan, "title"));
  batch.Delete(key("plan", plan, "text"));
  batch.Delete(key("plan", plan, "model"));
  batch.Delete(key("plan", plan, "chat"));
  batch.Delete(key("plan", plan, "template"));
  batch.Put(slice(kPlansKey), encodeIds(plans));
  check(m_db->Write(durableWrites(), &batch), "Cannot delete plan");
  m_plans = std::move(plans);
}

void NovelStore::remove(Id id)
{
  const Location where = require(id);
  std::vector<Act> acts = m_acts;
  rocksdb::WriteBatch batch;

  const auto deleteScene = [&](const Scene &scene) {
    batch.Delete(key("scene", scene.id, "title"));
    batch.Delete(key("scene", scene.id, "text"));
    batch.Delete(key("scene", scene.id, "brief"));
  };
  const auto deleteChapter = [&](const Chapter &chapter) {
    for (const Scene &scene : chapter.scenes) {
      deleteScene(scene);
    }
    batch.Delete(key("chapter", chapter.id, "title"));
    batch.Delete(key("chapter", chapter.id, "scenes"));
    batch.Delete(key("chapter", chapter.id, "brief"));
  };

  Act &act = acts[where.act];
  switch (where.kind) {
  case Kind::Act:
    for (const Chapter &chapter : act.chapters) {
      deleteChapter(chapter);
    }
    batch.Delete(key("act", id, "title"));
    batch.Delete(key("act", id, "chapters"));
    batch.Delete(key("act", id, "brief"));
    acts.erase(at(acts, where.act));
    batch.Put(slice(kActsKey), encodeIds(acts));
    break;
  case Kind::Chapter:
    deleteChapter(act.chapters[where.chapter]);
    act.chapters.erase(at(act.chapters, where.chapter));
    batch.Put(key("act", act.id, "chapters"), encodeIds(act.chapters));
    break;
  case Kind::Scene: {
    Chapter &chapter = act.chapters[where.chapter];
    deleteScene(chapter.scenes[where.scene]);
    chapter.scenes.erase(at(chapter.scenes, where.scene));
    batch.Put(key("chapter", chapter.id, "scenes"), encodeIds(chapter.scenes));
    break;
  }
  }

  check(m_db->Write(durableWrites(), &batch), "Cannot delete");
  m_acts = std::move(acts);
}

void NovelStore::move(Id id, Id newParent, std::size_t position)
{
  const Location from = require(id);
  std::vector<Act> acts = m_acts;
  rocksdb::WriteBatch batch;

  // Removing a node never shifts the indices of nodes at a higher level, so `to` (located before the
  // removal) stays valid.
  switch (from.kind) {
  case Kind::Act: {
    if (newParent != 0) {
      throw std::invalid_argument("NovelStore: acts have no parent");
    }
    Act node = std::move(acts[from.act]);
    acts.erase(at(acts, from.act));
    acts.insert(clampedPos(acts, position), std::move(node));
    batch.Put(slice(kActsKey), encodeIds(acts));
    break;
  }
  case Kind::Chapter: {
    const Location to = require(newParent, Kind::Act);
    auto &source = acts[from.act].chapters;
    Chapter node = std::move(source[from.chapter]);
    source.erase(at(source, from.chapter));
    auto &target = acts[to.act].chapters;
    target.insert(clampedPos(target, position), std::move(node));
    batch.Put(key("act", acts[from.act].id, "chapters"), encodeIds(source));
    batch.Put(key("act", newParent, "chapters"), encodeIds(target));
    break;
  }
  case Kind::Scene: {
    const Location to = require(newParent, Kind::Chapter);
    auto &sourceChapter = acts[from.act].chapters[from.chapter];
    Scene node = std::move(sourceChapter.scenes[from.scene]);
    sourceChapter.scenes.erase(at(sourceChapter.scenes, from.scene));
    auto &target = acts[to.act].chapters[to.chapter].scenes;
    target.insert(clampedPos(target, position), std::move(node));
    batch.Put(key("chapter", sourceChapter.id, "scenes"), encodeIds(sourceChapter.scenes));
    batch.Put(key("chapter", newParent, "scenes"), encodeIds(target));
    break;
  }
  }

  check(m_db->Write(durableWrites(), &batch), "Cannot move");
  m_acts = std::move(acts);
}
