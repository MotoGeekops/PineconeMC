// SPDX-License-Identifier: GPL-3.0-only
#include "TroubleshooterDialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QGridLayout>
#include <QLabel>
#include <QLocale>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSysInfo>
#include <QTimer>
#include <QVBoxLayout>

#include "Application.h"
#include "BuildConfig.h"
#include "FileSystem.h"
#include "HardwareInfo.h"
#include "StringUtils.h"
#include "java/JavaChecker.h"
#include "minecraft/CrashAnalyzer.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/auth/AccountList.h"
#include "minecraft/auth/MinecraftAccount.h"
#include "settings/SettingsObject.h"
#include "ui/GuiUtil.h"
#include "ui/instanceview/InstanceSummary.h"

namespace {
QString memory(qint64 mib)
{
    return CrashAnalyzer::formatMemory(static_cast<int>(mib));
}
}  // namespace

TroubleshooterDialog::TroubleshooterDialog(BaseInstance* instance, QWidget* parent) : QDialog(parent), m_instance(instance)
{
    setWindowTitle(tr("Troubleshooter"));
    resize(660, 640);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(8);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("troubleshooterSummary"));
    m_summary->setWordWrap(true);
    QFont summaryFont = m_summary->font();
    summaryFont.setPointSizeF(summaryFont.pointSizeF() * 1.3);
    summaryFont.setBold(true);
    m_summary->setFont(summaryFont);
    layout->addWidget(m_summary);

    auto* intro = new QLabel(instance ? tr("Checks this computer, the launcher and the selected instance, %1.").arg(instance->name())
                                      : tr("Checks this computer and the launcher. Select an instance first to check it too."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_rowsWidget = new QWidget(scroll);
    m_rowsLayout = new QVBoxLayout(m_rowsWidget);
    m_rowsLayout->setContentsMargins(0, 6, 10, 6);
    m_rowsLayout->setSpacing(14);
    m_rowsLayout->addStretch(1);
    scroll->setWidget(m_rowsWidget);
    layout->addWidget(scroll, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* copyButton = buttons->addButton(tr("Copy Report"), QDialogButtonBox::ActionRole);
    copyButton->setToolTip(tr("Copy the results, to paste them when you ask for help."));
    auto* againButton = buttons->addButton(tr("Check Again"), QDialogButtonBox::ActionRole);
    connect(copyButton, &QPushButton::clicked, this, [this] { GuiUtil::setClipboardText(report()); });
    connect(againButton, &QPushButton::clicked, this, &TroubleshooterDialog::runChecks);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    runChecks();
}

TroubleshooterDialog::~TroubleshooterDialog() = default;

TroubleshooterDialog::Row* TroubleshooterDialog::addRow(const QString& title)
{
    auto row = std::make_unique<Row>();
    auto* widget = new QWidget(m_rowsWidget);
    auto* grid = new QGridLayout(widget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(2);

    row->icon = new QLabel(widget);
    row->icon->setFixedSize(20, 20);
    row->title = new QLabel(title, widget);
    QFont titleFont = row->title->font();
    titleFont.setBold(true);
    row->title->setFont(titleFont);
    row->detail = new QLabel(widget);
    row->detail->setWordWrap(true);
    row->detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QPalette detailPalette = row->detail->palette();
    QColor muted = detailPalette.color(QPalette::WindowText);
    muted.setAlphaF(0.72f);
    detailPalette.setColor(QPalette::WindowText, muted);
    row->detail->setPalette(detailPalette);
    row->fix = new QPushButton(widget);
    row->fix->hide();

    grid->addWidget(row->icon, 0, 0, 2, 1, Qt::AlignTop);
    grid->addWidget(row->title, 0, 1);
    grid->addWidget(row->detail, 1, 1);
    grid->addWidget(row->fix, 0, 2, 2, 1, Qt::AlignVCenter);
    grid->setColumnStretch(1, 1);
    m_rowsLayout->insertWidget(m_rowsLayout->count() - 1, widget);

    auto* raw = row.get();
    connect(row->fix, &QPushButton::clicked, this, [raw] {
        if (raw->fixAction)
            raw->fixAction();
    });
    m_rows.push_back(std::move(row));
    setResult(raw, Status::Checking, tr("Checking..."));
    return raw;
}

void TroubleshooterDialog::setResult(Row* row, Status status, const QString& detail, const QString& fixLabel, std::function<void()> fixAction)
{
    static const char* icons[] = { "status-running", "status-good", "status-yellow", "status-bad" };
    row->status = status;
    row->icon->setPixmap(QIcon::fromTheme(icons[static_cast<int>(status)]).pixmap(QSize(20, 20), devicePixelRatioF()));
    row->detail->setText(detail);
    row->fixAction = std::move(fixAction);
    row->fix->setText(fixLabel);
    row->fix->setVisible(!fixLabel.isEmpty() && row->fixAction);
    updateSummary();
}

void TroubleshooterDialog::updateSummary()
{
    int checking = 0;
    int warnings = 0;
    int problems = 0;
    for (const auto& row : m_rows) {
        checking += row->status == Status::Checking;
        warnings += row->status == Status::Warning;
        problems += row->status == Status::Problem;
    }
    if (checking > 0)
        m_summary->setText(tr("Checking your setup..."));
    else if (problems > 0)
        m_summary->setText(problems == 1 ? tr("Found a problem") : tr("Found %1 problems").arg(problems));
    else if (warnings > 0)
        m_summary->setText(warnings == 1 ? tr("Found one thing worth a look") : tr("Found %1 things worth a look").arg(warnings));
    else
        m_summary->setText(tr("Everything looks fine"));
}

QString TroubleshooterDialog::report() const
{
    QStringList lines;
    lines << QString("%1 %2 troubleshooter, %3")
                 .arg(BuildConfig.LAUNCHER_DISPLAYNAME, BuildConfig.printableVersionString(),
                      QDateTime::currentDateTime().toString(Qt::ISODate));
    lines << QString("System: %1 (%2)").arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture());
    if (m_instance)
        lines << QString("Instance: %1").arg(m_instance->name());
    lines << QString();
    static const char* labels[] = { "CHECKING", "OK", "WARNING", "PROBLEM" };
    for (const auto& row : m_rows)
        lines << QString("[%1] %2: %3").arg(labels[static_cast<int>(row->status)], row->title->text(), row->detail->text());
    return lines.join('\n');
}

void TroubleshooterDialog::runChecks()
{
    ++m_generation;
    for (const auto& row : m_rows)
        delete row->icon->parentWidget();
    m_rows.clear();

    checkJava();
    checkMemory();
    checkDisk();
    checkFolders();
    checkAccount();
    checkGraphics();

    const QString metaOverride = APPLICATION->settings()->get("MetaURLOverride").toString();
    const QString metaUrl = metaOverride.isEmpty() ? BuildConfig.META_URL : metaOverride;
    checkConnection(tr("Minecraft downloads"), "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json",
                    tr("Needed to install and update the game."));
    checkConnection(tr("Minecraft libraries"), "https://libraries.minecraft.net/", tr("Needed to install and update the game."));
    checkConnection(tr("Game assets"), "https://resources.download.minecraft.net/", tr("Needed for sounds, music and languages."));
    checkConnection(tr("Launcher metadata"), metaUrl, tr("Needed to install Minecraft versions and mod loaders."));
    checkConnection(tr("Ely.by accounts"), "https://account.ely.by/", tr("Needed to sign in with an Ely.by account."));
    checkConnection(tr("Ely.by skins"), "https://skinsystem.ely.by/", tr("Needed for the skins of Ely.by accounts."));
    checkConnection(tr("Microsoft accounts"), "https://login.microsoftonline.com/", tr("Needed to sign in with a Microsoft account."));
    checkConnection(tr("Modrinth"), "https://api.modrinth.com/", tr("Needed to download mods and modpacks from Modrinth."));
    checkConnection(tr("CurseForge"), "https://api.curseforge.com/", tr("Needed to download mods and modpacks from CurseForge."));

    // anything still running by then is reported, rather than spinning forever
    QTimer::singleShot(std::chrono::seconds(30), this, [this, generation = m_generation] {
        if (generation != m_generation)
            return;
        for (const auto& row : m_rows) {
            if (row->status == Status::Checking)
                setResult(row.get(), Status::Warning, tr("This check didn't finish in time."));
        }
    });
}

void TroubleshooterDialog::checkJava()
{
    auto* row = addRow(tr("Java"));
    auto* minecraft = dynamic_cast<MinecraftInstance*>(m_instance.data());
    const auto settings = minecraft ? minecraft->settings() : APPLICATION->settings();
    const bool automatic = APPLICATION->settings()->get("AutomaticJavaSwitch").toBool();
    const bool overridden = minecraft && minecraft->settings()->get("OverrideJavaLocation").toBool();
    const QString configured = settings->get("JavaPath").toString();
    const QString resolved = configured.isEmpty() ? QString() : QStandardPaths::findExecutable(FS::ResolveExecutable(configured));

    QPointer<MinecraftInstance> instance(minecraft);
    auto pickAutomatically = [this, instance] {
        APPLICATION->settings()->set("AutomaticJavaSwitch", true);
        if (BuildConfig.JAVA_DOWNLOADER_ENABLED)
            APPLICATION->settings()->set("AutomaticJavaDownload", true);
        if (instance)
            instance->settings()->set("OverrideJavaLocation", false);
        runChecks();
    };

    if (automatic && !overridden) {
        setResult(row, Status::Good, tr("The launcher picks a fitting Java for each Minecraft version, and downloads it when needed."));
        return;
    }
    if (resolved.isEmpty()) {
        setResult(row, Status::Problem,
                  configured.isEmpty() ? tr("No Java is set up.") : tr("The Java that is set up doesn't exist any more: %1").arg(configured),
                  tr("Pick Java Automatically"), pickAutomatically);
        return;
    }

    QList<int> compatible;
    QString minecraftVersion;
    if (minecraft) {
        auto* profile = minecraft->getPackProfile();
        minecraftVersion = profile->getComponentVersion("net.minecraft");
        if (const auto launchProfile = profile->getProfile())
            compatible = launchProfile->getCompatibleJavaMajors();
    }

    auto checker = makeShared<JavaChecker>(resolved, QString(), 0, 0, 0, 0);
    connect(checker.get(), &JavaChecker::checkFinished, this,
            [this, row, generation = m_generation, resolved, compatible, minecraftVersion,
             pickAutomatically](const JavaChecker::Result& result) {
                if (generation != m_generation)
                    return;
                if (result.validity != JavaChecker::Result::Validity::Valid) {
                    setResult(row, Status::Problem, tr("The Java at %1 doesn't run properly.").arg(resolved), tr("Pick Java Automatically"),
                              pickAutomatically);
                    return;
                }
                const QString about = tr("Java %1 from %2, %3-bit.")
                                          .arg(result.javaVersion.toString(), result.javaVendor, result.is_64bit ? "64" : "32");
                const int major = result.javaVersion.major();
                if (!compatible.isEmpty() && !compatible.contains(major)) {
                    QStringList versions;
                    for (int version : compatible)
                        versions << QString::number(version);
                    setResult(row, Status::Warning,
                              tr("%1 Minecraft %2 is made for Java %3.").arg(about, minecraftVersion, versions.join(", ")),
                              tr("Pick Java Automatically"), pickAutomatically);
                } else if (!result.is_64bit) {
                    setResult(row, Status::Warning, tr("%1 32-bit Java can only use about 1.5 GB of memory.").arg(about),
                              tr("Pick Java Automatically"), pickAutomatically);
                } else {
                    setResult(row, Status::Good, about);
                }
            });
    m_tasks.push_back(checker);
    checker->start();
}

void TroubleshooterDialog::checkMemory()
{
    auto* row = addRow(tr("Memory"));
    const qint64 total = static_cast<qint64>(HardwareInfo::totalRamMiB());
    const auto settings = m_instance ? m_instance->settings() : APPLICATION->settings();
    const qint64 maximum = settings->get("MaxMemAlloc").toInt();
    const int mods = m_instance ? InstanceSummary::modCount(m_instance) : 0;
    const QString who = m_instance ? m_instance->name() : tr("Minecraft");

    QPointer<BaseInstance> instance(m_instance);
    auto openSettings = [this, instance] {
        if (instance)
            APPLICATION->showInstanceWindow(instance, "settings");
        else
            APPLICATION->ShowGlobalSettings(this, "java-settings");
    };

    if (total <= 0) {
        setResult(row, Status::Good, tr("%1 may use up to %2.").arg(who, memory(maximum)));
    } else if (maximum > total - 1024) {
        setResult(row, Status::Problem,
                  tr("%1 may use up to %2, but the computer only has %3. Lower it to about %4.")
                      .arg(who, memory(maximum), memory(total), memory(std::max<qint64>(1024, total / 2 / 512 * 512))),
                  tr("Memory Settings"), openSettings);
    } else if (maximum > total * 3 / 4) {
        setResult(row, Status::Warning,
                  tr("%1 may use up to %2 of the computer's %3. Leave some for the system, or the game may stutter.")
                      .arg(who, memory(maximum), memory(total)),
                  tr("Memory Settings"), openSettings);
    } else if (mods >= 100 && maximum < 4096) {
        setResult(row, Status::Warning,
                  tr("%1 may use up to %2 with %3 mods installed. Modded instances usually need 6 to 8 GB.")
                      .arg(who, memory(maximum))
                      .arg(mods),
                  tr("Memory Settings"), openSettings);
    } else {
        setResult(row, Status::Good, tr("%1 may use up to %2 of the computer's %3.").arg(who, memory(maximum), memory(total)));
    }
}

void TroubleshooterDialog::checkDisk()
{
    auto* row = addRow(tr("Disk space"));
    const QString instances = QDir(APPLICATION->dataRoot()).absoluteFilePath(APPLICATION->settings()->get("InstanceDir").toString());
    QStorageInfo storage(QFileInfo::exists(instances) ? instances : APPLICATION->dataRoot());
    if (!storage.isValid()) {
        setResult(row, Status::Warning, tr("Couldn't read the free space of the launcher's drive."));
        return;
    }
    const qint64 free = storage.bytesAvailable();
    QString drive = QDir::toNativeSeparators(storage.rootPath());
    if (drive.size() > 1 && drive.endsWith(QDir::separator()))
        drive.chop(1);
    const QString amount = StringUtils::humanReadableFileSize(static_cast<double>(free), true);
    constexpr qint64 gigabyte = 1000LL * 1000 * 1000;
    if (free < gigabyte)
        setResult(row, Status::Problem,
                  tr("Only %1 free on %2. Minecraft needs room for its files and worlds: free up some space.").arg(amount, drive));
    else if (free < 5 * gigabyte)
        setResult(row, Status::Warning, tr("%1 free on %2. Big modpacks may not fit.").arg(amount, drive));
    else
        setResult(row, Status::Good, tr("%1 free on %2.").arg(amount, drive));
}

void TroubleshooterDialog::checkFolders()
{
    auto* row = addRow(tr("Launcher folder"));
    const QString root = QDir::toNativeSeparators(QDir(APPLICATION->dataRoot()).absolutePath());
    const QString instances =
        QDir::toNativeSeparators(QDir(APPLICATION->dataRoot()).absoluteFilePath(APPLICATION->settings()->get("InstanceDir").toString()));

    QStringList problems;
    QStringList warnings;
    for (const auto& path : QSet<QString>{ root, instances }) {
        if (path.contains('!'))
            problems << tr("%1 contains \"!\", which breaks older Minecraft versions.").arg(path);
        if (path.contains("OneDrive", Qt::CaseInsensitive))
            warnings << tr("%1 is in OneDrive. Syncing can lock or remove files while the game runs; a folder outside OneDrive works "
                           "better.")
                            .arg(path);
        static const QRegularExpression nonAscii(QStringLiteral("[^\\x{0000}-\\x{007F}]"));
        if (nonAscii.match(path).hasMatch())
            warnings << tr("%1 has special characters, which some older versions and mods can't handle.").arg(path);
    }
    if (!problems.isEmpty())
        setResult(row, Status::Problem, (problems + warnings).join('\n'));
    else if (!warnings.isEmpty())
        setResult(row, Status::Warning, warnings.join('\n'));
    else
        setResult(row, Status::Good, root);
}

void TroubleshooterDialog::checkAccount()
{
    auto* row = addRow(tr("Account"));
    auto manageAccounts = [this] { APPLICATION->ShowGlobalSettings(this, "accounts"); };
    const auto account = APPLICATION->accounts()->defaultAccount();
    if (!account) {
        setResult(row, Status::Warning, tr("No account is selected. Add or pick one to play."), tr("Manage Accounts"), manageAccounts);
        return;
    }
    QString type;
    switch (account->accountType()) {
        case AccountType::MSA:
            type = tr("Microsoft");
            break;
        case AccountType::Ely:
            type = tr("Ely.by");
            break;
        case AccountType::Offline:
            type = tr("offline");
            break;
    }
    const QString who = tr("%1 (%2 account)").arg(account->profileName(), type);
    switch (account->accountState()) {
        case AccountState::Expired:
            setResult(row, Status::Problem, tr("%1 needs to sign in again.").arg(who), tr("Manage Accounts"), manageAccounts);
            break;
        case AccountState::Errored:
            setResult(row, Status::Problem,
                      account->lastError().isEmpty() ? tr("%1 couldn't sign in.").arg(who)
                                                     : tr("%1 couldn't sign in: %2").arg(who, account->lastError()),
                      tr("Manage Accounts"), manageAccounts);
            break;
        case AccountState::Disabled:
        case AccountState::Gone:
            setResult(row, Status::Problem, tr("%1 can't be used any more.").arg(who), tr("Manage Accounts"), manageAccounts);
            break;
        default:
            setResult(row, Status::Good, who);
            break;
    }
}

void TroubleshooterDialog::checkGraphics()
{
    auto* row = addRow(tr("Graphics"));
    QOpenGLContext context;
    QOffscreenSurface surface;
    if (context.create()) {
        surface.setFormat(context.format());
        surface.create();
    }
    if (!context.isValid() || !surface.isValid() || !context.makeCurrent(&surface)) {
        setResult(row, Status::Problem,
                  tr("OpenGL isn't available, and Minecraft needs it. Install the graphics driver from your graphics card maker."));
        return;
    }
    auto* gl = context.functions();
    const QString renderer = QString::fromLatin1(reinterpret_cast<const char*>(gl->glGetString(GL_RENDERER)));
    const QString version = QString::fromLatin1(reinterpret_cast<const char*>(gl->glGetString(GL_VERSION)));
    context.doneCurrent();

    static const QRegularExpression versionNumber(R"re(^(\d+)\.(\d+))re");
    const auto match = versionNumber.match(version);
    const int major = match.captured(1).toInt();
    const int minor = match.captured(2).toInt();
    const QString shortVersion = match.hasMatch() ? QString("%1.%2").arg(major).arg(minor) : version;

    static const QRegularExpression software(R"re(GDI Generic|Microsoft Basic Render|llvmpipe|softpipe|SwiftShader)re");
    if (software.match(renderer).hasMatch()) {
        setResult(row, Status::Problem,
                  tr("No graphics driver is in use (%1). Install the driver from your graphics card maker; Minecraft runs badly, or not "
                     "at all, without it.")
                      .arg(renderer));
    } else if (match.hasMatch() && major * 10 + minor < 32) {
        setResult(row, Status::Warning,
                  tr("%1 supports OpenGL %2. Minecraft 1.17 and newer need OpenGL 3.2, so update the graphics driver.")
                      .arg(renderer, shortVersion));
    } else {
        setResult(row, Status::Good, tr("%1, OpenGL %2.").arg(renderer, shortVersion));
    }
}

void TroubleshooterDialog::checkConnection(const QString& title, const QString& url, const QString& purpose)
{
    auto* row = addRow(title);
    QNetworkRequest request{ QUrl(url) };
    request.setTransferTimeout(15000);
    request.setHeader(QNetworkRequest::UserAgentHeader, APPLICATION->getUserAgent());
    QElapsedTimer timer;
    timer.start();
    auto* reply = APPLICATION->network()->head(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, row, timer, purpose, generation = m_generation] {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        // any answer at all means the server can be reached
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() > 0) {
            setResult(row, Status::Good, tr("Reachable (%1 ms). %2").arg(timer.elapsed()).arg(purpose));
            return;
        }
        setResult(row, Status::Problem,
                  tr("Can't connect: %1. %2 Check the internet connection, a firewall or antivirus, and the proxy settings.")
                      .arg(reply->errorString(), purpose),
                  tr("Proxy Settings"), [this] { APPLICATION->ShowGlobalSettings(this, "proxy-settings"); });
    });
}
