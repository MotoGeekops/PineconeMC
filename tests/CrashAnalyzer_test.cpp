// SPDX-License-Identifier: GPL-3.0-only
#include <QLocale>
#include <QTest>

#include "minecraft/CrashAnalyzer.h"

using CrashAnalyzer::Diagnosis;
using CrashAnalyzer::Fix;
using CrashAnalyzer::Input;

namespace {
// what the launcher logs once the game process runs
const QString s_gameStarted = "Minecraft process ID: 1234\n\n";

Input input(const QString& log)
{
    Input in;
    in.log = s_gameStarted + log;
    in.javaMajor = 17;
    in.maxMemoryMiB = 4096;
    in.minMemoryMiB = 512;
    in.physicalMemoryMiB = 16384;
    in.minecraftVersion = "1.20.1";
    in.compatibleJavaMajors = { 17, 21 };
    in.mods.insert("sodium", { "Sodium", "sodium-fabric-0.5.3+mc1.20.1.jar" });
    in.mods.insert("iris", { "Iris", "iris-mc1.20.1-1.6.11.jar" });
    in.mods.insert("create", { "Create", "create-1.20.1-0.5.1.f.jar" });
    in.mods.insert("optifabric", { "OptiFabric", "optifabric-1.13.0.jar" });
    return in;
}

QStringList ids(const QList<Diagnosis>& diagnoses)
{
    QStringList result;
    for (const auto& diagnosis : diagnoses)
        result << diagnosis.id;
    return result;
}

bool hasFix(const Diagnosis& diagnosis, Fix::Action action, const QString& value = {})
{
    for (const auto& fix : diagnosis.fixes) {
        if (fix.action == action && (value.isEmpty() || fix.value == value))
            return true;
    }
    return false;
}
}  // namespace

class CrashAnalyzerTest : public QObject {
    Q_OBJECT

   private slots:
    void initTestCase() { QLocale::setDefault(QLocale::c()); }

    void fabricMissingDependency()
    {
        const auto result = CrashAnalyzer::analyze(input(
            "[main/ERROR]: Incompatible mods found!\n"
            "net.fabricmc.loader.impl.FormattedException: Some of your mods are incompatible with the game or each other!\n"
            "A potential solution has been determined, this may resolve your problem:\n"
            "\t - Install fabric-api, version 0.92.0 or later.\n"
            "More details:\n"
            "\t - Mod 'Iris' (iris) 1.6.11 requires version 0.5.0 or later of sodium, which is missing!\n"
            "\t - Mod 'Create Fabric' (create) 0.5.1 requires version 0.92.0 or later of fabric-api, which is missing!\n"));
        QCOMPARE(ids(result), QStringList({ "missing-mod", "missing-mod" }));
        // sorted by mod id: fabric-api, then sodium
        QCOMPARE(result[0].title, QString("Fabric API is missing"));
        QVERIFY(result[0].explanation.contains("Create"));
        QVERIFY(result[0].explanation.contains("0.92.0 or later"));
        QCOMPARE(result[1].title, QString("Sodium is missing"));
        QVERIFY(hasFix(result[1], Fix::Action::OpenModsPage));
        QVERIFY(hasFix(result[1], Fix::Action::DisableMod, "iris-mc1.20.1-1.6.11.jar"));
    }

    void namesFromTheLog()
    {
        // a mod that isn't in the instance's list still gets its proper name from the loader's message
        auto in = input("\t - Mod 'Sodium Extra' (sodium-extra) 0.5.4 requires any version of sodium, which is missing!\n");
        const auto result = CrashAnalyzer::analyze(in);
        QCOMPARE(ids(result), QStringList({ "missing-mod" }));
        QVERIFY(result[0].explanation.startsWith("Sodium Extra needs Sodium"));
    }

