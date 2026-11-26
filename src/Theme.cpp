#include "Theme.h"

#include <QApplication>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace Theme {

namespace {

constexpr auto kThemeKey = "appearance/theme";

QPalette lightPalette()
{
  QPalette p;
  const QColor window(0xef, 0xef, 0xef), text(0x1e, 0x1e, 0x1e), base(0xff, 0xff, 0xff);
  const QColor highlight(0x30, 0x7c, 0xd6), disabled(0x9a, 0x9a, 0x9a);
  p.setColor(QPalette::Window, window);
  p.setColor(QPalette::WindowText, text);
  p.setColor(QPalette::Base, base);
  p.setColor(QPalette::AlternateBase, QColor(0xf5, 0xf5, 0xf5));
  p.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xdc));
  p.setColor(QPalette::ToolTipText, text);
  p.setColor(QPalette::PlaceholderText, QColor(0x80, 0x80, 0x80));
  p.setColor(QPalette::Text, text);
  p.setColor(QPalette::Button, window);
  p.setColor(QPalette::ButtonText, text);
  p.setColor(QPalette::BrightText, Qt::red);
  p.setColor(QPalette::Light, Qt::white);
  p.setColor(QPalette::Midlight, QColor(0xe3, 0xe3, 0xe3));
  p.setColor(QPalette::Mid, QColor(0xa0, 0xa0, 0xa0));
  p.setColor(QPalette::Dark, QColor(0x9f, 0x9f, 0x9f));
  p.setColor(QPalette::Shadow, QColor(0x76, 0x76, 0x76));
  p.setColor(QPalette::Highlight, highlight);
  p.setColor(QPalette::HighlightedText, Qt::white);
  p.setColor(QPalette::Link, QColor(0x1a, 0x5f, 0xb4));
  p.setColor(QPalette::LinkVisited, QColor(0x6b, 0x3f, 0xa0));
  for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
    p.setColor(QPalette::Disabled, role, disabled);
  }
  return p;
}

// Google's dark palette: Google Grey 900 backgrounds, near-white text, and the
// light blue Google uses as the accent colour on dark surfaces.
QPalette darkPalette()
{
  QPalette p;
  const QColor window(0x20, 0x21, 0x24), text(0xe8, 0xea, 0xed), base(0x20, 0x21, 0x24);
  const QColor highlight(0x8a, 0xb4, 0xf8), disabled(0x5f, 0x63, 0x68);
  p.setColor(QPalette::Window, window);
  p.setColor(QPalette::WindowText, text);
  p.setColor(QPalette::Base, base);
  p.setColor(QPalette::AlternateBase, QColor(0x30, 0x31, 0x34));
  p.setColor(QPalette::ToolTipBase, QColor(0x30, 0x31, 0x34));
  p.setColor(QPalette::ToolTipText, text);
  p.setColor(QPalette::PlaceholderText, QColor(0x9a, 0xa0, 0xa6));
  p.setColor(QPalette::Text, text);
  p.setColor(QPalette::Button, QColor(0x30, 0x31, 0x34));
  p.setColor(QPalette::ButtonText, text);
  p.setColor(QPalette::BrightText, QColor(0xf2, 0x8b, 0x82));
  p.setColor(QPalette::Light, QColor(0x3c, 0x40, 0x43));
  p.setColor(QPalette::Midlight, QColor(0x35, 0x36, 0x3a));
  p.setColor(QPalette::Mid, window);
  p.setColor(QPalette::Dark, QColor(0x13, 0x14, 0x17));
  p.setColor(QPalette::Shadow, QColor(0x10, 0x10, 0x11));
  p.setColor(QPalette::Highlight, highlight);
  p.setColor(QPalette::HighlightedText, window); // Google pairs the blue accent with dark text
  p.setColor(QPalette::Link, highlight);
  p.setColor(QPalette::LinkVisited, QColor(0xc5, 0x8a, 0xf9));
  for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
    p.setColor(QPalette::Disabled, role, disabled);
  }
  return p;
}

// The platform's own style, remembered before the first change so System can return to it.
QString systemStyleName()
{
  static const QString name = QApplication::style() ? QApplication::style()->name() : QString();
  return name;
}

} // namespace

Mode load()
{
  return fromString(QSettings().value(kThemeKey).toString());
}

void save(Mode mode)
{
  QSettings().setValue(kThemeKey, toString(mode));
}

void apply(Mode mode)
{
  const QString systemStyle = systemStyleName();
  switch (mode) {
  case Mode::System:
    QGuiApplication::styleHints()->unsetColorScheme();
    if (!systemStyle.isEmpty() && QApplication::style()->name() != systemStyle) {
      QApplication::setStyle(QStyleFactory::create(systemStyle));
    }
    QApplication::setPalette(QPalette()); // a default palette resets to the platform's colours
    break;
  case Mode::Light:
  case Mode::Dark: {
    const bool dark = mode == Mode::Dark;
    QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    if (QApplication::style()->name().compare(QStringLiteral("fusion"), Qt::CaseInsensitive) != 0) {
      QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }
    QApplication::setPalette(dark ? darkPalette() : lightPalette());
    break;
  }
  }
}

QString toString(Mode mode)
{
  switch (mode) {
  case Mode::Light:
    return QStringLiteral("light");
  case Mode::Dark:
    return QStringLiteral("dark");
  case Mode::System:
    break;
  }
  return QStringLiteral("system");
}

Mode fromString(const QString &text)
{
  if (text == QLatin1String("light")) {
    return Mode::Light;
  }
  if (text == QLatin1String("dark")) {
    return Mode::Dark;
  }
  return Mode::System;
}

bool isDark(const QPalette &palette)
{
  return palette.color(QPalette::Window).lightness() < palette.color(QPalette::WindowText).lightness();
}

} // namespace Theme
