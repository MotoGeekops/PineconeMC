// SPDX-License-Identifier: GPL-3.0-only
#include "PineconeTheme.h"

#include <QFile>
#include <QObject>

QString PineconeTheme::id()
{
    return "pinecone";
}

QString PineconeTheme::name()
{
    return QObject::tr("Pinecone");
}

QString PineconeTheme::tooltip()
{
    return QObject::tr("Dark theme with floating cards and PineconeMC greens");
}

QPalette PineconeTheme::colorScheme()
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(0x20, 0x20, 0x20));
    palette.setColor(QPalette::WindowText, QColor(0xec, 0xec, 0xec));
    palette.setColor(QPalette::Base, QColor(0x19, 0x19, 0x19));
    palette.setColor(QPalette::AlternateBase, QColor(0x1d, 0x1d, 0x1d));
    palette.setColor(QPalette::ToolTipBase, QColor(0x2a, 0x2a, 0x2a));
    palette.setColor(QPalette::ToolTipText, QColor(0xf0, 0xf0, 0xf0));
    palette.setColor(QPalette::Text, QColor(0xec, 0xec, 0xec));
    palette.setColor(QPalette::Button, QColor(0x2c, 0x2c, 0x2c));
    palette.setColor(QPalette::ButtonText, QColor(0xec, 0xec, 0xec));
    palette.setColor(QPalette::BrightText, QColor(0xff, 0x7b, 0x72));
    palette.setColor(QPalette::Link, QColor(0x2f, 0xa3, 0xc6));
    palette.setColor(QPalette::Highlight, QColor(0x96, 0xdb, 0x59));
    palette.setColor(QPalette::HighlightedText, QColor(0x0f, 0x1a, 0x0a));
    palette.setColor(QPalette::PlaceholderText, QColor(0x6b, 0x6b, 0x6b));
    return fadeInactive(palette, fadeAmount(), fadeColor());
}

double PineconeTheme::fadeAmount()
{
    return 0.55;
}

QColor PineconeTheme::fadeColor()
{
    return QColor(0x20, 0x20, 0x20);
}

bool PineconeTheme::hasStyleSheet()
{
    return true;
}

QString PineconeTheme::appStyleSheet()
{
    QFile file(":/themes/pinecone/themeStyle.css");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

QStringList PineconeTheme::searchPaths()
{
    return { ":/themes/pinecone/resources" };
}

LogColors PineconeTheme::logColorScheme()
{
    LogColors colors;
    colors.foreground[MessageLevel::Launcher] = QColor(0x96, 0xdb, 0x59);
    colors.foreground[MessageLevel::Debug] = QColor(0x7d, 0x9a, 0x8c);
    colors.foreground[MessageLevel::Warning] = QColor(0xe3, 0xb3, 0x41);
    colors.foreground[MessageLevel::Error] = QColor(0xff, 0x7b, 0x72);
    colors.foreground[MessageLevel::Fatal] = QColor(0xff, 0xd7, 0xd4);
    colors.background[MessageLevel::Fatal] = QColor(0x5c, 0x1a, 0x1a);
    return colors;
}
