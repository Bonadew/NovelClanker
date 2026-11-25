#pragma once

#include <QPalette>
#include <QString>

// Light and dark mode for the whole application.
//
// System follows the platform: its own style and colours (and its light/dark setting where it has one). Light and
// Dark use the Fusion style with a fixed palette, so they look the same on every platform, and also tell the
// platform which scheme is used (on Windows 11 this makes the title bar match).
namespace Theme {

enum class Mode { System, Light, Dark };

// The saved choice (per user, not per novel); System if none.
Mode load();
void save(Mode mode);

// Applies `mode` to the running application. Call after the QApplication exists.
void apply(Mode mode);

QString toString(Mode mode); // "system", "light", "dark"
Mode fromString(const QString &text); // System for anything unknown

// True when the palette has light text on a dark background.
bool isDark(const QPalette &palette);

} // namespace Theme
