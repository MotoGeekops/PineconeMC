// SPDX-License-Identifier: GPL-3.0-only
#include "ContinueCard.h"

#include <QDir>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <optional>

#include "Application.h"
#include "BaseInstance.h"
#include "InstanceList.h"
#include "LaunchMode.h"
#include "icons/IconList.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/World.h"
#include "minecraft/launch/MinecraftTarget.h"
#include "ui/instanceview/InstanceSummary.h"

namespace {

/// The most recently played world of an instance, if quick play can open it directly.
std::optional<World> lastPlayedWorld(BaseInstance* instance)
{
    auto* minecraft = dynamic_cast<MinecraftInstance*>(instance);
    if (!minecraft || !minecraft->traits().contains("feature:is_quick_play_singleplayer"))
        return std::nullopt;

    std::optional<World> latest;
    const QDir saves(minecraft->worldDir());
    for (const auto& entry : saves.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        World world(entry);
        if (world.isValid() && (!latest || world.lastPlayed() > latest->lastPlayed()))
            latest = world;
    }
    return latest;
}

}  // namespace

ContinueCard::ContinueCard(QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("continueCard"));
    // a plain raised panel for themes without a style sheet rule for it
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::AlternateBase);

    m_icon = new QLabel(this);
    m_icon->setObjectName(QStringLiteral("continueIcon"));
    m_icon->setFixedSize(48, 48);

    m_caption = new QLabel(tr("Continue playing"), this);
    m_caption->setObjectName(QStringLiteral("continueCaption"));
    QFont captionFont = m_caption->font();
    captionFont.setCapitalization(QFont::AllUppercase);
    captionFont.setLetterSpacing(QFont::PercentageSpacing, 108);
    captionFont.setPointSizeF(captionFont.pointSizeF() * 0.85);
    captionFont.setBold(true);
    m_caption->setFont(captionFont);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("continueTitle"));
    QFont titleFont = m_title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.3);
    titleFont.setBold(true);
    m_title->setFont(titleFont);

    m_details = new QLabel(this);
    m_details->setObjectName(QStringLiteral("continueDetails"));

    m_worldButton = new QPushButton(this);
    m_worldButton->setObjectName(QStringLiteral("continueWorldButton"));
    m_worldButton->setIcon(QIcon::fromTheme("worlds"));
    m_worldButton->setCursor(Qt::PointingHandCursor);
    connect(m_worldButton, &QPushButton::clicked, this, [this] { launch(true); });

    m_playButton = new QPushButton(tr("Play"), this);
    m_playButton->setObjectName(QStringLiteral("continuePlayButton"));
    m_playButton->setIcon(QIcon::fromTheme("launch"));
    m_playButton->setCursor(Qt::PointingHandCursor);
    connect(m_playButton, &QPushButton::clicked, this, [this] { launch(false); });

    m_closeButton = new QToolButton(this);
    m_closeButton->setObjectName(QStringLiteral("continueCloseButton"));
    m_closeButton->setText(QString(QChar(0x2715)));
    m_closeButton->setAutoRaise(true);
    m_closeButton->setToolTip(tr("Hide this card. Right-click the instance panel to bring it back."));
    connect(m_closeButton, &QToolButton::clicked, this, &ContinueCard::hideRequested);

    auto* text = new QVBoxLayout;
    text->setSpacing(1);
    text->addWidget(m_caption);
    text->addWidget(m_title);
    text->addWidget(m_details);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 10, 8, 10);
    layout->setSpacing(12);
    layout->addWidget(m_icon);
    layout->addLayout(text, 1);
    layout->addWidget(m_worldButton);
    layout->addWidget(m_playButton);
    layout->addWidget(m_closeButton, 0, Qt::AlignTop);

    updateMutedColors();

    // follow launches, renames, new and deleted instances
    auto* instances = APPLICATION->instances();
    connect(instances, &QAbstractItemModel::dataChanged, this, &ContinueCard::scheduleRefresh);
    connect(instances, &QAbstractItemModel::rowsInserted, this, &ContinueCard::scheduleRefresh);
    connect(instances, &QAbstractItemModel::rowsRemoved, this, &ContinueCard::scheduleRefresh);
    connect(instances, &QAbstractItemModel::modelReset, this, &ContinueCard::scheduleRefresh);

    // keeps "last played 5 minutes ago" current
    m_clock = new QTimer(this);
    m_clock->setInterval(60 * 1000);
    connect(m_clock, &QTimer::timeout, this, &ContinueCard::updateContents);
    m_clock->start();

    refresh();
}

