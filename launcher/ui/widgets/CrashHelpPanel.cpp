// SPDX-License-Identifier: GPL-3.0-only
#include "CrashHelpPanel.h"

#include <QDir>
#include <QEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "Application.h"
#include "BuildConfig.h"
#include "DesktopServices.h"
#include "FileSystem.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/mod/ModFolderModel.h"
#include "minecraft/mod/Resource.h"

CrashHelpPanel::CrashHelpPanel(QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("crashHelp"));
    // a plain raised panel for themes without a style sheet rule for it
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::AlternateBase);

    m_icon = new QLabel(this);
    m_icon->setObjectName(QStringLiteral("crashHelpIcon"));
    m_icon->setPixmap(QIcon::fromTheme("status-bad").pixmap(QSize(28, 28), devicePixelRatioF()));
    m_icon->setFixedSize(28, 28);

    m_caption = new QLabel(tr("Why the game stopped"), this);
    m_caption->setObjectName(QStringLiteral("crashHelpCaption"));
    QFont captionFont = m_caption->font();
    captionFont.setCapitalization(QFont::AllUppercase);
    captionFont.setLetterSpacing(QFont::PercentageSpacing, 108);
    captionFont.setPointSizeF(captionFont.pointSizeF() * 0.85);
    captionFont.setBold(true);
    m_caption->setFont(captionFont);

    m_content = new QWidget(this);
    auto* contentLayout = new QVBoxLayout(m_content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(10);

    auto* closeButton = new QToolButton(this);
    closeButton->setObjectName(QStringLiteral("crashHelpCloseButton"));
    closeButton->setText(QString(QChar(0x2715)));
    closeButton->setAutoRaise(true);
    closeButton->setToolTip(tr("Hide"));
    connect(closeButton, &QToolButton::clicked, this, &QWidget::hide);

    auto* text = new QVBoxLayout;
    text->setSpacing(4);
    text->addWidget(m_caption);
    text->addWidget(m_content);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 10, 8, 12);
    layout->setSpacing(12);
    layout->addWidget(m_icon, 0, Qt::AlignTop);
    layout->addLayout(text, 1);
    layout->addWidget(closeButton, 0, Qt::AlignTop);

    hide();
}

void CrashHelpPanel::showDiagnoses(MinecraftInstance* instance, const QList<CrashAnalyzer::Diagnosis>& diagnoses)
{
    m_instance = instance;
    m_mutedLabels.clear();
    qDeleteAll(m_content->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly));
    if (!instance || diagnoses.isEmpty()) {
        hide();
        return;
    }

    auto* contentLayout = static_cast<QVBoxLayout*>(m_content->layout());
    // more than three causes at once is more noise than help
    for (const auto& diagnosis : diagnoses.mid(0, 3)) {
        auto* block = new QWidget(m_content);
        auto* blockLayout = new QVBoxLayout(block);
        blockLayout->setContentsMargins(0, 0, 0, 0);
        blockLayout->setSpacing(3);

        auto* title = new QLabel(diagnosis.title, block);
        title->setObjectName(QStringLiteral("crashHelpTitle"));
        title->setWordWrap(true);
        QFont titleFont = title->font();
        titleFont.setPointSizeF(titleFont.pointSizeF() * 1.15);
        titleFont.setBold(true);
        title->setFont(titleFont);
        blockLayout->addWidget(title);

        auto* explanation = new QLabel(diagnosis.explanation, block);
        explanation->setObjectName(QStringLiteral("crashHelpExplanation"));
        explanation->setWordWrap(true);
        explanation->setTextInteractionFlags(Qt::TextSelectableByMouse);
        blockLayout->addWidget(explanation);

        auto* status = new QLabel(block);
        status->setObjectName(QStringLiteral("crashHelpStatus"));
        status->setWordWrap(true);
        status->hide();

        if (!diagnosis.fixes.isEmpty()) {
            auto* buttons = new QHBoxLayout;
            buttons->setSpacing(6);
            buttons->setContentsMargins(0, 4, 0, 0);
            bool first = true;
            for (const auto& fix : diagnosis.fixes) {
                auto* button = new QPushButton(fix.label, block);
                button->setObjectName(first ? QStringLiteral("crashHelpPrimaryButton") : QStringLiteral("crashHelpButton"));
                button->setCursor(Qt::PointingHandCursor);
                first = false;
                connect(button, &QPushButton::clicked, this, [this, fix, button, status] {
                    const QString done = applyFix(m_instance, fix);
                    if (done.isEmpty())
                        return;
                    button->setEnabled(false);
                    status->setText(done);
                    status->show();
                });
                buttons->addWidget(button);
            }
            buttons->addStretch(1);
            blockLayout->addLayout(buttons);
        }
        blockLayout->addWidget(status);
        contentLayout->addWidget(block);
        m_mutedLabels << explanation;
    }
    updateMutedColors();
    show();
}

