#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Writes a ZIP archive in memory: entries are stored as given or compressed with deflate (zlib), in the order they
// are added. Enough for EPUB files (no ZIP64: entries and archive stay below 4 GiB).
class ZipWriter
{
  public:
    // `name` uses '/' between folders. `compress` false stores the data as is (EPUB's "mimetype" must be stored).
    void add(std::string_view name, std::string_view data, bool compress = true);

    // The complete archive. The writer can't be used afterwards.
    std::string finish();

  private:
    std::string m_archive;
    std::string m_centralDirectory;
    std::uint16_t m_entries = 0;
};
