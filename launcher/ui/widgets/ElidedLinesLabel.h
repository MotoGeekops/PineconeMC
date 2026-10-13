// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QPainter>
#include <QStringList>
#include <QWidget>

/// A few centred lines of muted text. Long lines are elided rather than wrapped, because
/// toolbar layouts (like the instance side panel) can't do height-for-width.
/// It never asks for width, so it can't make the panel wider.
class ElidedLinesLabel : public QWidget {
   public:
    explicit ElidedLinesLabel(QWidget* parent = nullptr) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }

    void setLines(const QStringList& lines)
    {
        m_lines = lines;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        const int lineSpacing = fontMetrics().lineSpacing();
        return { 0, m_lines.isEmpty() ? 0 : static_cast<int>(m_lines.size()) * lineSpacing + 2 * verticalPadding };
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

   protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        QColor color = palette().color(QPalette::WindowText);
        color.setAlphaF(0.65f);
        painter.setPen(color);
        const int lineSpacing = fontMetrics().lineSpacing();
        int top = verticalPadding;
        for (const auto& line : m_lines) {
            const QRect lineRect(horizontalPadding, top, width() - 2 * horizontalPadding, lineSpacing);
            painter.drawText(lineRect, Qt::AlignCenter, fontMetrics().elidedText(line, Qt::ElideRight, lineRect.width()));
            top += lineSpacing;
        }
    }

   private:
    static constexpr int horizontalPadding = 6;
    static constexpr int verticalPadding = 3;
    QStringList m_lines;
};
