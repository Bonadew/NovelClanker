#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rocksdb {
class DB;
}

class StorageError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

// A novel stored in a RocksDB database (a directory on disk), structured as acts -> chapters -> scenes.
//
// Every scene's text is its own database entry; acts and chapters only carry a title and the ordered ids
// of their children. Structural changes therefore never rewrite scene text. Key layout (<id> is a
// zero-padded 20-digit number; ids are unique across all node kinds and never reused):
//
//   novel/format          format version ("2")
//   novel/next_id         next unused id (decimal)
//   novel/acts            act ids in order (comma-separated decimals)
//   novel/synopsis        the synopsis, exactly as written (optional; missing = empty)
//   novel/plan            the novel plan, Markdown (optional; missing = empty)
//   act/<id>/title        act title
//   act/<id>/chapters     chapter ids in order
//   chapter/<id>/title    chapter title
//   chapter/<id>/scenes   scene ids in order
//   scene/<id>/title      scene title
//   scene/<id>/text       scene Markdown
//
// The structure (titles and ordering) is loaded into memory on open; scene text is read on demand.
// RocksDB locks the database while it is open, so a novel can only be opened by one NovelStore (or
// process) at a time. Every change is written atomically and synced to disk. Methods throw StorageError
// on failure, and std::invalid_argument for unknown ids or impossible structure changes.
class NovelStore
{
  public:
    using Id = std::uint64_t;

    enum class Kind { Act, Chapter, Scene };

    struct Scene {
        Id id;
        std::string title;
    };

    struct Chapter {
        Id id;
        std::string title;
        std::vector<Scene> scenes;
    };

    struct Act {
        Id id;
        std::string title;
        std::vector<Chapter> chapters;
    };

    enum class Mode {
        Create, // create a new, empty novel; fails if a database already exists at the path
        Open,   // open an existing novel
    };

    NovelStore(const std::filesystem::path &dir, Mode mode);
    ~NovelStore();

    NovelStore(const NovelStore &) = delete;
    NovelStore &operator=(const NovelStore &) = delete;

    const std::filesystem::path &path() const
    {
        return m_path;
    }

    // The whole structure, in reading order.
    const std::vector<Act> &acts() const
    {
        return m_acts;
    }

    std::optional<Kind> kindOf(Id id) const;
    // Parent of a chapter (its act) or scene (its chapter); nullopt for acts and unknown ids.
    std::optional<Id> parentOf(Id id) const;

    // Insert a node so it ends up at `position` among its siblings (clamped to the end).
    Id addAct(std::size_t position, std::string_view title);
    Id addChapter(Id act, std::size_t position, std::string_view title);
    Id addScene(Id chapter, std::size_t position, std::string_view title, std::string_view text = {});

    void rename(Id id, std::string_view title);

    std::string sceneText(Id scene) const;
    void setSceneText(Id scene, std::string_view markdown);

    // Synopsis and plan are stored byte-for-byte as given. Both are empty until set.
    std::string synopsis() const;
    void setSynopsis(std::string_view text);
    std::string plan() const;
    void setPlan(std::string_view markdown);

    // Removes a node and everything below it.
    void remove(Id id);

    // Moves a node to `position` among the children of `newParent` (position in the resulting order,
    // clamped to the end). Acts take no parent (pass 0); chapters move between acts, scenes between
    // chapters.
    void move(Id id, Id newParent, std::size_t position);

  private:
    struct Location {
        Kind kind;
        std::size_t act = 0, chapter = 0, scene = 0;
    };

    std::optional<Location> locate(Id id) const;
    Location require(Id id, std::optional<Kind> kind = std::nullopt) const;

    std::filesystem::path m_path;
    std::unique_ptr<rocksdb::DB> m_db;
    // Cached structure and novel/next_id. Safe because the database is opened exclusively; every change
    // is written through atomically before the cache is updated.
    std::vector<Act> m_acts;
    Id m_nextId = 1;
};