    void fabricWrongMinecraftVersion()
    {
        const auto result = CrashAnalyzer::analyze(input(
            "\t - Mod 'Sodium' (sodium) 0.5.8 requires version 1.20.4 of 'Minecraft' (minecraft), but only the wrong version is "
            "present: 1.20.1!\n"));
        QCOMPARE(ids(result), QStringList({ "wrong-minecraft-version" }));
        QVERIFY(result[0].explanation.contains("1.20.4"));
        QVERIFY(result[0].explanation.contains("1.20.1"));
        QVERIFY(hasFix(result[0], Fix::Action::DisableMod, "sodium-fabric-0.5.3+mc1.20.1.jar"));
    }

    void fabricWrongDependencyVersion()
    {
        const auto result = CrashAnalyzer::analyze(
            input("\t - Mod 'Iris' (iris) 1.6.11 requires version 0.5.4 or later of mod 'Sodium' (sodium), but only the wrong version is "
                  "present: 0.5.3!\n"));
        QCOMPARE(ids(result), QStringList({ "wrong-mod-version" }));
        QCOMPARE(result[0].title, QString("Iris needs a different Sodium"));
        QVERIFY(result[0].explanation.contains("0.5.4 or later"));
    }

    void fabricIncompatible()
    {
        const auto result = CrashAnalyzer::analyze(
            input("\t - Mod 'OptiFabric' (optifabric) 1.13.0 is incompatible with any version of mod 'Sodium' (sodium), yet a conflicting "
                  "version is present: 0.5.3!\n"));
        QCOMPARE(ids(result), QStringList({ "incompatible-mods" }));
        QVERIFY(hasFix(result[0], Fix::Action::DisableMod, "optifabric-1.13.0.jar"));
        QVERIFY(hasFix(result[0], Fix::Action::DisableMod, "sodium-fabric-0.5.3+mc1.20.1.jar"));
    }

    void fabricJavaVersion()
    {
        const auto result = CrashAnalyzer::analyze(
            input("\t - Mod 'Sodium' (sodium) 0.6.0 requires version 21 or later of 'OpenJDK 64-Bit Server VM' (java), but only the wrong "
                  "version is present: 17!\n"));
        QCOMPARE(ids(result), QStringList({ "java-too-old" }));
        QVERIFY(result[0].explanation.contains("21"));
        QVERIFY(result[0].explanation.contains("17"));
        // Java 17 is fine for Minecraft 1.20.1, so picking automatically would not help
        QVERIFY(!hasFix(result[0], Fix::Action::UseRecommendedJava));
        QVERIFY(hasFix(result[0], Fix::Action::OpenJavaSettings));
    }

    void classFileVersion()
    {
        auto in = input(
            "Exception in thread \"main\" java.lang.UnsupportedClassVersionError: net/minecraft/client/main/Main has been compiled by a "
            "more recent version of the Java Runtime (class file version 65.0), this version of the Java Runtime only recognizes class "
            "file versions up to 52.0\n");
        in.javaMajor = 8;
        const auto result = CrashAnalyzer::analyze(in);
        QCOMPARE(ids(result), QStringList({ "java-too-old" }));
        QVERIFY(result[0].explanation.contains("Java 21"));
        QVERIFY(result[0].explanation.contains("Java 8"));
        // Java 8 doesn't fit Minecraft 1.20.1 at all, so letting the launcher pick fixes it
        QVERIFY(hasFix(result[0], Fix::Action::UseRecommendedJava));
    }

    void javaTooNewForOldForge()
    {
        auto in = input(
            "java.lang.ClassCastException: class jdk.internal.loader.ClassLoaders$AppClassLoader cannot be cast to class "
            "java.net.URLClassLoader\n");
        in.minecraftVersion = "1.12.2";
        in.compatibleJavaMajors = { 8 };
        const auto result = CrashAnalyzer::analyze(in);
        QCOMPARE(ids(result), QStringList({ "java-too-new" }));
        QVERIFY(hasFix(result[0], Fix::Action::UseRecommendedJava));
    }

