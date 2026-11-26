// Tests for the EPUB export: reads the archive back (ZIP + deflate) and checks its files.
// Usage: epub_test <scratch-dir>

#include "Epub.h"
#include "NovelStore.h"

#include <QByteArray>
#include <QString>
#include <QXmlStreamReader>
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

// Optional: NOVELCLANKER_EPUB_OUT=<folder> saves each test book there (e.g. to check them with EPUBCheck).
const std::string &saved(const std::string &name, const std::string &epub)
{
  if (const QString dir = qEnvironmentVariable("NOVELCLANKER_EPUB_OUT"); !dir.isEmpty()) {
    std::ofstream(fs::path(dir.toStdU16String()) / name, std::ios::binary) << epub;
  }
  return epub;
}

bool has(const std::string &text, const std::string &part)
{
  return text.find(part) != std::string::npos;
}

std::uint32_t read16(const std::string &s, std::size_t at)
{
  return static_cast<unsigned char>(s[at]) | (static_cast<unsigned char>(s[at + 1]) << 8);
}

std::uint32_t read32(const std::string &s, std::size_t at)
{
  return read16(s, at) | (read16(s, at + 2) << 16);
}

struct Entry {
  std::string name;
  int method = 0;
  std::string data; // uncompressed
};

// A minimal ZIP reader: walks the central directory, inflates entries and checks their CRC.
std::vector<Entry> unzip(const std::string &zip)
{
  std::vector<Entry> entries;
  const std::size_t end = zip.rfind(std::string("PK\x05\x06", 4));
  EXPECT(end != std::string::npos);
  if (end == std::string::npos) {
    return entries;
  }
  const std::uint32_t count = read16(zip, end + 10);
  std::size_t at = read32(zip, end + 16);
  for (std::uint32_t i = 0; i < count; ++i) {
    EXPECT(read32(zip, at) == 0x02014b50);
    Entry entry;
    entry.method = static_cast<int>(read16(zip, at + 10));
    const std::uint32_t crc = read32(zip, at + 16);
    const std::uint32_t packedSize = read32(zip, at + 20), size = read32(zip, at + 24);
    const std::uint32_t nameLength = read16(zip, at + 28);
    const std::uint32_t local = read32(zip, at + 42);
    entry.name = zip.substr(at + 46, nameLength);
    at += 46 + nameLength + read16(zip, at + 30) + read16(zip, at + 32);

    EXPECT(read32(zip, local) == 0x04034b50);
    const std::size_t dataAt = local + 30 + read16(zip, local + 26) + read16(zip, local + 28);
    const std::string packed = zip.substr(dataAt, packedSize);
    if (entry.method == 0) {
      entry.data = packed;
    } else {
      entry.data.resize(size);
      z_stream stream{};
      inflateInit2(&stream, -MAX_WBITS);
      stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(packed.data()));
      stream.avail_in = static_cast<uInt>(packed.size());
      stream.next_out = reinterpret_cast<Bytef *>(entry.data.data());
      stream.avail_out = static_cast<uInt>(size);
      EXPECT(inflate(&stream, Z_FINISH) == Z_STREAM_END);
      inflateEnd(&stream);
    }
    EXPECT(entry.data.size() == size);
    EXPECT(crc32(0, reinterpret_cast<const Bytef *>(entry.data.data()), static_cast<uInt>(size)) == crc);
    entries.push_back(std::move(entry));
  }
  return entries;
}

bool wellFormed(const std::string &xml, const std::string &name)
{
  QXmlStreamReader reader(QByteArray::fromStdString(xml));
  while (!reader.atEnd()) {
    reader.readNext();
  }
  if (reader.hasError()) {
    std::fprintf(stderr, "%s: line %lld: %s\n", name.c_str(), static_cast<long long>(reader.lineNumber()),
                 qPrintable(reader.errorString()));
  }
  return !reader.hasError();
}

std::map<std::string, std::string> files(const std::vector<Entry> &entries)
{
  std::map<std::string, std::string> out;
  for (const Entry &e : entries) {
    out[e.name] = e.data;
  }
  return out;
}