QString CrashHelpPanel::applyFix(MinecraftInstance* instance, const CrashAnalyzer::Fix& fix)
{
    using Action = CrashAnalyzer::Fix::Action;
    if (!instance)
        return {};
    auto settings = instance->settings();

    switch (fix.action) {
        case Action::UseRecommendedJava: {
            // the launcher then picks (or downloads) the Java this Minecraft version wants when it starts the game
            APPLICATION->settings()->set("AutomaticJavaSwitch", true);
            if (BuildConfig.JAVA_DOWNLOADER_ENABLED)
                APPLICATION->settings()->set("AutomaticJavaDownload", true);
            settings->set("OverrideJavaLocation", false);
            return tr("Done: the launcher picks a fitting Java from now on. Launch again to try it.");
        }
        case Action::SetMaxMemory: {
            const int maximum = fix.value.toInt();
            if (maximum <= 0)
                return {};
            const int minimum = std::min(settings->get("MinMemAlloc").toInt(), maximum);
            settings->set("OverrideMemory", true);
            settings->set("MinMemAlloc", minimum);
            settings->set("MaxMemAlloc", maximum);
            return tr("Done: the game may now use %1. Launch again to try it.").arg(CrashAnalyzer::formatMemory(maximum));
        }
        case Action::DisableMod: {
            auto* mods = instance->loaderModList();
            for (auto* resource : mods->allResources()) {
                if (resource->fileinfo().fileName() == fix.value) {
                    if (!resource->enable(EnableAction::DISABLE))
                        return tr("Could not disable %1.").arg(fix.value);
                    mods->update();
                    return tr("Done: %1 is disabled. Launch again to try without it.").arg(fix.value);
                }
            }
            // the mod list wasn't loaded: disable it the same way the Mods page does
            const QString path = FS::PathCombine(instance->modsRoot(), fix.value);
            if (!QFile::exists(path) || !QFile::rename(path, path + ".disabled"))
                return tr("Could not disable %1.").arg(fix.value);
            mods->update();
            return tr("Done: %1 is disabled. Launch again to try without it.").arg(fix.value);
        }
        case Action::OpenJavaSettings:
        case Action::OpenMemorySettings:
            APPLICATION->showInstanceWindow(instance, "settings");
            return {};
        case Action::OpenModsPage:
            APPLICATION->showInstanceWindow(instance, "mods");
            return {};
        case Action::OpenVersionPage:
            APPLICATION->showInstanceWindow(instance, "version");
            return {};
        case Action::OpenFile:
            DesktopServices::openPath(fix.value);
            return {};
        case Action::OpenUrl:
            DesktopServices::openUrl(QUrl(fix.value));
            return {};
    }
    return {};
}

void CrashHelpPanel::updateMutedColors()
{
    // setBackgroundRole() in the constructor already sends a PaletteChange, before the labels exist
    if (!m_caption)
        return;
    QColor muted = palette().color(QPalette::WindowText);
    muted.setAlphaF(0.7f);
    for (auto* label : m_mutedLabels) {
        QPalette pal = label->palette();
        pal.setColor(QPalette::WindowText, muted);
        label->setPalette(pal);
    }
    // the caption carries the warning colour
    QPalette captionPalette = m_caption->palette();
    captionPalette.setColor(QPalette::WindowText, palette().color(QPalette::BrightText));
    m_caption->setPalette(captionPalette);
}

void CrashHelpPanel::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        updateMutedColors();
    QFrame::changeEvent(event);
}