    void forgeMissingDependency()
    {
        const auto result = CrashAnalyzer::analyze(
            input("[main/ERROR] [net.minecraftforge.fml.loading.ModSorter/LOADING]: Missing or unsupported mandatory dependencies:\n"
                  "\tMod ID: 'geckolib', Requested by: 'create', Expected range: '[4.2,)', Actual version: '[MISSING]'\n"
                  "\tMod ID: 'minecraft', Requested by: 'iris', Expected range: '[1.20.4,1.21)', Actual version: '1.20.1'\n"
                  "\tMod ID: 'forge', Requested by: 'sodium', Expected range: '[47.2.0,)', Actual version: '47.1.0'\n"));
        QCOMPARE(ids(result), QStringList({ "wrong-minecraft-version", "wrong-loader-version", "missing-mod" }));
        QCOMPARE(result[2].title, QString("GeckoLib is missing"));
        QVERIFY(result[2].explanation.contains("4.2 or later"));
        QVERIFY(result[0].explanation.contains("1.20.4 to 1.21"));
        QVERIFY(result[1].explanation.contains("Forge 47.2.0 or later"));
        QVERIFY(hasFix(result[1], Fix::Action::OpenVersionPage));
    }

    void duplicateMods()
    {
        const auto forge = CrashAnalyzer::analyze(input(
            "Found duplicate mods:\n\tMod ID: 'jei' from mod files: jei-1.20.1-forge-15.2.0.jar, jei-1.20.1-forge-15.3.0.jar\n"));
        QCOMPARE(ids(forge), QStringList({ "duplicate-mod" }));
        QVERIFY(forge[0].explanation.contains("jei-1.20.1-forge-15.3.0.jar"));

        const auto neoforge = CrashAnalyzer::analyze(
            input(QString("Mod %1ejei%1r is present in multiple files: jei-a.jar, jei-b.jar\n").arg(QChar(0x00A7))));
        QCOMPARE(ids(neoforge), QStringList({ "duplicate-mod" }));
    }

    void outOfMemory()
    {
        const auto result = CrashAnalyzer::analyze(input("java.lang.OutOfMemoryError: Java heap space\n"));
        QCOMPARE(ids(result), QStringList({ "out-of-memory" }));
        QVERIFY(hasFix(result[0], Fix::Action::SetMaxMemory, "6144"));
    }

    void heapCannotBeReserved()
    {
        auto in = input("Error occurred during initialization of VM\nCould not reserve enough space for 8388608KB object heap\n");
        in.maxMemoryMiB = 8192;
        const auto result = CrashAnalyzer::analyze(in);
        QCOMPARE(ids(result), QStringList({ "memory-unavailable" }));
        QVERIFY(hasFix(result[0], Fix::Action::SetMaxMemory, "4096"));
    }

    void graphicsDriver()
    {
        const auto result = CrashAnalyzer::analyze(
            input("[Render thread/ERROR]: GLFW error 65542: WGL: The driver does not appear to support OpenGL\n"
                  "GL info: ' Vendor: 'NVIDIA Corporation' Version: '4.6.0' Renderer: 'NVIDIA GeForce GTX 1060/PCIe/SSE2'\n"));
        QCOMPARE(ids(result), QStringList({ "graphics-driver" }));
        QVERIFY(hasFix(result[0], Fix::Action::OpenUrl, "https://www.nvidia.com/en-us/drivers/"));
    }

    void brokenModFile()
    {
        const auto result = CrashAnalyzer::analyze(
            input("java.lang.RuntimeException: Error analyzing [C:\\Games\\inst\\.minecraft\\mods\\broken-mod.jar]: "
                  "java.util.zip.ZipException: zip END header not found\n"));
        QCOMPARE(ids(result), QStringList({ "broken-mod-file" }));
        QVERIFY(hasFix(result[0], Fix::Action::DisableMod, "broken-mod.jar"));
    }

