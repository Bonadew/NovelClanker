#include "NovelStore.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace {

using ChapterId = NovelStore::ChapterId;

constexpr std::string_view kFormatKey = "novel/format";
constexpr std::string_view kFormatVersion = "1";
constexpr std::string_view kNextIdKey = "novel/next_chapter_id";
constexpr std::string_view kOrderKey = "novel/chapter_order";

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

std::string chapterKey(ChapterId id)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "chapter/%020llu", static_cast<unsigned long long>(id));
    return buf;
}

ChapterId parseId(std::string_view text)
{
    ChapterId value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size()) {
        throw StorageError("Corrupt novel: invalid number '" + std::string(text) + "'");
    }
    return value;
}

std::string encodeIds(const std::vector<ChapterId> &ids)
{
    std::string out;
    for (ChapterId id : ids) {
        if (!out.empty()) {
            out += ',';
        }
        out += std::to_string(id);
    }
    return out;
}

std::vector<ChapterId> decodeIds(std::string_view text)
{
    std::vector<ChapterId> ids;
    while (!text.empty()) {
        const auto comma = text.find(',');
        ids.push_back(parseId(text.substr(0, comma)));
        text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    }
    return ids;
}

// RocksDB takes paths as std::string. Pass UTF-8 so non-ASCII folder names work (on Windows this relies
// on RocksDB being built with WITH_WINDOWS_UTF8_FILENAMES, see profiles/windows-msvc).
std::string toUtf8(const std::filesystem::path &path)
{
    const std::u8string u8 = path.u8string();
    return {u8.begin(), u8.end()};
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
        throw StorageError("Not a NovelWriter novel: " + toUtf8(dir));
    }

    rocksdb::Options options;
    options.OptimizeForSmallDb();
    options.compression = rocksdb::kLZ4Compression;
    options.create_if_missing = mode == Mode::Create;
    options.error_if_exists = mode == Mode::Create;

    const auto opened = rocksdb::DB::Open(options, toUtf8(dir), &m_db);
    if (mode == Mode::Open && opened.IsInvalidArgument()) {
        // e.g. "CURRENT does not exist (create_if_missing is false)": not a database at all
        throw StorageError("Not a NovelWriter novel: " + toUtf8(dir));
    }
    check(opened, "Cannot open novel");

    if (mode == Mode::Create) {
        rocksdb::WriteBatch batch;
        batch.Put(slice(kFormatKey), slice(kFormatVersion));
        batch.Put(slice(kNextIdKey), slice(std::to_string(m_nextId)));
        batch.Put(slice(kOrderKey), slice(""));
        check(m_db->Write(durableWrites(), &batch), "Cannot initialize novel");
        return;
    }

    std::string value;
    const auto status = m_db->Get({}, slice(kFormatKey), &value);
    if (status.IsNotFound()) {
        throw StorageError("Not a NovelWriter novel: " + toUtf8(dir));
    }
    check(status, "Cannot read novel");
    if (value != kFormatVersion) {
        throw StorageError("Unsupported novel format version '" + value + "'");
    }

    check(m_db->Get({}, slice(kNextIdKey), &value), "Cannot read novel");
    m_nextId = parseId(value);
    check(m_db->Get({}, slice(kOrderKey), &value), "Cannot read novel");
    m_order = decodeIds(value);
}

NovelStore::~NovelStore()
{
    if (m_db) {
        m_db->Close();
    }
}

std::string NovelStore::chapterText(ChapterId id) const
{
    std::string text;
    check(m_db->Get({}, slice(chapterKey(id)), &text), "Cannot read chapter");
    return text;
}

void NovelStore::setChapterText(ChapterId id, std::string_view markdown)
{
    check(m_db->Put(durableWrites(), slice(chapterKey(id)), slice(markdown)), "Cannot save chapter");
}

NovelStore::ChapterId NovelStore::insertChapter(std::size_t position, std::string_view markdown)
{
    const ChapterId id = m_nextId;
    std::vector<ChapterId> order = m_order;
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(std::min(position, order.size())), id);

    rocksdb::WriteBatch batch;
    batch.Put(slice(chapterKey(id)), slice(markdown));
    batch.Put(slice(kNextIdKey), slice(std::to_string(id + 1)));
    batch.Put(slice(kOrderKey), slice(encodeIds(order)));
    check(m_db->Write(durableWrites(), &batch), "Cannot add chapter");

    m_nextId = id + 1;
    m_order = std::move(order);
    return id;
}

void NovelStore::removeChapter(ChapterId id)
{
    std::vector<ChapterId> order = m_order;
    std::erase(order, id);

    rocksdb::WriteBatch batch;
    batch.Delete(slice(chapterKey(id)));
    batch.Put(slice(kOrderKey), slice(encodeIds(order)));
    check(m_db->Write(durableWrites(), &batch), "Cannot delete chapter");

    m_order = std::move(order);
}

void NovelStore::moveChapter(std::size_t from, std::size_t to)
{
    if (from >= m_order.size() || to >= m_order.size() || from == to) {
        return;
    }
    std::vector<ChapterId> order = m_order;
    const ChapterId id = order[from];
    order.erase(order.begin() + static_cast<std::ptrdiff_t>(from));
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(to), id);

    check(m_db->Put(durableWrites(), slice(kOrderKey), slice(encodeIds(order))), "Cannot reorder chapters");
    m_order = std::move(order);
}