void ContinueCard::setAllowed(bool allowed)
{
    m_allowed = allowed;
    setVisible(m_allowed && m_instance);
}

void ContinueCard::scheduleRefresh()
{
    if (m_refreshQueued)
        return;
    m_refreshQueued = true;
    QTimer::singleShot(0, this, &ContinueCard::refresh);
}

void ContinueCard::refresh()
{
    m_refreshQueued = false;
    BaseInstance* latest = nullptr;
    auto* instances = APPLICATION->instances();
    for (int i = 0; i < instances->count(); ++i) {
        auto* instance = instances->at(i);
        if (instance->lastLaunch() > 0 && (!latest || instance->lastLaunch() > latest->lastLaunch()))
            latest = instance;
    }
    setInstance(latest);
}

void ContinueCard::setInstance(BaseInstance* instance)
{
    if (m_instance != instance) {
        if (m_instance)
            disconnect(m_instance, nullptr, this, nullptr);
        m_instance = instance;
        m_scannedLaunch = -1;
        if (m_instance)
            connect(m_instance, &BaseInstance::runningStatusChanged, this, &ContinueCard::updateContents);
    }
    updateContents();
}

void ContinueCard::updateContents()
{
    setVisible(m_allowed && m_instance);
    if (!m_instance)
        return;

    m_icon->setPixmap(APPLICATION->icons()->getIcon(m_instance->iconKey()).pixmap(QSize(48, 48), devicePixelRatioF()));
    m_title->setText(m_instance->name());

    QStringList details;
    const auto version = InstanceSummary::versionLine(m_instance);
    if (!version.isEmpty())
        details << version;
    details << tr("last played %1").arg(InstanceSummary::relativeTime(m_instance->lastLaunch()));
    m_details->setText(details.join(InstanceSummary::separator()));

    // reading the worlds touches the disk, so only redo it after the instance was played again
    if (m_scannedLaunch != m_instance->lastLaunch()) {
        m_scannedLaunch = m_instance->lastLaunch();
        m_worldFolder.clear();
        if (auto world = lastPlayedWorld(m_instance)) {
            m_worldFolder = world->folderName();
            const QString name = world->name();
            m_worldButton->setText(m_worldButton->fontMetrics().elidedText(name, Qt::ElideRight, 180));
            m_worldButton->setToolTip(tr("Launch and load \"%1\" right away").arg(name));
        }
    }

    const bool running = m_instance->isRunning();
    m_playButton->setText(running ? tr("Running") : tr("Play"));
    m_playButton->setEnabled(!running && m_instance->canLaunch());
    m_worldButton->setVisible(!m_worldFolder.isEmpty());
    m_worldButton->setEnabled(!running && m_instance->canLaunch());
}

void ContinueCard::launch(bool intoWorld)
{
    if (!m_instance)
        return;
    if (intoWorld && !m_worldFolder.isEmpty()) {
        auto target = std::make_shared<MinecraftTarget>();
        target->world = m_worldFolder;
        APPLICATION->launch(m_instance, LaunchMode::Normal, target);
    } else {
        APPLICATION->launch(m_instance);
    }
}

void ContinueCard::updateMutedColors()
{
    // setBackgroundRole() in the constructor already sends a PaletteChange, before the labels exist
    if (!m_caption || !m_details)
        return;
    // secondary text: the card's text colour, faded
    for (auto* label : { m_caption, m_details }) {
        QPalette pal = label->palette();
        QColor muted = palette().color(QPalette::WindowText);
        muted.setAlphaF(0.6f);
        pal.setColor(QPalette::WindowText, muted);
        label->setPalette(pal);
    }
}

void ContinueCard::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        updateMutedColors();
    QFrame::changeEvent(event);
}
