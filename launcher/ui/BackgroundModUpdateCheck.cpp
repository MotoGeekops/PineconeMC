// SPDX-License-Identifier: GPL-3.0-only
#include "BackgroundModUpdateCheck.h"

#include <QDateTime>
#include <QDebug>
#include <QPointer>

#include "Application.h"
#include "InstanceList.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/mod/ModFolderModel.h"
#include "minecraft/mod/Resource.h"
#include "modplatform/flame/FlameCheckUpdate.h"
#include "modplatform/modrinth/ModrinthCheckUpdate.h"
#include "settings/Setting.h"
#include "settings/SettingsObject.h"
#include "tasks/SequentialTask.h"
#include "ui/DesktopNotifier.h"

namespace {
// CurseForge is asked once per mod: for big packs that would be hundreds of requests a day
constexpr int s_maxFlameMods = 40;
constexpr qint64 s_checkIntervalSecs = 20 * 60 * 60;
}  // namespace

void BackgroundModUpdateCheck::start(MinecraftInstance* instance)
{
    const auto settings = APPLICATION->settings();
    if (!instance || instance->isRunning() || !settings->get("NotifyOnModUpdates").toBool() ||
        settings->get("ModMetadataDisabled").toBool()) {
        return;
    }
    auto lastCheck = instance->settings()->getOrRegisterSetting("LastModUpdateCheck", QString());
    const auto last = QDateTime::fromString(lastCheck->get().toString(), Qt::ISODate);
    if (last.isValid() && last.secsTo(QDateTime::currentDateTime()) < s_checkIntervalSecs) {
        qDebug() << "Mod update check for" << instance->name() << "skipped, last one was" << last;
        return;
    }
    lastCheck->set(QDateTime::currentDateTime().toString(Qt::ISODate));
    qDebug() << "Checking for mod updates of" << instance->name() << "in the background";
    new BackgroundModUpdateCheck(instance);
}

void BackgroundModUpdateCheck::startForLastPlayed()
{
    BaseInstance* latest = nullptr;
    auto* instances = APPLICATION->instances();
    for (int i = 0; i < instances->count(); ++i) {
        auto* instance = instances->at(i);
        if (instance->lastLaunch() > 0 && (!latest || instance->lastLaunch() > latest->lastLaunch()))
            latest = instance;
    }
    start(dynamic_cast<MinecraftInstance*>(latest));
}

BackgroundModUpdateCheck::BackgroundModUpdateCheck(MinecraftInstance* instance) : QObject(instance), m_instance(instance)
{
    // the mod list knows which mods came from Modrinth or CurseForge once it has been loaded
    auto* mods = m_instance->loaderModList();
    connect(mods, &ResourceFolderModel::updateFinished, this, &BackgroundModUpdateCheck::modsLoaded, Qt::SingleShotConnection);
    mods->update();
}

void BackgroundModUpdateCheck::modsLoaded()
{
    auto* mods = m_instance->loaderModList();
    for (auto* resource : mods->allResources()) {
        if (!resource->enabled() || !resource->metadata())
            continue;
        if (resource->metadata()->provider == ModPlatform::ResourceProvider::MODRINTH)
            m_modrinth << resource;
        else if (resource->metadata()->provider == ModPlatform::ResourceProvider::FLAME)
            m_flame << resource;
    }
    if (m_flame.size() > s_maxFlameMods)
        m_flame.clear();

    auto* profile = m_instance->getPackProfile();
    const auto minecraft = profile ? profile->getComponent("net.minecraft") : nullptr;
    qDebug() << "Mod update check:" << m_modrinth.size() << "mods from Modrinth," << m_flame.size() << "from CurseForge";
    if ((m_modrinth.isEmpty() && m_flame.isEmpty()) || !minecraft) {
        deleteLater();
        return;
    }
    m_gameVersions = { minecraft->getVersion() };
    const auto loaders = profile->getModLoadersList();

    auto task = makeShared<SequentialTask>(tr("Checking for mod updates"));
    if (!m_modrinth.isEmpty()) {
        m_modrinthCheck.reset(new ModrinthCheckUpdate(m_modrinth, m_gameVersions, loaders, mods));
        task->addTask(m_modrinthCheck);
    }
    if (!m_flame.isEmpty()) {
        m_flameCheck.reset(new FlameCheckUpdate(m_flame, m_gameVersions, loaders, mods));
        task->addTask(m_flameCheck);
    }
    m_task = task;
    connect(task.get(), &Task::finished, this, &BackgroundModUpdateCheck::checked);
    task->start();
}

void BackgroundModUpdateCheck::checked()
{
    qsizetype updates = 0;
    if (m_modrinthCheck)
        updates += m_modrinthCheck->getUpdates().size();
    if (m_flameCheck)
        updates += m_flameCheck->getUpdates().size();
    qDebug() << "Mod update check of" << m_instance->name() << "found" << updates << "updates";

    if (updates > 0 && !m_instance->isRunning()) {
        QPointer<MinecraftInstance> instance(m_instance);
        DesktopNotifier::notify(updates == 1 ? tr("1 mod update for %1").arg(m_instance->name())
                                             : tr("%1 mod updates for %2").arg(updates).arg(m_instance->name()),
                                tr("Click to open the Mods page, then use Check for Updates."), [instance] {
                                    if (instance)
                                        APPLICATION->showInstanceWindow(instance, "mods");
                                });
    }
    deleteLater();
}