// @relation(FR-EXP-2, FR-EXP-3, FR-EXP-4, scope=function)
void testBook(const fs::path &root)
{
  std::string epub;
  {
    NovelStore store(root / "book.novel", NovelStore::Mode::Create);
    const auto act1 = store.addAct(0, "Part One: Home");
    const auto ch1 = store.addChapter(act1, 0, "Chapter 1: The Map Room");
    store.addScene(ch1, 0, "Ink", "Elin unrolled the **old** map.\n\nA street was *missing* & gone.");
    store.addScene(ch1, 1, "Hall", "She wrote <script>alert(1)</script> in the margin.\n\n> A quote.");
    store.addChapter(act1, 1, "Chapter 2: Silence");
    const auto act2 = store.addAct(1, "Part Two: Away");
    store.addScene(store.addChapter(act2, 0, "Chapter 3: Tide Tables"), 0, "Sea", "Salt.");
    store.setDetails({{"title", "The Map Room"},
                      {"subtitle", "A Novel"},
                      {"author", "E. Lindqvist"},
                      {"contributors", "Editor\tA. Editor\nTranslator\tT. Ranslator"},
                      {"language", "Swedish"},
                      {"publisher", "Harbour Press"},
                      {"description", "A cartographer & her town."},
                      {"keywords", "maps\nharbour"},
                      {"series_name", "Harbour Lights"},
                      {"series_number", "2"},
                      {"isbn", "978-0-306-40615-7"}});
    epub = saved("book.epub", Epub::write(store, {"book", "urn:uuid:12345678-1234-4abc-8def-1234567890ab",
                                                  "2026-11-25T12:00:00Z"}));
  }
  // "mimetype" first, uncompressed, with no extra field: readers find it at a fixed offset.
  EXPECT(epub.compare(0, 4, "PK\x03\x04") == 0);
  EXPECT(epub.compare(30, 8, "mimetype") == 0 && epub.compare(38, 20, "application/epub+zip") == 0);

  const auto entries = unzip(epub);
  EXPECT(!entries.empty() && entries[0].name == "mimetype" && entries[0].method == 0);
  const auto f = files(entries);
  for (const auto &[name, content] : f) {
    if (name != "mimetype" && name != "OEBPS/style.css") {
      EXPECT(wellFormed(content, name));
    }
  }
  EXPECT(has(f.at("META-INF/container.xml"), "full-path=\"OEBPS/content.opf\""));

  const std::string &opf = f.at("OEBPS/content.opf");
  EXPECT(
      has(opf, "<dc:identifier id=\"bookid\">urn:uuid:12345678-1234-4abc-8def-1234567890ab</dc:identifier>"));
  EXPECT(has(opf, "<dc:identifier id=\"isbn\">urn:isbn:9780306406157</dc:identifier>"));
  EXPECT(has(opf, "<dc:title id=\"book-title\">The Map Room</dc:title>"));
  EXPECT(has(opf, "<dc:title id=\"book-subtitle\">A Novel</dc:title>"));
  EXPECT(has(opf, "<dc:language>sv</dc:language>"));
  EXPECT(has(opf, "<dc:creator id=\"author\">E. Lindqvist</dc:creator>"));
  EXPECT(has(opf, "<dc:contributor id=\"contributor1\">A. Editor</dc:contributor>"));
  EXPECT(has(opf, "refines=\"#contributor2\" property=\"role\" scheme=\"marc:relators\">trl</meta>"));
  EXPECT(has(opf, "<dc:publisher>Harbour Press</dc:publisher>"));
  EXPECT(has(opf, "<dc:description>A cartographer &amp; her town.</dc:description>"));
  EXPECT(has(opf, "<dc:subject>maps</dc:subject>") && has(opf, "<dc:subject>harbour</dc:subject>"));
  EXPECT(has(opf, "<meta property=\"belongs-to-collection\" id=\"series\">Harbour Lights</meta>"));
  EXPECT(has(opf, "<meta refines=\"#series\" property=\"group-position\">2</meta>"));
  EXPECT(has(opf, "<meta property=\"dcterms:modified\">2026-11-25T12:00:00Z</meta>"));
  // Reading order: title page, then each part before its chapters.
  const std::regex itemref("idref=\"([^\"]+)\"");
  std::string spine;
  for (auto it = std::sregex_iterator(opf.begin(), opf.end(), itemref); it != std::sregex_iterator(); ++it) {
    spine += (spine.empty() ? "" : " ") + (*it)[1].str();
  }
  EXPECT(spine == "title part-1 chapter-1 chapter-2 part-2 chapter-3");
  // IDs are unique in the package document (metadata and manifest share one namespace).
  std::set<std::string> ids;
  const std::regex idAttribute(" id=\"([^\"]+)\"");
  for (auto it = std::sregex_iterator(opf.begin(), opf.end(), idAttribute); it != std::sregex_iterator();
       ++it) {
    expect(ids.insert((*it)[1].str()).second, ("unique id: " + (*it)[1].str()).c_str(), __LINE__);
  }
  // Every manifest item is in the archive.
  const std::regex href("href=\"([^\"]+)\"");
  for (auto it = std::sregex_iterator(opf.begin(), opf.end(), href); it != std::sregex_iterator(); ++it) {
    EXPECT(f.count("OEBPS/" + (*it)[1].str()) == 1);
  }

  const std::string &nav = f.at("OEBPS/nav.xhtml");
  EXPECT(has(nav, "epub:type=\"toc\""));
  EXPECT(has(nav, "<li><a href=\"part-1.xhtml\">Part One: Home</a>\n<ol>\n<li><a href=\"chapter-1.xhtml\">"
                  "Chapter 1: The Map Room</a></li>"));

  const std::string &title = f.at("OEBPS/title.xhtml");
  EXPECT(has(title, "<h1>The Map Room</h1>") && has(title, "Harbour Lights, book 2") &&
         has(title, "E. Lindqvist") && has(title, "xml:lang=\"sv\""));

  const std::string &chapter = f.at("OEBPS/chapter-1.xhtml");
  EXPECT(has(chapter, "<h2>Chapter 1: The Map Room</h2>"));
  EXPECT(has(chapter, "<strong>old</strong>") && has(chapter, "<em>missing</em> &amp; gone"));
  EXPECT(has(chapter, "<hr class=\"scene-break\"/>"));
  EXPECT(has(chapter, "&lt;script&gt;") && !has(chapter, "<script>")); // HTML in a scene is shown as text
  EXPECT(has(chapter, "<blockquote>"));
  EXPECT(has(f.at("OEBPS/chapter-2.xhtml"), "<h2>Chapter 2: Silence</h2>")); // a chapter without scenes
}

