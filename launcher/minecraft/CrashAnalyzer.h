// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>

class MinecraftInstance;

/// Reads the log of a failed launch (and the crash report, if one was written) and explains the likely cause in plain words.
namespace CrashAnalyzer {

struct Fix {
    enum class Action {
        UseRecommendedJava,  // let the launcher pick (and download) a Java that fits the Minecraft version
        OpenJavaSettings,
        SetMaxMemory,  // value: MiB
        OpenMemorySettings,
        DisableMod,  // value: file name in the mods folder
        OpenModsPage,
        OpenVersionPage,
        OpenFile,  // value: path
        OpenUrl,   // value: url
    };
    Action action;
    QString label;
    QString value;
};

struct Diagnosis {
    QString id;  // stable, e.g. "out-of-memory"
    QString title;
    QString explanation;
    QList<Fix> fixes;
};

struct ModInfo {
    QString name;
    QString file;  // file name in the mods folder
};

struct Input {
    QString log;
    QString crashReport;  // written during the failed launch, if any
    QString crashReportPath;
    QString jvmCrashLog;  // hs_err_pid*.log written during the failed launch, if any
    QString jvmCrashLogPath;
    int javaMajor = 0;  // 0 when unknown
    int maxMemoryMiB = 0;
    int minMemoryMiB = 0;
    int physicalMemoryMiB = 0;
    QString minecraftVersion;
    QList<int> compatibleJavaMajors;  // the Java versions the Minecraft version is known to work with
    QHash<QString, ModInfo> mods;     // enabled mods by mod id
};

/// Collects what analyze() looks at, for a launch of @p instance that started at @p launchStarted and just failed.
Input gather(MinecraftInstance* instance, const QString& log, const QDateTime& launchStarted);

/// Likely causes, most specific first. Empty when the launch was not a crash worth explaining.
QList<Diagnosis> analyze(const Input& input);

/// "512 MB", "6 GB", "4.5 GB"
QString formatMemory(int mib);

}  // namespace CrashAnalyzer
