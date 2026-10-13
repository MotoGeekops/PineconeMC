// SPDX-License-Identifier: GPL-3.0-only
#include "CrashAnalyzer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>

#include <algorithm>
#include <optional>

#include "FileSystem.h"
#include "HardwareInfo.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/mod/Mod.h"
#include "minecraft/mod/ModFolderModel.h"
#include "minecraft/mod/tasks/LocalModParseTask.h"

namespace CrashAnalyzer {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("CrashAnalyzer", text);
}

/// The newest file matching @p patterns in @p dir that was written at or after @p since (at most 2 MB of it).
QString readNewestSince(const QString& dir, const QStringList& patterns, const QDateTime& since, QString& path)
{
    QFileInfo newest;
    for (const auto& file : QDir(dir).entryInfoList(patterns, QDir::Files)) {
        if (file.lastModified() >= since && (!newest.exists() || file.lastModified() > newest.lastModified()))
            newest = file;
    }
    if (!newest.exists())
        return {};
    QFile file(newest.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    path = newest.absoluteFilePath();
    return QString::fromUtf8(file.read(2 * 1024 * 1024));
}

/// The file name of a path, whichever kind of slashes it uses (logs from Windows use backslashes)
QString fileNameOf(const QString& path)
{
    static const QRegularExpression separator(R"re([/\\])re");
    return path.mid(path.lastIndexOf(separator) + 1);
}

/// Readable names for the libraries mods most often miss.
QString knownModName(const QString& id)
{
    static const QHash<QString, QString> names{
        { "fabric-api", "Fabric API" },
        { "fabric", "Fabric API" },
        { "fabric-language-kotlin", "Fabric Language Kotlin" },
        { "cloth-config", "Cloth Config API" },
        { "cloth_config", "Cloth Config API" },
        { "cloth-config2", "Cloth Config API" },
        { "architectury", "Architectury API" },
        { "geckolib", "GeckoLib" },
        { "geckolib3", "GeckoLib" },
        { "forgeconfigapiport", "Forge Config API Port" },
        { "kotlinforforge", "Kotlin for Forge" },
        { "yet_another_config_lib_v3", "YetAnotherConfigLib" },
        { "yet-another-config-lib", "YetAnotherConfigLib" },
        { "modmenu", "Mod Menu" },
        { "sodium", "Sodium" },
        { "iris", "Iris" },
        { "balm", "Balm" },
        { "curios", "Curios API" },
        { "trinkets", "Trinkets" },
        { "puzzleslib", "Puzzles Lib" },
        { "bookshelf", "Bookshelf" },
        { "owo", "owo-lib" },
        { "resourcefullib", "Resourceful Lib" },
        { "playeranimator", "playerAnimator" },
        { "creativecore", "CreativeCore" },
        { "collective", "Collective" },
    };
    return names.value(id);
}

/// "version 0.5.0 or later" -> "0.5.0 or later", "any version" -> "", Maven ranges like "[4.2,)" -> "4.2 or later"
QString readableRange(QString range)
{
    range = range.trimmed();
    if (range == QLatin1String("any version") || range == QLatin1String("*") || range.isEmpty())
        return {};
    if (range.startsWith(QLatin1String("version ")))
        return range.mid(8);

    static const QRegularExpression maven(R"re(^([\[(])\s*([^,\])]*)\s*(?:,\s*([^\])]*))?\s*([\])])$)re");
    const auto match = maven.match(range);
    if (!match.hasMatch())
        return range;
    const QString low = match.captured(2).trimmed();
    const QString high = match.captured(3).trimmed();
    const bool hasComma = range.contains(',');
    if (!hasComma)
        return low;
    if (!low.isEmpty() && high.isEmpty())
        return tr("%1 or later").arg(low);
    if (low.isEmpty() && !high.isEmpty())
        return tr("up to %1").arg(high);
    return tr("%1 to %2").arg(low, high);
}

struct Context {
    const Input& in;
    QString text;  // the log, the crash report and the JVM crash log together
    QList<Diagnosis> found;
    QHash<QString, QString> namesInLog;  // mod names the loader printed next to their ids

    bool has(const QString& id) const
    {
        return std::any_of(found.begin(), found.end(), [&id](const Diagnosis& d) { return d.id == id; });
    }

    void add(Diagnosis diagnosis)
    {
        for (const auto& existing : found) {
            if (existing.id == diagnosis.id && existing.title == diagnosis.title)
                return;
        }
        found << std::move(diagnosis);
    }

    QString modName(const QString& id, const QString& fallback = {}) const
    {
        if (auto it = in.mods.constFind(id); it != in.mods.constEnd() && !it->name.isEmpty())
            return it->name;
        if (!fallback.isEmpty())
            return fallback;
        if (const QString logged = namesInLog.value(id); !logged.isEmpty())
            return logged;
        const QString known = knownModName(id);
        return known.isEmpty() ? id : known;
    }

    void noteName(const QString& id, const QString& name)
    {
        if (!id.isEmpty() && !name.isEmpty())
            namesInLog.insert(id, name);
    }

    /// "Disable <mod>", when the mod is one of the installed ones
    void addDisableFix(QList<Fix>& fixes, const QString& id) const
    {
        if (auto it = in.mods.constFind(id); it != in.mods.constEnd() && !it->file.isEmpty())
            fixes << Fix{ Fix::Action::DisableMod, tr("Disable %1").arg(modName(id)), it->file };
    }

    QList<Fix> javaFixes(bool autoHelps) const
    {
        QList<Fix> fixes;
        if (autoHelps)
            fixes << Fix{ Fix::Action::UseRecommendedJava, tr("Pick Java Automatically"), {} };
        fixes << Fix{ Fix::Action::OpenJavaSettings, tr("Java Settings"), {} };
        return fixes;
    }

    /// Letting the launcher choose fixes it when the Java that failed isn't one this Minecraft version works with.
    bool autoJavaHelps() const
    {
        return in.javaMajor > 0 && !in.compatibleJavaMajors.isEmpty() && !in.compatibleJavaMajors.contains(in.javaMajor);
    }
};

void javaTooOld(Context& c, int needed, int current)
{
    if (c.has("java-too-old") || needed <= 0)
        return;
    const QString explanation =
        current > 0 ? tr("The game or one of its mods needs Java %1 or newer, but it was started with Java %2.").arg(needed).arg(current)
                    : tr("The game or one of its mods needs Java %1 or newer.").arg(needed);
    c.add({ "java-too-old", tr("This needs a newer Java"), explanation, c.javaFixes(c.autoJavaHelps()) });
}

void checkJava(Context& c)
{
    static const QRegularExpression missingJava(R"re(The Java binary "[^"]*" couldn't be found)re");
    if (missingJava.match(c.text).hasMatch()) {
        c.add({ "java-missing", tr("Java was not found"),
                tr("The Java this instance is set to use doesn't exist any more. Let the launcher pick one, or choose another in the Java "
                   "settings."),
                c.javaFixes(true) });
    }

    static const QRegularExpression brokenJava(R"re(Could not start java:)re");
    if (brokenJava.match(c.text).hasMatch()) {
        c.add({ "java-broken", tr("Java didn't start"),
                tr("The Java this instance is set to use fails to start. Let the launcher pick another one, or choose another in the "
                   "Java settings."),
                c.javaFixes(true) });
    }

    // "...compiled by a more recent version of the Java Runtime (class file version 65.0), this version of the Java Runtime only
    // recognizes class file versions up to 61.0"
    static const QRegularExpression classVersion(
        R"re(class file version (\d+)\.\d+\), this version of the Java Runtime only recognizes class file versions up to (\d+))re");
    if (const auto match = classVersion.match(c.text); match.hasMatch())
        javaTooOld(c, match.captured(1).toInt() - 44, match.captured(2).toInt() - 44);

    // Fabric: "requires version 21 or later of 'OpenJDK 64-Bit Server VM' (java), but only the wrong version is present: 17!"
    static const QRegularExpression fabricJava(R"re(requires (?:any )?version (\d+)[^,\r\n]* of '[^']*' \(java\), but only the wrong versions? (?:is|are) present: (\d+))re");
    if (const auto match = fabricJava.match(c.text); match.hasMatch())
        javaTooOld(c, match.captured(1).toInt(), match.captured(2).toInt());

    static const QRegularExpression tooNew(
        R"re(ClassLoaders\$AppClassLoader cannot be cast to (?:class )?java\.net\.URLClassLoader|Unsupported class file major version \d+|module java\.base does not "opens java\.)re");
    if (tooNew.match(c.text).hasMatch()) {
        const QString explanation =
            c.in.javaMajor > 0
                ? tr("This Minecraft version or its mod loader doesn't work with Java %1. Older versions need older Java: Java 8 for "
                     "Minecraft 1.16 and older, Java 17 for 1.17 to 1.20.4.")
                      .arg(c.in.javaMajor)
                : tr("This Minecraft version or its mod loader needs an older Java: Java 8 for Minecraft 1.16 and older, Java 17 for 1.17 "
                     "to 1.20.4.");
        c.add({ "java-too-new", tr("This needs an older Java"), explanation, c.javaFixes(c.autoJavaHelps() || c.in.javaMajor > 17) });
    }
}

int suggestedMemory(const Input& in)
{
    // half again as much, at least 4 GB, in whole GB, and leave the rest of the computer some room
    int target = std::max(4096, (in.maxMemoryMiB * 3 / 2 + 1023) / 1024 * 1024);
    if (in.physicalMemoryMiB > 0)
        target = std::min(target, (in.physicalMemoryMiB - 3072) / 1024 * 1024);
    return target;
}

void checkMemory(Context& c)
{
    const auto& in = c.in;

    static const QRegularExpression outOfMemory(R"re(java\.lang\.OutOfMemoryError)re");
    if (outOfMemory.match(c.text).hasMatch()) {
        Diagnosis diagnosis{ "out-of-memory", tr("Minecraft ran out of memory"), {}, {} };
        const int suggested = suggestedMemory(in);
        if (suggested > in.maxMemoryMiB) {
            diagnosis.explanation = tr("It may use up to %1, and that wasn't enough. Give it more: modded instances often need 6 to 8 GB.")
                                        .arg(formatMemory(in.maxMemoryMiB));
            diagnosis.fixes << Fix{ Fix::Action::SetMaxMemory, tr("Use %1").arg(formatMemory(suggested)), QString::number(suggested) };
        } else {
            diagnosis.explanation = tr("It may use up to %1, which is already a lot for this computer (%2 in total). Close other "
                                       "programs, or remove some mods.")
                                        .arg(formatMemory(in.maxMemoryMiB), formatMemory(in.physicalMemoryMiB));
        }
        diagnosis.fixes << Fix{ Fix::Action::OpenMemorySettings, tr("Memory Settings"), {} };
        c.add(diagnosis);
    }

    static const QRegularExpression reserve(R"re(Could not reserve enough space for (?:\d+KB )?object heap)re");
    if (reserve.match(c.text).hasMatch()) {
        Diagnosis diagnosis{ "memory-unavailable", tr("Java couldn't get the memory it was set to use"),
                             tr("This instance asks for %1, more than Java could get. Lower the maximum memory, or use a 64-bit Java.")
                                 .arg(formatMemory(in.maxMemoryMiB)),
                             {} };
        int lower = in.maxMemoryMiB / 2;
        if (in.physicalMemoryMiB > 0)
            lower = std::min(lower, in.physicalMemoryMiB / 2);
        lower = lower / 512 * 512;
        if (lower >= 1024 && lower < in.maxMemoryMiB)
            diagnosis.fixes << Fix{ Fix::Action::SetMaxMemory, tr("Use %1").arg(formatMemory(lower)), QString::number(lower) };
        diagnosis.fixes << Fix{ Fix::Action::OpenMemorySettings, tr("Memory Settings"), {} };
        c.add(diagnosis);
    }

    static const QRegularExpression badSettings(
        R"re(Initial heap size set to a larger value than the maximum heap size|Invalid (?:maximum|initial) heap size)re");
    if (badSettings.match(c.text).hasMatch()) {
        c.add({ "memory-settings", tr("The memory settings don't fit together"),
                in.minMemoryMiB > in.maxMemoryMiB ? tr("The minimum memory (%1) is set higher than the maximum (%2).")
                                                        .arg(formatMemory(in.minMemoryMiB), formatMemory(in.maxMemoryMiB))
                                                  : tr("The memory settings of this instance aren't valid."),
                { Fix{ Fix::Action::OpenMemorySettings, tr("Memory Settings"), {} } } });
    }

    static const QRegularExpression systemMemory(
        R"re(There is insufficient memory for the Java Runtime Environment to continue|Native memory allocation \(\w+\) failed)re");
    if (systemMemory.match(c.text).hasMatch()) {
        c.add({ "system-out-of-memory", tr("The computer ran out of memory"),
                tr("There was no memory left for the game. Close other programs, or lower the maximum memory of this instance (now %1).")
                    .arg(formatMemory(in.maxMemoryMiB)),
                { Fix{ Fix::Action::OpenMemorySettings, tr("Memory Settings"), {} } } });
    }
}

struct MissingMod {
    QString name;
    QString range;
    QStringList requiredBy;  // mod ids
};

void addMissingMods(Context& c, const QMap<QString, MissingMod>& missing)
{
    int shown = 0;
    for (auto it = missing.constBegin(); it != missing.constEnd() && shown < 3; ++it, ++shown) {
        const auto& mod = it.value();
        QStringList requiredBy;
        for (const auto& id : mod.requiredBy)
            requiredBy << c.modName(id);
        const QString range = readableRange(mod.range);
        const QString wanted = range.isEmpty() ? mod.name : tr("%1 (%2)").arg(mod.name, range);

        Diagnosis diagnosis{ "missing-mod", tr("%1 is missing").arg(mod.name),
                             tr("%1 needs %2, which isn't installed. Get it on the Mods page with \"Download mods\".")
                                 .arg(QLocale().createSeparatedList(requiredBy), wanted),
                             { Fix{ Fix::Action::OpenModsPage, tr("Open Mods"), {} } } };
        if (mod.requiredBy.size() == 1)
            c.addDisableFix(diagnosis.fixes, mod.requiredBy.first());
        c.add(diagnosis);
    }
}

void wrongMinecraftVersion(Context& c, const QString& modId, const QString& range, const QString& present)
{
    Diagnosis diagnosis{ "wrong-minecraft-version", tr("%1 is for another Minecraft version").arg(c.modName(modId)),
                         tr("%1 needs Minecraft %2, but this instance runs %3. Get the version of the mod made for %3, or remove it.")
                             .arg(c.modName(modId), readableRange(range), present),
                         {} };
    c.addDisableFix(diagnosis.fixes, modId);
    diagnosis.fixes << Fix{ Fix::Action::OpenModsPage, tr("Open Mods"), {} };
    c.add(diagnosis);
}

void wrongLoaderVersion(Context& c, const QString& modId, const QString& loader, const QString& range, const QString& present)
{
    c.add({ "wrong-loader-version", tr("%1 needs a different %2 version").arg(c.modName(modId), loader),
            tr("%1 needs %2 %3, but %4 is installed. Change the loader version on the Version page.")
                .arg(c.modName(modId), loader, readableRange(range), present),
            { Fix{ Fix::Action::OpenVersionPage, tr("Open Version Page"), {} } } });
}

void wrongModVersion(Context& c, const QString& modId, const QString& depName, const QString& range, const QString& present)
{
    c.add({ "wrong-mod-version", tr("%1 needs a different %2").arg(c.modName(modId), depName),
            tr("%1 needs %2 %3, but %4 is installed. Update %2 on the Mods page.").arg(c.modName(modId), depName, readableRange(range), present),
            { Fix{ Fix::Action::OpenModsPage, tr("Open Mods"), {} } } });
}

void checkFabricDependencies(Context& c)
{
    // "Mod 'Iris' (iris) 1.6.11 requires version 0.5.0 or later of mod 'Sodium' (sodium), which is missing!"
    // "Mod 'Sodium Extra' (sodium-extra) 0.5.4 requires any version of sodium, which is missing!"
    static const QRegularExpression missing(
        R"re(Mod '([^']*)' \(([^)]+)\) \S+ requires (.+?) of (?:(?:mod )?'([^']*)' \(([^)]+)\)|([^\s,]+)), which is missing!)re");
    QMap<QString, MissingMod> missingMods;
    for (auto it = missing.globalMatch(c.text); it.hasNext();) {
        const auto match = it.next();
        c.noteName(match.captured(2), match.captured(1));
        const QString depId = match.captured(5).isEmpty() ? match.captured(6) : match.captured(5);
        if (depId == "java" || depId == "minecraft" || depId == "fabricloader")
            continue;
        auto& entry = missingMods[depId];
        entry.name = match.captured(4).isEmpty() ? c.modName(depId) : match.captured(4);
        entry.range = match.captured(3);
        if (!entry.requiredBy.contains(match.captured(2)))
            entry.requiredBy << match.captured(2);
    }
    addMissingMods(c, missingMods);

    // "Mod 'Sodium' (sodium) 0.5.8 requires version 1.20.4 of 'Minecraft' (minecraft), but only the wrong version is present: 1.20.1!"
    static const QRegularExpression wrong(
        R"re(Mod '([^']*)' \(([^)]+)\) \S+ requires (.+?) of (?:(?:mod )?'([^']*)' \(([^)]+)\)|([^\s,]+)), but only the wrong versions? (?:is|are) present: ([^!\r\n]+)!)re");
    for (auto it = wrong.globalMatch(c.text); it.hasNext();) {
        const auto match = it.next();
        const QString modId = match.captured(2);
        c.noteName(modId, match.captured(1));
        const QString depId = match.captured(5).isEmpty() ? match.captured(6) : match.captured(5);
        const QString depName = match.captured(4).isEmpty() ? c.modName(depId) : match.captured(4);
        const QString range = match.captured(3);
        const QString present = match.captured(7);
        if (depId == "java")
            continue;  // checkJava() explains that one
        if (depId == "minecraft")
            wrongMinecraftVersion(c, modId, range, present);
        else if (depId == "fabricloader" || depId == "quilt_loader")
            wrongLoaderVersion(c, modId, depName, range, present);
        else
            wrongModVersion(c, modId, depName, range, present);
    }

    // "Mod 'OptiFabric' (optifabric) 1.13.0 is incompatible with any version of mod 'Sodium' (sodium), yet a conflicting version is present"
    static const QRegularExpression breaks(
        R"re(Mod '([^']*)' \(([^)]+)\) \S+ is incompatible with .+? of (?:(?:mod )?'([^']*)' \(([^)]+)\)|([^\s,]+)), yet)re");
    for (auto it = breaks.globalMatch(c.text); it.hasNext();) {
        const auto match = it.next();
        const QString modId = match.captured(2);
        c.noteName(modId, match.captured(1));
        const QString otherId = match.captured(4).isEmpty() ? match.captured(5) : match.captured(4);
        const QString otherName = match.captured(3).isEmpty() ? c.modName(otherId) : match.captured(3);
        Diagnosis diagnosis{ "incompatible-mods", tr("%1 and %2 don't work together").arg(c.modName(modId, match.captured(1)), otherName),
                             tr("The makers of %1 say it doesn't work with %2. Remove one of them, or look for updates that fix this.")
                                 .arg(c.modName(modId, match.captured(1)), otherName),
                             {} };
        c.addDisableFix(diagnosis.fixes, modId);
        c.addDisableFix(diagnosis.fixes, otherId);
        c.add(diagnosis);
    }

    // the solution Fabric worked out: "Remove mod 'Sodium' (sodium) 0.5.3 (C:\...\mods\sodium-0.5.3.jar)."
    static const QRegularExpression remove(R"re(Remove (?:mod )?'([^']*)' \(([^)]+)\) \S+ \(([^\r\n]+)\)\.)re");
    for (auto it = remove.globalMatch(c.text); it.hasNext();) {
        const auto match = it.next();
        const QString file = fileNameOf(match.captured(3));
        c.add({ "fabric-suggests-removal", tr("Fabric suggests removing %1").arg(match.captured(1)),
                tr("Fabric Loader worked out that the mods load once %1 (%2) is gone.").arg(match.captured(1), file),
                { Fix{ Fix::Action::DisableMod, tr("Disable %1").arg(match.captured(1)), file } } });
    }
}

void checkForgeDependencies(Context& c)
{
    // Forge and NeoForge: "Mod ID: 'geckolib', Requested by: 'alexsmobs', Expected range: '[4.2,)', Actual version: '[MISSING]'"
    static const QRegularExpression dependency(R"re(Mod ID: '([^']+)', Requested by: '([^']+)', Expected range: '([^']*)', Actual version: '([^']*)')re");
    QMap<QString, MissingMod> missingMods;
    for (auto it = dependency.globalMatch(c.text); it.hasNext();) {
        const auto match = it.next();
        const QString depId = match.captured(1);
        const QString modId = match.captured(2);
        const QString range = match.captured(3);
        const QString present = match.captured(4);
        if (present == "[MISSING]") {
            auto& entry = missingMods[depId];
            entry.name = c.modName(depId);
            entry.range = range;
            if (!entry.requiredBy.contains(modId))
                entry.requiredBy << modId;
        } else if (depId == "minecraft") {
            wrongMinecraftVersion(c, modId, range, present);
        } else if (depId == "forge" || depId == "neoforge") {
            wrongLoaderVersion(c, modId, depId == "forge" ? "Forge" : "NeoForge", range, present);
        } else {
            wrongModVersion(c, modId, c.modName(depId), range, present);
        }
    }
    addMissingMods(c, missingMods);

    // Forge: "Mod ID: 'jei' from mod files: jei-1.jar, jei-2.jar", NeoForge: "Mod jei is present in multiple files: a.jar, b.jar"
    static const QRegularExpression duplicates(
        R"re(Mod ID: '([^']+)' from mod files: ([^\r\n]+)|Mod (?:\x{00A7}e)?([\w.-]+)(?:\x{00A7}r)? is present in multiple files: ([^\r\n]+))re");
    for (auto it = duplicates.globalMatch(c.text); it.hasNext();) {
        const auto match = it.next();
        const QString id = match.captured(1).isEmpty() ? match.captured(3) : match.captured(1);
        const QString files = match.captured(2).isEmpty() ? match.captured(4) : match.captured(2);
        c.add({ "duplicate-mod", tr("%1 is installed twice").arg(c.modName(id)),
                tr("These files contain the same mod: %1. Keep the newest one and remove the others.").arg(files.trimmed()),
                { Fix{ Fix::Action::OpenModsPage, tr("Open Mods"), {} } } });
    }
}

void checkBrokenModFiles(Context& c)
{
    // Fabric: "Error analyzing [C:\...\mods\foo.jar]: java.util.zip.ZipException: zip END header not found"
    static const QRegularExpression analyzing(R"re(Error analyzing \[([^\]\r\n]+?\.jar)\]: java\.util\.zip\.)re");
    static const QRegularExpression zipError(
        R"re(([^\s\\/\[\]'"]+\.jar)[^\r\n]*?(?:ZipException|zip END header not found|invalid CEN header|zip file is empty))re");
    QStringList files;
    for (const auto* pattern : { &analyzing, &zipError }) {
        for (auto it = pattern->globalMatch(c.text); it.hasNext();) {
            const QString file = fileNameOf(it.next().captured(1));
            if (!files.contains(file))
                files << file;
        }
    }
    for (const auto& file : files) {
        c.add({ "broken-mod-file", tr("%1 is damaged").arg(file), tr("%1 can't be read. Download it again, or remove it.").arg(file),
                { Fix{ Fix::Action::DisableMod, tr("Disable It"), file }, Fix{ Fix::Action::OpenModsPage, tr("Open Mods"), {} } } });
    }
}

/// The mods an error points at, most likely first. Only installed mods count.
QStringList suspectedMods(const Context& c)
{
    QStringList suspects;
    auto suspect = [&](const QString& id) {
        if (c.in.mods.contains(id) && !suspects.contains(id))
            suspects << id;
    };

    // named outright by the loader or Mixin
    static const QList<QRegularExpression> named{
        QRegularExpression(R"re(Could not execute entrypoint stage '\w+' due to errors, provided by '([^']+)')re"),
        QRegularExpression(R"re(Mixin apply for mod ([\w.-]+) failed)re"),
        QRegularExpression(R"re(Mixin \[[^\]]*\] from mod ([\w.-]+))re"),
        QRegularExpression(R"re(\(([\w.-]+)\) has failed to load correctly)re"),
    };
    for (const auto& pattern : named) {
        for (auto it = pattern.globalMatch(c.text); it.hasNext();)
            suspect(it.next().captured(1));
    }

    // Forge crash reports list them: "Suspected Mods: \n\tCreate (create), Version: 0.5.1"
    const int listed = c.in.crashReport.indexOf("Suspected Mod");
    if (listed >= 0) {
        static const QRegularExpression entry(R"re(\(([\w.-]+)\), Version:)re");
        for (auto it = entry.globalMatch(c.in.crashReport.mid(listed, 2000)); it.hasNext();)
            suspect(it.next().captured(1));
    }

    // methods that mods add with Mixin carry the mod id: "handler$zbk000$sodium$onRender"
    const QString& trace = c.in.crashReport.isEmpty() ? c.in.log : c.in.crashReport;
    static const QRegularExpression mixinMethod(R"re(\$[a-z]{3}\d{3}\$([a-z][a-z0-9_-]*)\$)re");
    QMap<QString, int> counts;
    for (auto it = mixinMethod.globalMatch(trace); it.hasNext();) {
        const QString id = it.next().captured(1);
        if (c.in.mods.contains(id))
            counts[id]++;
    }
    QStringList byCount = counts.keys();
    std::stable_sort(byCount.begin(), byCount.end(), [&counts](const QString& a, const QString& b) { return counts[a] > counts[b]; });
    for (const auto& id : byCount)
        suspect(id);

    return suspects;
}

void checkModCrash(Context& c)
{
    const QStringList suspects = suspectedMods(c);
    if (suspects.isEmpty())
        return;
    QStringList names;
    for (const auto& id : suspects.mid(0, 3))
        names << c.modName(id);

    Diagnosis diagnosis{ "mod-crash", {}, {}, {} };
    if (suspects.size() == 1) {
        diagnosis.title = tr("%1 seems to have crashed the game").arg(names.first());
        diagnosis.explanation = tr("The error points at %1. Update it, or disable it to check whether the game starts without it.").arg(names.first());
    } else {
        diagnosis.title = tr("A mod seems to have crashed the game");
        diagnosis.explanation =
            tr("The error points at %1. Update them, or disable them one at a time to find the one at fault.").arg(QLocale().createSeparatedList(names));
    }
    c.addDisableFix(diagnosis.fixes, suspects.first());
    diagnosis.fixes << Fix{ Fix::Action::OpenModsPage, tr("Open Mods"), {} };
    c.add(diagnosis);
}

void checkGraphics(Context& c)
{
    static const QRegularExpression glFailure(
        R"re(Pixel format not accelerated|GLFW error 6554[23]|WGL: The driver does not appear to support OpenGL|Failed to create an OpenGL context|Couldn't set pixel format|OpenGL 3\.2 is not supported|No OpenGL context found in the current thread)re",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression driverFrame(R"re(Problematic frame:\s*#\s*C\s+\[(atio6axx|atig6pxx|atioglxx|amdxc64|ig\w*icd\w*|nvoglv\w*|nvd3dum\w*)\.dll)re",
                                                QRegularExpression::CaseInsensitiveOption);
    const auto frame = driverFrame.match(c.text);
    if (!glFailure.match(c.text).hasMatch() && !frame.hasMatch())
        return;

    QString vendor;
    const QString dll = frame.captured(1).toLower();
    static const QRegularExpression vendorLine(R"re((?:Vendor|Renderer)[^\r\n]*?(NVIDIA|GeForce|AMD|ATI|Radeon|Intel))re");
    const QString named = vendorLine.match(c.text).captured(1);
    if (dll.startsWith("nv") || named == "NVIDIA" || named == "GeForce")
        vendor = "NVIDIA";
    else if (dll.startsWith("ati") || dll.startsWith("amd") || named == "AMD" || named == "ATI" || named == "Radeon")
        vendor = "AMD";
    else if (dll.startsWith("ig") || named == "Intel")
        vendor = "Intel";

    static const QHash<QString, QString> driverPages{
        { "NVIDIA", "https://www.nvidia.com/en-us/drivers/" },
        { "AMD", "https://www.amd.com/en/support/download/drivers.html" },
        { "Intel", "https://www.intel.com/content/www/us/en/support/detect.html" },
    };
    Diagnosis diagnosis{ "graphics-driver", tr("Graphics driver problem"),
                         tr("Minecraft couldn't use the graphics card. Install the latest graphics driver from %1. On laptops, also make "
                            "sure Minecraft runs on the dedicated graphics card.")
                             .arg(vendor.isEmpty() ? tr("NVIDIA, AMD or Intel") : vendor),
                         {} };
    if (!vendor.isEmpty())
        diagnosis.fixes << Fix{ Fix::Action::OpenUrl, tr("Get the %1 Driver").arg(vendor), driverPages.value(vendor) };
    c.add(diagnosis);
}

void checkDisk(Context& c)
{
    static const QRegularExpression diskFull(R"re(No space left on device|There is not enough space on the disk)re");
    if (diskFull.match(c.text).hasMatch())
        c.add({ "disk-full", tr("The disk is full"), tr("Minecraft couldn't write its files. Free up some space and try again."), {} });
}

/// The exit code from the launcher's own "Process exited with code 1." line, in whatever language the launcher uses.
std::optional<int> exitCode(const QString& log)
{
    for (const char* source : { "Process exited with code %1.", "Process crashed with exitcode %1." }) {
        const QString translated = QCoreApplication::translate("LoggedProcess", source);
        QString pattern = QRegularExpression::escape(translated);
        pattern.replace(QRegularExpression::escape("%1"), "(-?\\d+)");
        const auto match = QRegularExpression(pattern).match(log);
        if (match.hasMatch())
            return match.captured(1).toInt();
    }
    return std::nullopt;
}

void explainGenericCrash(Context& c)
{
    const auto& in = c.in;
    if (!in.crashReport.isEmpty()) {
        // "Description: Unexpected error\n\njava.lang.NullPointerException: ..."
        static const QRegularExpression description(R"re(Description: [^\r\n]*\r?\n\s*\r?\n([^\r\n]+))re");
        QString error = description.match(in.crashReport).captured(1).trimmed();
        if (error.size() > 300)
            error = error.left(300) + QStringLiteral("...");
        c.add({ "crash-report", tr("Minecraft crashed"),
                error.isEmpty() ? tr("The game wrote a crash report. It often names the mod or setting at fault.")
                                : tr("The crash report says: %1").arg(error),
                { Fix{ Fix::Action::OpenFile, tr("Open Crash Report"), in.crashReportPath } } });
        return;
    }
    if (!in.jvmCrashLog.isEmpty()) {
        c.add({ "jvm-crash", tr("Java itself crashed"),
                tr("This is usually caused by the graphics driver or a broken Java. Updating the graphics driver, or letting the launcher "
                   "pick a different Java, often helps."),
                { Fix{ Fix::Action::OpenFile, tr("Open Error Report"), in.jvmCrashLogPath },
                  Fix{ Fix::Action::UseRecommendedJava, tr("Pick Java Automatically"), {} } } });
        return;
    }
    // only when the game itself ran: failures before that come with the launcher's own message
    if (!in.log.contains("Minecraft process ID:"))
        return;
    const auto code = exitCode(in.log);
    QString explanation;
    if (code && *code < -1000000)  // Windows exception codes like 0xC0000005
        explanation = tr("It stopped with error code %1 and left no crash report. That usually means Java or the graphics driver "
                         "crashed; updating the graphics driver often helps.")
                          .arg(*code);
    else if (code)
        explanation = tr("It stopped with exit code %1 and left no crash report. The last lines of the log usually show why; you can "
                         "also upload the log and ask for help.")
                          .arg(*code);
    else
        explanation = tr("It left no crash report. The last lines of the log usually show why; you can also upload the log and ask "
                         "for help.");
    c.add({ "unknown-crash", tr("Minecraft closed unexpectedly"), explanation, {} });
}

}  // namespace

QString formatMemory(int mib)
{
    if (mib < 1024)
        return tr("%1 MB").arg(mib);
    // whole gigabytes without decimals, the rest with one: "6 GB", "4.5 GB", "15.9 GB"
    return tr("%1 GB").arg(QLocale().toString(mib / 1024.0, 'f', mib % 1024 == 0 ? 0 : 1));
}

Input gather(MinecraftInstance* instance, const QString& log, const QDateTime& launchStarted)
{
    Input in;
    in.log = log;

    // only files from this launch; a little slack for file systems that store coarse times
    const QDateTime since = launchStarted.isValid() ? launchStarted.addSecs(-2) : QDateTime::currentDateTime().addSecs(-3600);
    in.crashReport = readNewestSince(FS::PathCombine(instance->gameRoot(), "crash-reports"), { "crash-*.txt" }, since, in.crashReportPath);
    in.jvmCrashLog = readNewestSince(instance->gameRoot(), { "hs_err_pid*.log" }, since, in.jvmCrashLogPath);

    // "Java is version 17.0.8, using 64 (amd64) architecture, from Eclipse Adoptium"; "1.8.0_402" is Java 8
    static const QRegularExpression javaVersion(R"re(Java is version (\d+)(?:\.(\d+))?)re");
    if (const auto match = javaVersion.match(log); match.hasMatch()) {
        const int first = match.captured(1).toInt();
        in.javaMajor = first == 1 ? match.captured(2).toInt() : first;
    }

    const auto settings = instance->settings();
    in.maxMemoryMiB = settings->get("MaxMemAlloc").toInt();
    in.minMemoryMiB = settings->get("MinMemAlloc").toInt();
    in.physicalMemoryMiB = static_cast<int>(HardwareInfo::totalRamMiB());

    if (auto* profile = instance->getPackProfile()) {
        in.minecraftVersion = profile->getComponentVersion("net.minecraft");
        if (const auto launchProfile = profile->getProfile())
            in.compatibleJavaMajors = launchProfile->getCompatibleJavaMajors();
    }

    for (auto* resource : instance->loaderModList()->allResources()) {
        const auto* mod = dynamic_cast<Mod*>(resource);
        if (mod && mod->enabled() && !mod->mod_id().isEmpty())
            in.mods.insert(mod->mod_id(), { mod->name(), mod->fileinfo().fileName() });
    }
    // the mod list is only loaded once a launch gets that far; otherwise read the mods folder directly
    if (in.mods.isEmpty()) {
        for (const auto& file : QDir(instance->modsRoot()).entryInfoList({ "*.jar", "*.zip" }, QDir::Files)) {
            Mod mod(file);
            if (ModUtils::process(mod, ModUtils::ProcessingLevel::BasicInfoOnly) && !mod.mod_id().isEmpty())
                in.mods.insert(mod.mod_id(), { mod.name(), file.fileName() });
        }
    }
    return in;
}

QList<Diagnosis> analyze(const Input& input)
{
    Context c{ input, {}, {} };
    // the end of a long log matters most; cap the work for logs that ran for hours
    constexpr int maxLog = 4 * 1024 * 1024;
    c.text = (input.log.size() > maxLog ? input.log.right(maxLog) : input.log) + '\n' + input.crashReport + '\n' + input.jvmCrashLog;

    checkJava(c);
    checkMemory(c);
    checkFabricDependencies(c);
    checkForgeDependencies(c);
    checkBrokenModFiles(c);
    checkDisk(c);
    checkGraphics(c);
    // a guess from the stack trace, only when nothing clearer was found
    if (c.found.isEmpty())
        checkModCrash(c);
    if (c.found.isEmpty())
        explainGenericCrash(c);
    return c.found;
}

}  // namespace CrashAnalyzer
