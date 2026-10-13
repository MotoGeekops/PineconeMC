// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QColor>
#include <QObject>

/// Paints the native title bar of every top-level window with the theme's window colour,
/// including windows opened later. Needs Windows 11; older versions ignore it.
/// An invalid colour hands the title bar back to the system.
class TitlebarColorizer : public QObject {
    Q_OBJECT

   public:
    explicit TitlebarColorizer(QObject* parent = nullptr);

    void setColor(const QColor& color);

   protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

   private:
    QColor m_color;
};
