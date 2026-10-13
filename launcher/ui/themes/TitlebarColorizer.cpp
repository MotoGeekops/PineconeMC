// SPDX-License-Identifier: GPL-3.0-only
#include "TitlebarColorizer.h"

#include <QApplication>
#include <QEvent>
#include <QStyleHints>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>

#include <dwmapi.h>
#endif

namespace {

#ifdef Q_OS_WIN
// DWMWINDOWATTRIBUTE values, spelled out so older Windows SDK headers still compile
constexpr DWORD DwmUseImmersiveDarkMode = 20;
constexpr DWORD DwmCaptionColor = 35;
constexpr DWORD DwmTextColor = 36;
constexpr COLORREF DwmColorDefault = 0xFFFFFFFF;
#endif

bool hasTitlebar(const QWidget* widget)
{
    if (!widget->isWindow())
        return false;
    const auto type = widget->windowType();
    return type == Qt::Window || type == Qt::Dialog;
}

void paintTitlebar(QWidget* window, const QColor& color)
{
#ifdef Q_OS_WIN
    if (!window->testAttribute(Qt::WA_WState_Created))
        return;
    auto hwnd = reinterpret_cast<HWND>(window->winId());

    BOOL darkMode;
    COLORREF caption = DwmColorDefault;
    COLORREF text = DwmColorDefault;
    if (color.isValid()) {
        // dark mode also switches the minimize/maximize/close glyphs to a readable colour
        darkMode = color.lightnessF() < 0.5;
        caption = RGB(color.red(), color.green(), color.blue());
        text = darkMode ? RGB(0xF2, 0xF2, 0xF2) : RGB(0x1A, 0x1A, 0x1A);
    } else {
        darkMode = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    }
    DwmSetWindowAttribute(hwnd, DwmUseImmersiveDarkMode, &darkMode, sizeof(darkMode));
    DwmSetWindowAttribute(hwnd, DwmCaptionColor, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, DwmTextColor, &text, sizeof(text));
#else
    Q_UNUSED(window)
    Q_UNUSED(color)
#endif
}

}  // namespace

TitlebarColorizer::TitlebarColorizer(QObject* parent) : QObject(parent)
{
    qApp->installEventFilter(this);
}

void TitlebarColorizer::setColor(const QColor& color)
{
    m_color = color;
    for (auto* window : QApplication::topLevelWidgets()) {
        if (hasTitlebar(window))
            paintTitlebar(window, m_color);
    }
}

bool TitlebarColorizer::eventFilter(QObject* watched, QEvent* event)
{
    // ThemeChange: Windows may reset the dark-mode flag when the system theme flips
    const auto type = event->type();
    if ((type == QEvent::Show || type == QEvent::ThemeChange) && watched->isWidgetType()) {
        auto* widget = static_cast<QWidget*>(watched);
        if (hasTitlebar(widget))
            paintTitlebar(widget, m_color);
    }
    return QObject::eventFilter(watched, event);
}