// One act: no part pages; nothing set: the fallback title, no author, undetermined language.
// @relation(FR-EXP-2, FR-EXP-3, scope=function)
void testMinimalBook(const fs::path &root)
{
  NovelStore store(root / "minimal.novel", NovelStore::Mode::Create);
  store.addScene(store.addChapter(store.addAct(0, "Act 1"), 0, "Chapter 1"), 0, "S", "Text.");
  const auto f =
      files(unzip(saved("minimal.epub", Epub::write(store, {"my-novel", Epub::newIdentifier(), {}}))));
  const std::string &opf = f.at("OEBPS/content.opf");
  EXPECT(has(opf, "<dc:title id=\"book-title\">my-novel</dc:title>"));
  EXPECT(!has(opf, "dc:creator") && has(opf, "<dc:language>und</dc:language>"));
  EXPECT(f.count("OEBPS/part-1.xhtml") == 0 && f.count("OEBPS/chapter-1.xhtml") == 1);
  EXPECT(
      std::regex_search(opf, std::regex("dcterms:modified\">\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\dZ<")));
  for (const auto &[name, content] : f) {
    if (name != "mimetype" && name != "OEBPS/style.css") {
      EXPECT(wellFormed(content, name));
    }
  }

  NovelStore empty(root / "empty.novel", NovelStore::Mode::Create);
  const auto e = files(unzip(saved("empty.epub", Epub::write(empty, {"", Epub::newIdentifier(), {}}))));
  EXPECT(has(e.at("OEBPS/content.opf"), ">Untitled</dc:title>"));
  EXPECT(has(e.at("OEBPS/nav.xhtml"), "<li><a href=\"title.xhtml\">")); // the contents list is never empty
  EXPECT(wellFormed(e.at("OEBPS/nav.xhtml"), "nav.xhtml"));
}

// @relation(FR-EXP-3, scope=function)
void testHelpers()
{
  EXPECT(Epub::languageCode("English") == "en" && Epub::languageCode(" swedish ") == "sv");
  EXPECT(Epub::languageCode("pt-BR") == "pt-BR" && Epub::languageCode("de") == "de");
  EXPECT(Epub::languageCode("") == "und" && Epub::languageCode("Klingon") == "und");
  const std::string a = Epub::newIdentifier(), b = Epub::newIdentifier();
  EXPECT(a != b);
  EXPECT(std::regex_match(
      a, std::regex("urn:uuid:[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")));
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

  testBook(root);
  testMinimalBook(root);
  testHelpers();

  if (g_failures == 0) {
    std::puts("All EPUB tests passed.");
  }
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
