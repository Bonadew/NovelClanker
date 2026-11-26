#include "ZipWriter.h"

#include <zlib.h>

#include <ctime>
#include <stdexcept>

namespace {

void put16(std::string &out, std::uint16_t v)
{
  out += static_cast<char>(v & 0xff);
  out += static_cast<char>(v >> 8);
}

void put32(std::string &out, std::uint32_t v)
{
  put16(out, static_cast<std::uint16_t>(v & 0xffff));
  put16(out, static_cast<std::uint16_t>(v >> 16));
}

// The current local time in MS-DOS format (what ZIP stores).
void dosTime(std::uint16_t &time, std::uint16_t &date)
{
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  time = static_cast<std::uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
  date = static_cast<std::uint16_t>(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

std::string deflateRaw(std::string_view data)
{
  z_stream stream{};
  if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    throw std::runtime_error("Cannot start compression");
  }
  std::string out(deflateBound(&stream, static_cast<uLong>(data.size())), '\0');
  stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
  stream.avail_in = static_cast<uInt>(data.size());
  stream.next_out = reinterpret_cast<Bytef *>(out.data());
  stream.avail_out = static_cast<uInt>(out.size());
  const int result = deflate(&stream, Z_FINISH);
  out.resize(stream.total_out);
  deflateEnd(&stream);
  if (result != Z_STREAM_END) {
    throw std::runtime_error("Cannot compress");
  }
  return out;
}

} // namespace

void ZipWriter::add(std::string_view name, std::string_view data, bool compress)
{
  if (m_entries == 0xffff || data.size() >= 0xffffffffu || m_archive.size() >= 0xffffffffu) {
    throw std::runtime_error("Too much data for a ZIP file");
  }
  const auto crc = static_cast<std::uint32_t>(
      crc32(0, reinterpret_cast<const Bytef *>(data.data()), static_cast<uInt>(data.size())));
  const std::string packed = compress ? deflateRaw(data) : std::string(data);
  const std::uint16_t method = compress ? 8 : 0;
  std::uint16_t time = 0, date = 0;
  dosTime(time, date);
  const auto offset = static_cast<std::uint32_t>(m_archive.size());
  constexpr std::uint16_t kUtf8Names = 0x0800;

  // Local file header
  put32(m_archive, 0x04034b50);
  put16(m_archive, 20); // version needed: 2.0
  put16(m_archive, kUtf8Names);
  put16(m_archive, method);
  put16(m_archive, time);
  put16(m_archive, date);
  put32(m_archive, crc);
  put32(m_archive, static_cast<std::uint32_t>(packed.size()));
  put32(m_archive, static_cast<std::uint32_t>(data.size()));
  put16(m_archive, static_cast<std::uint16_t>(name.size()));
  put16(m_archive, 0); // no extra field
  m_archive += name;
  m_archive += packed;

  // Central directory entry
  put32(m_centralDirectory, 0x02014b50);
  put16(m_centralDirectory, 20); // made by: 2.0
  put16(m_centralDirectory, 20);
  put16(m_centralDirectory, kUtf8Names);
  put16(m_centralDirectory, method);
  put16(m_centralDirectory, time);
  put16(m_centralDirectory, date);
  put32(m_centralDirectory, crc);
  put32(m_centralDirectory, static_cast<std::uint32_t>(packed.size()));
  put32(m_centralDirectory, static_cast<std::uint32_t>(data.size()));
  put16(m_centralDirectory, static_cast<std::uint16_t>(name.size()));
  put16(m_centralDirectory, 0); // extra
  put16(m_centralDirectory, 0); // comment
  put16(m_centralDirectory, 0); // disk
  put16(m_centralDirectory, 0); // internal attributes
  put32(m_centralDirectory, 0); // external attributes
  put32(m_centralDirectory, offset);
  m_centralDirectory += name;
  ++m_entries;
}

std::string ZipWriter::finish()
{
  const auto directoryOffset = static_cast<std::uint32_t>(m_archive.size());
  m_archive += m_centralDirectory;
  put32(m_archive, 0x06054b50); // end of central directory
  put16(m_archive, 0);
  put16(m_archive, 0);
  put16(m_archive, m_entries);
  put16(m_archive, m_entries);
  put32(m_archive, static_cast<std::uint32_t>(m_centralDirectory.size()));
  put32(m_archive, directoryOffset);
  put16(m_archive, 0); // no comment
  return std::move(m_archive);
}
