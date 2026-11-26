#pragma once

#include "NovelStore.h"

#include <string>
#include <string_view>

// Exports a novel as an EPUB 3 eBook (the format Amazon KDP, Apple Books, Kobo and most readers accept).
//
// The book's metadata comes from the novel's details (Novel Setup): title, subtitle, author and contributors,
// language, publisher, description, keywords, series and ISBN. The book has a title page, then one page per
// chapter (headed by its title) with the scenes' Markdown turned into XHTML and a scene break between scenes.
// When the novel has more than one act, each act also gets a part page, and the contents list chapters under
// their act.
namespace Epub {

struct Options {
  std::string fallbackTitle; // used when the novel has no title (e.g. its file name)
  std::string identifier;    // the book's permanent identifier, e.g. from newIdentifier()
  std::string modified;      // "2026-11-25T12:00:00Z"; empty: now
};

// The .epub file's bytes.
std::string write(const NovelStore &store, const Options &options);

// A new random identifier: "urn:uuid:..." (keep it for later exports of the same book).
std::string newIdentifier();

// BCP 47 language code for a language name ("English" -> "en", "Swedish" -> "sv"); a code is returned as is;
// "und" (undetermined) if unknown or empty.
std::string languageCode(std::string_view language);

} // namespace Epub
