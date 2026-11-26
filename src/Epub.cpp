#include "Epub.h"

#include "ZipWriter.h"

#include <md4c-html.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <map>
#include <random>
#include <vector>

namespace Epub {

namespace {

std::string escape(std::string_view text)
{
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    default:
      // Control characters other than tab and newline are not allowed in XML.
      if (static_cast<unsigned char>(c) >= 0x20 || c == '\t' || c == '\n') {
        out += c;
      }
    }
  }
  return out;
}

std::vector<std::string> lines(const std::string &text)
{
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      out.push_back(std::move(line));
    }
    start = end + 1;
  }
  return out;
}

// Scene Markdown -> XHTML. Raw HTML in the text is shown as text, so the result is always well-formed.
std::string markdownToXhtml(const std::string &markdown)
{
  std::string out;
  md_html(
      markdown.data(), static_cast<MD_SIZE>(markdown.size()),
      [](const MD_CHAR *text, MD_SIZE size, void *userdata) {
        static_cast<std::string *>(userdata)->append(text, size);
      },
      &out, MD_FLAG_NOHTML | MD_FLAG_STRIKETHROUGH, MD_HTML_FLAG_XHTML);
  return out;
}

// MARC relator codes for the contributor roles of Novel Setup.
std::string relator(const std::string &role)
{
  static const std::map<std::string, std::string> codes{
      {"Author", "aut"},       {"Editor", "edt"},   {"Illustrator", "ill"},
      {"Translator", "trl"},   {"Narrator", "nrt"}, {"Photographer", "pht"},
      {"Introduction", "aui"}, {"Foreword", "aui"}, {"Preface", "aui"}};
  const auto it = codes.find(role);
  return it == codes.end() ? "ctb" : it->second;
}

std::string nowUtc()
{
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
}

const char *kStyle = R"CSS(body { margin: 0 5%; font-family: serif; line-height: 1.4; }
h1, h2 { text-align: center; font-weight: normal; margin: 2em 0 1.5em; page-break-after: avoid; }
h1 { font-size: 1.8em; }
h2 { font-size: 1.4em; }
p { margin: 0; text-indent: 1.5em; }
h1 + p, h2 + p, h3 + p, hr + p, blockquote p { text-indent: 0; }
hr.scene-break { border: none; margin: 1.5em 0; text-align: center; }
hr.scene-break::after { content: "* * *"; letter-spacing: 0.5em; }
blockquote { margin: 1em 2em; font-style: italic; }
.title-page { text-align: center; margin-top: 25%; }
.title-page h1 { margin-bottom: 0.5em; }
.subtitle { font-size: 1.2em; font-style: italic; margin-bottom: 2em; }
.series { margin-bottom: 2em; }
.author { font-size: 1.2em; }
.title-page p, .part p { text-indent: 0; }
.part { margin-top: 30%; }
)CSS";

class Book {
public:
  Book(const NovelStore &store, const Options &options) : m_store(store), m_options(options)
  {
    m_details = store.details();
    m_title = detail("title").empty() ? options.fallbackTitle : detail("title");
    if (m_title.empty()) {
      m_title = "Untitled";
    }
    m_language = languageCode(detail("language"));
  }

  std::string build()
  {
    addTitlePage();
    const auto &acts = m_store.acts();
    const bool parts = acts.size() > 1;
    for (const auto &act : acts) {
      if (parts) {
        const std::string file = "part-" + std::to_string(++m_partCount) + ".xhtml";
        addPage(file, act.title,
                "<section class=\"part\" epub:type=\"part\">\n<h1>" + escape(act.title) +
                    "</h1>\n</section>");
        m_toc += "<li><a href=\"" + file + "\">" + escape(act.title) + "</a>";
        m_toc += act.chapters.empty() ? "</li>\n" : "\n<ol>\n";
      }
      for (const auto &chapter : act.chapters) {
        addChapter(chapter);
      }
      if (parts && !act.chapters.empty()) {
        m_toc += "</ol>\n</li>\n";
      }
    }

    ZipWriter zip;
    zip.add("mimetype", "application/epub+zip", false); // first and uncompressed, as EPUB requires
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
            "<rootfiles>\n<rootfile full-path=\"OEBPS/content.opf\" "
            "media-type=\"application/oebps-package+xml\"/>\n"
            "</rootfiles>\n</container>\n");
    zip.add("OEBPS/content.opf", packageDocument());
    zip.add("OEBPS/nav.xhtml", navigation());
    zip.add("OEBPS/style.css", kStyle);
    for (const auto &[file, content] : m_pages) {
      zip.add("OEBPS/" + file, content);
    }
    return zip.finish();
  }

