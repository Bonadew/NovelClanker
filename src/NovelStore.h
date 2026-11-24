#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
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

// A novel stored in a RocksDB database (a directory on disk).
//
// Each chapter is its own entry holding the chapter's Markdown text. The reading order is kept in a
// separate entry, so reordering chapters never rewrites chapter text. Key layout:
//
//   novel/format            format version ("1")
//   novel/next_chapter_id   next unused chapter id (decimal)
//   novel/chapter_order     chapter ids in reading order (comma-separated decimals)
//   chapter/<id>            chapter Markdown; <id> is zero-padded to 20 digits
//
// RocksDB locks the database while it is open, so a novel can only be opened by one NovelStore (or
// process) at a time. All methods throw StorageError on failure.
class NovelStore
{
  public:
    using ChapterId = std::uint64_t;

    enum class Mode {
        Create, // create a new novel; fails if a database already exists at the path
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

    // Chapter ids in reading order.
    const std::vector<ChapterId> &chapters() const
    {
        return m_order;
    }

    std::string chapterText(ChapterId id) const;
    void setChapterText(ChapterId id, std::string_view markdown);

    // Inserts a new chapter so that it ends up at `position` in the reading order (clamped to the end).
    ChapterId insertChapter(std::size_t position, std::string_view markdown);
    void removeChapter(ChapterId id);
    void moveChapter(std::size_t from, std::size_t to);

  private:
    std::filesystem::path m_path;
    std::unique_ptr<rocksdb::DB> m_db;
    // Cached copies of novel/chapter_order and novel/next_chapter_id. Safe because the database is
    // opened exclusively; every change is written through atomically.
    std::vector<ChapterId> m_order;
    ChapterId m_nextId = 1;
};