    void modCrashFromStackTrace()
    {
        auto in = input(QString());
        in.crashReport =
            "---- Minecraft Crash Report ----\n"
            "Description: Rendering overlay\n\n"
            "java.lang.NullPointerException: Cannot invoke \"Object.hashCode()\" because \"key\" is null\n"
            "\tat net.minecraft.class_310.handler$zbk000$sodium$onRender(class_310.java:1234)\n"
            "\tat net.minecraft.class_310.handler$zbk001$sodium$postRender(class_310.java:1240)\n"
            "\tat net.minecraft.class_757.handler$bah000$iris$render(class_757.java:99)\n";
        const auto result = CrashAnalyzer::analyze(in);
        QCOMPARE(ids(result), QStringList({ "mod-crash" }));
        QVERIFY(result[0].explanation.contains("Sodium"));
        // the mod named most often comes first
        QVERIFY(hasFix(result[0], Fix::Action::DisableMod, "sodium-fabric-0.5.3+mc1.20.1.jar"));
    }

    void entrypointCrash()
    {
        const auto result = CrashAnalyzer::analyze(input(
            "java.lang.RuntimeException: Could not execute entrypoint stage 'main' due to errors, provided by 'create' at "
            "'com.simibubi.create.Create'!\n"));
        QCOMPARE(ids(result), QStringList({ "mod-crash" }));
        QCOMPARE(result[0].title, QString("Create seems to have crashed the game"));
    }

    void genericCrashReport()
    {
        auto in = input(QString());
        in.crashReport = "---- Minecraft Crash Report ----\n// Oops.\n\nTime: 2026-10-13 14:03:22\nDescription: Unexpected error\n\n"
                         "java.lang.IllegalStateException: Something odd happened\n\tat net.minecraft.class_1.method_1(class_1.java:1)\n";
        in.crashReportPath = "C:/crash-reports/crash.txt";
        const auto result = CrashAnalyzer::analyze(in);
        QCOMPARE(ids(result), QStringList({ "crash-report" }));
        QVERIFY(result[0].explanation.contains("IllegalStateException: Something odd happened"));
        QVERIFY(hasFix(result[0], Fix::Action::OpenFile, "C:/crash-reports/crash.txt"));
    }

    void exitCodeOnly()
    {
        const auto result = CrashAnalyzer::analyze(input("Process exited with code 1.\n"));
        QCOMPARE(ids(result), QStringList({ "unknown-crash" }));
        QVERIFY(result[0].explanation.contains("exit code 1"));
    }

    void failureBeforeTheGameStarted()
    {
        // a failed download or sign-in comes with the launcher's own message: nothing to explain here
        Input in;
        in.log = "Checking Java version...\nFailed to download https://example.invalid/file.jar\n";
        QVERIFY(CrashAnalyzer::analyze(in).isEmpty());
    }

    void missingJava()
    {
        Input in;
        in.log = "The Java binary \"C:/Java/bin/javaw.exe\" couldn't be found. Please fix the Java path override in the instance's "
                 "settings or disable it.\n";
        const auto result = CrashAnalyzer::analyze(in);
        QCOMPARE(ids(result), QStringList({ "java-missing" }));
        QVERIFY(hasFix(result[0], Fix::Action::UseRecommendedJava));
    }

    void readableRanges()
    {
        QCOMPARE(CrashAnalyzer::formatMemory(4096), QString("4 GB"));
        QCOMPARE(CrashAnalyzer::formatMemory(4608), QString("4.5 GB"));
        QCOMPARE(CrashAnalyzer::formatMemory(1000), QString("1000 MB"));
        QCOMPARE(CrashAnalyzer::formatMemory(16310), QString("15.9 GB"));
    }
};

QTEST_GUILESS_MAIN(CrashAnalyzerTest)

#include "CrashAnalyzer_test.moc"