private:
  std::string detail(const char *name) const
  {
    const auto it = m_details.find(name);
    return it == m_details.end() ? std::string() : it->second;
  }

  std::string page(const std::string &title, const std::string &body) const
  {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n"
           "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" "
           "xml:lang=\"" +
           m_language + "\" lang=\"" + m_language + "\">\n<head>\n<meta charset=\"UTF-8\"/>\n<title>" +
           escape(title) +
           "</title>\n<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/>\n</head>\n<body>\n" +
           body + "\n</body>\n</html>\n";
  }

  void addPage(const std::string &file, const std::string &title, const std::string &body)
  {
    m_pages.emplace_back(file, page(title, body));
  }

  void addTitlePage()
  {
    std::string body =
        "<section class=\"title-page\" epub:type=\"titlepage\">\n<h1>" + escape(m_title) + "</h1>\n";
    if (!detail("subtitle").empty()) {
      body += "<p class=\"subtitle\">" + escape(detail("subtitle")) + "</p>\n";
    }
    if (!detail("series_name").empty()) {
      body += "<p class=\"series\">" + escape(detail("series_name")) +
              (detail("series_number").empty() ? "" : ", book " + escape(detail("series_number"))) + "</p>\n";
    }
    if (!detail("author").empty()) {
      body += "<p class=\"author\">" + escape(detail("author")) + "</p>\n";
    }
    body += "</section>";
    addPage("title.xhtml", m_title, body);
  }

  void addChapter(const NovelStore::Chapter &chapter)
  {
    const std::string file = "chapter-" + std::to_string(++m_chapterCount) + ".xhtml";
    std::string body = "<section epub:type=\"chapter\">\n<h2>" + escape(chapter.title) + "</h2>\n";
    for (std::size_t s = 0; s < chapter.scenes.size(); ++s) {
      if (s > 0) {
        body += "<hr class=\"scene-break\"/>\n";
      }
      body += markdownToXhtml(m_store.sceneText(chapter.scenes[s].id));
    }
    body += "</section>";
    addPage(file, chapter.title, body);
    m_toc += "<li><a href=\"" + file + "\">" + escape(chapter.title) + "</a></li>\n";
  }

  std::string navigation() const
  {
    // A contents list can't be empty: a novel without chapters lists its title page.
    const std::string entries =
        m_toc.empty() ? "<li><a href=\"title.xhtml\">" + escape(m_title) + "</a></li>\n" : m_toc;
    return page("Contents",
                "<nav epub:type=\"toc\" id=\"toc\">\n<h1>Contents</h1>\n<ol>\n" + entries + "</ol>\n</nav>");
  }

  std::string packageDocument() const
  {
    std::string meta;
    meta += "<dc:identifier id=\"bookid\">" + escape(m_options.identifier) + "</dc:identifier>\n";
    std::string isbn = detail("isbn");
    isbn.erase(std::remove_if(isbn.begin(), isbn.end(), [](char c) { return c == '-' || c == ' '; }),
               isbn.end());
    if (!isbn.empty()) {
      meta += "<dc:identifier id=\"isbn\">urn:isbn:" + escape(isbn) + "</dc:identifier>\n";
    }
    meta += "<dc:title id=\"book-title\">" + escape(m_title) +
            "</dc:title>\n"
            "<meta refines=\"#book-title\" property=\"title-type\">main</meta>\n";
    if (!detail("subtitle").empty()) {
      meta += "<dc:title id=\"book-subtitle\">" + escape(detail("subtitle")) +
              "</dc:title>\n"
              "<meta refines=\"#book-subtitle\" property=\"title-type\">subtitle</meta>\n";
    }
    meta += "<dc:language>" + m_language + "</dc:language>\n";
    if (!detail("author").empty()) {
      meta += "<dc:creator id=\"author\">" + escape(detail("author")) +
              "</dc:creator>\n"
              "<meta refines=\"#author\" property=\"role\" scheme=\"marc:relators\">aut</meta>\n";
    }
    int n = 0;
    for (const std::string &line : lines(detail("contributors"))) {
      const std::size_t tab = line.find('\t');
      const std::string role = tab == std::string::npos ? std::string() : line.substr(0, tab);
      const std::string name = tab == std::string::npos ? line : line.substr(tab + 1);
      const std::string id = "contributor" + std::to_string(++n);
      meta += "<dc:contributor id=\"" + id + "\">" + escape(name) + "</dc:contributor>\n<meta refines=\"#" +
              id + "\" property=\"role\" scheme=\"marc:relators\">" + relator(role) + "</meta>\n";
    }
    if (!detail("publisher").empty()) {
      meta += "<dc:publisher>" + escape(detail("publisher")) + "</dc:publisher>\n";
    }
    if (!detail("description").empty()) {
      meta += "<dc:description>" + escape(detail("description")) + "</dc:description>\n";
    }
    for (const std::string &keyword : lines(detail("keywords"))) {
      meta += "<dc:subject>" + escape(keyword) + "</dc:subject>\n";
    }
    if (!detail("series_name").empty()) {
      meta += "<meta property=\"belongs-to-collection\" id=\"series\">" + escape(detail("series_name")) +
              "</meta>\n<meta refines=\"#series\" property=\"collection-type\">series</meta>\n";
      if (!detail("series_number").empty()) {
        meta += "<meta refines=\"#series\" property=\"group-position\">" + escape(detail("series_number")) +
                "</meta>\n";
      }
    }
    meta += "<meta property=\"dcterms:modified\">" +
            (m_options.modified.empty() ? nowUtc() : m_options.modified) + "</meta>\n";

    std::string manifest =
        "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" "
        "properties=\"nav\"/>\n<item id=\"style\" href=\"style.css\" media-type=\"text/css\"/>\n";
    std::string spine;
    for (const auto &[file, content] : m_pages) {
      const std::string id = file.substr(0, file.find('.'));
      manifest += "<item id=\"" + id + "\" href=\"" + file + "\" media-type=\"application/xhtml+xml\"/>\n";
      spine += "<itemref idref=\"" + id + "\"/>\n";
    }
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"bookid\" "
           "xml:lang=\"" +
           m_language + "\">\n<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n" + meta +
           "</metadata>\n<manifest>\n" + manifest + "</manifest>\n<spine>\n" + spine +
           "</spine>\n</package>\n";
  }

  const NovelStore &m_store;
  const Options &m_options;
  std::map<std::string, std::string> m_details;
  std::string m_title;
  std::string m_language;
  std::vector<std::pair<std::string, std::string>> m_pages; // file name, content, in reading order
  std::string m_toc;
  int m_chapterCount = 0;
  int m_partCount = 0;
};

} // namespace

