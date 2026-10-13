// SPDX-License-Identifier: GPL-3.0-only
#include "InstanceSummary.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QLocale>
#include <QSet>

#include "BaseInstance.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"

namespace InstanceSummary {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("InstanceSummary", text);
}

/// The instance's components, read from disk the first time they are needed.
PackProfile* components(BaseInstance* instance)
{
    auto* minecraft = dynamic_cast<MinecraftInstance*>(instance);
    if (!minecraft)
        return nullptr;
    auto* profile = minecraft->getPackProfile();
    // components load lazily; read them once so there is something to show before the instance is opened
    static QSet<const PackProfile*> attempted;
    if (profile && profile->rowCount() == 0 && !attempted.contains(profile)) {
        attempted.insert(profile);
        profile->reload(Net::Mode::Offline);
    }
    return profile;
}

QString loaderName(ModPlatform::ModLoaderType type)
{
    switch (type) {
        case ModPlatform::NeoForge:
            return "NeoForge";
        case ModPlatform::Forge:
            return "Forge";
        case ModPlatform::Cauldron:
            return "Cauldron";
        case ModPlatform::LiteLoader:
            return "LiteLoader";
        case ModPlatform::Fabric:
            return "Fabric";
        case ModPlatform::Quilt:
            return "Quilt";
        case ModPlatform::Babric:
            return "Babric";
        case ModPlatform::BTA:
            return "BTA";
        case ModPlatform::LegacyFabric:
            return "Legacy Fabric";
        case ModPlatform::Ornithe:
            return "Ornithe";
        case ModPlatform::Rift:
            return "Rift";
        case ModPlatform::DataPack:
            break;
    }
    return {};
}

}  // namespace

QString separator()
{
    // escaped: the sources are not compiled as UTF-8 on MSVC
    return QStringLiteral(" \u00B7 ");
}

QString minecraftVersion(BaseInstance* instance)
{
    auto* profile = components(instance);
    return profile ? profile->getComponentVersion("net.minecraft") : QString();
}

QString modLoader(BaseInstance* instance)
{
    auto* profile = components(instance);
    if (!profile)
        return {};
    // the first entry is the real loader; compatible ones (Fabric for Quilt) are appended after it
    const auto loaders = profile->getModLoadersList();
    return loaders.isEmpty() ? QString() : loaderName(loaders.first());
}

QString versionLine(BaseInstance* instance)
{
    const auto version = minecraftVersion(instance);
    const auto loader = modLoader(instance);
    if (version.isEmpty())
        return loader;
    if (loader.isEmpty())
        return version;
    return version + separator() + loader;
}

int modCount(BaseInstance* instance)
{
    auto* minecraft = dynamic_cast<MinecraftInstance*>(instance);
    if (!minecraft)
        return 0;
    const QDir mods(minecraft->modsRoot());
    if (!mods.exists())
        return 0;
    static const QStringList patterns{ "*.jar", "*.zip", "*.litemod", "*.jar.disabled", "*.zip.disabled", "*.litemod.disabled" };
    return static_cast<int>(mods.entryList(patterns, QDir::Files).size());
}

QString searchText(BaseInstance* instance, const QString& group)
{
    return QStringList{ instance->name(), group, minecraftVersion(instance), modLoader(instance) }.join(' ').toLower();
}

QString relativeTime(qint64 msecsSinceEpoch)
{
    if (msecsSinceEpoch <= 0)
        return {};
    const auto then = QDateTime::fromMSecsSinceEpoch(msecsSinceEpoch);
    const auto now = QDateTime::currentDateTime();
    const qint64 seconds = then.secsTo(now);
    if (seconds < 60)
        return tr("just now");
    if (seconds < 3600) {
        const auto minutes = seconds / 60;
        return minutes == 1 ? tr("1 minute ago") : tr("%1 minutes ago").arg(minutes);
    }
    if (seconds < 86400) {
        const auto hours = seconds / 3600;
        return hours == 1 ? tr("1 hour ago") : tr("%1 hours ago").arg(hours);
    }
    const qint64 days = then.date().daysTo(now.date());
    if (days <= 1)
        return tr("yesterday");
    if (days < 7)
        return tr("%1 days ago").arg(days);
    if (days < 30) {
        const auto weeks = days / 7;
        return weeks == 1 ? tr("1 week ago") : tr("%1 weeks ago").arg(weeks);
    }
    return QLocale().toString(then.date(), QLocale::ShortFormat);
}

}  // namespace InstanceSummary