std::string write(const NovelStore &store, const Options &options)
{
  return Book(store, options).build();
}

std::string newIdentifier()
{
  std::random_device random;
  std::uniform_int_distribution<int> byte(0, 255);
  unsigned char b[16];
  for (unsigned char &x : b) {
    x = static_cast<unsigned char>(byte(random));
  }
  b[6] = static_cast<unsigned char>((b[6] & 0x0f) | 0x40); // version 4 (random)
  b[8] = static_cast<unsigned char>((b[8] & 0x3f) | 0x80); // RFC 4122 variant
  char text[64];
  std::snprintf(text, sizeof text,
                "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return text;
}

std::string languageCode(std::string_view language)
{
  static const std::map<std::string, std::string> codes{
      {"english", "en"},    {"german", "de"},    {"french", "fr"},  {"spanish", "es"}, {"italian", "it"},
      {"portuguese", "pt"}, {"dutch", "nl"},     {"swedish", "sv"}, {"danish", "da"},  {"norwegian", "no"},
      {"finnish", "fi"},    {"icelandic", "is"}, {"polish", "pl"},  {"czech", "cs"},   {"japanese", "ja"},
      {"chinese", "zh"},    {"hindi", "hi"},     {"arabic", "ar"},  {"hebrew", "he"},  {"greek", "el"},
      {"turkish", "tr"},    {"ukrainian", "uk"}, {"russian", "ru"}, {"latin", "la"}};
  std::string key;
  for (const char c : language) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  if (const auto it = codes.find(key); it != codes.end()) {
    return it->second;
  }
  // Already a code: "en", "sv", "pt-BR", "zh-Hant".
  const bool code =
      key.size() >= 2 && key.size() <= 12 && std::isalpha(static_cast<unsigned char>(key[0])) &&
      std::all_of(key.begin(), key.end(),
                  [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-'; }) &&
      (key.size() <= 3 || key[2] == '-' || key[3] == '-');
  if (code) {
    std::string original(language);
    original.erase(std::remove_if(original.begin(), original.end(),
                                  [](char c) { return std::isspace(static_cast<unsigned char>(c)); }),
                   original.end());
    return original;
  }
  return "und";
}

} // namespace Epub
