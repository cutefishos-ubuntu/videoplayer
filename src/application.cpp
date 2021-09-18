/*
 * SPDX-FileCopyrightText: 2020 George Florea Bănuș <georgefb899@gmail.com>
 * SPDX-FileCopyrightText: 2021 Reion Wong <support@cutefishos.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "_debug.h"
#include "application.h"
#include "audiosettings.h"
#include "customcommandsmodel.h"
#include "generalsettings.h"
#include "global.h"
#include "haction.h"
#include "lockmanager.h"
#include "mediaplayer2.h"
#include "mediaplayer2player.h"
#include "mousesettings.h"
#include "mpvobject.h"
#include "playbacksettings.h"
#include "playlistitem.h"
#include "playlistmodel.h"
#include "playlistsettings.h"
#include "subtitlesfoldersmodel.h"
#include "subtitlessettings.h"
#include "thumbnailimageprovider.h"
#include "tracksmodel.h"
#include "videosettings.h"
#include "worker.h"

#include <clocale>

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDBusConnection>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeDatabase>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickView>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>
#include <QThread>

#include <KAboutApplicationDialog>
#include <KAboutData>
#include <KColorSchemeManager>
#include <KConfig>
#include <KConfigGroup>
#include <KFileMetaData/Properties>
#include <KShortcutsDialog>
#include <KTreeWidgetSearchLine>

static QApplication *createApplication(int &argc, char **argv, const QString &applicationName)
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication::setOrganizationName("georgefb");
    QApplication::setApplicationName(applicationName);
    QApplication::setOrganizationDomain("georgefb.com");
    QApplication::setApplicationDisplayName("Video Player");
    QApplication::setApplicationVersion(Application::version());
    QApplication::setWindowIcon(QIcon::fromTheme("org.kde.haruna"));
    QApplication *app = new QApplication(argc, argv);
    return app;
}

Application::Application(int &argc, char **argv, const QString &applicationName)
    : m_app(createApplication(argc, argv, applicationName))
    , m_collection(this)
{
    m_config = KSharedConfig::openConfig(Global::instance()->appConfigFilePath());
    m_shortcuts = new KConfigGroup(m_config, "Shortcuts");
    m_schemes = new KColorSchemeManager(this);
    m_systemDefaultStyle = m_app->style()->objectName();

    setupUserActions();

    // register mpris dbus service
    QString mspris2Name(QStringLiteral("org.mpris.MediaPlayer2.haruna"));
    QDBusConnection::sessionBus().registerService(mspris2Name);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/org/mpris/MediaPlayer2"), this, QDBusConnection::ExportAdaptors);
    // org.mpris.MediaPlayer2 mpris2 interface
    new MediaPlayer2(this);

    if (GeneralSettings::guiStyle() != QStringLiteral("System")) {
        QApplication::setStyle(GeneralSettings::guiStyle());
    }

    // Qt sets the locale in the QGuiApplication constructor, but libmpv
    // requires the LC_NUMERIC category to be set to "C", so change it back.
    std::setlocale(LC_NUMERIC, "C");

    setupWorkerThread();
    setupAboutData();
    setupCommandLineParser();
    registerQmlTypes();
    setupQmlSettingsTypes();

    m_engine = new QQmlApplicationEngine(this);
    const QUrl url(QStringLiteral("qrc:/qml/main.qml"));
    auto onObjectCreated = [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl) {
            QCoreApplication::exit(-1);
        }
    };
    QObject::connect(m_engine, &QQmlApplicationEngine::objectCreated,
                     m_app, onObjectCreated, Qt::QueuedConnection);
    m_engine->addImportPath("qrc:/qml");
    m_engine->addImageProvider("thumbnail", new ThumbnailImageProvider());
    setupQmlContextProperties();
    m_engine->load(url);
}

Application::~Application()
{
    delete m_engine;
}

int Application::run()
{
    return m_app->exec();
}

void Application::setupWorkerThread()
{
    auto worker = Worker::instance();
    auto thread = new QThread();
    worker->moveToThread(thread);
    QObject::connect(thread, &QThread::finished,
                     worker, &Worker::deleteLater);
    QObject::connect(thread, &QThread::finished,
                     thread, &QThread::deleteLater);
    thread->start();
}

void Application::setupAboutData()
{
    m_aboutData = KAboutData(QStringLiteral("haruna"),
                             tr("Video Player"),
                             Application::version());
    m_aboutData.setShortDescription(tr("A configurable video player."));
    m_aboutData.setLicense(KAboutLicense::GPL_V3);
    m_aboutData.setCopyrightStatement(tr("(c) 2019-2021"));
    m_aboutData.setHomepage(QStringLiteral("https://invent.kde.org/multimedia/haruna"));
    m_aboutData.setBugAddress(QStringLiteral("https://bugs.kde.org").toUtf8());
    m_aboutData.setDesktopFileName("org.kde.haruna");

    m_aboutData.addAuthor(tr("George Florea Bănuș"),
                        tr("Developer"),
                        QStringLiteral("georgefb899@gmail.com"),
                        QStringLiteral("https://georgefb.com"));

    KAboutData::setApplicationData(m_aboutData);
}

void Application::setupCommandLineParser()
{
    QCommandLineParser parser;
    m_aboutData.setupCommandLine(&parser);
    parser.addPositionalArgument(QStringLiteral("file"), tr("File to open"));
    parser.process(*m_app);
    m_aboutData.processCommandLine(&parser);

    for (auto i = 0; i < parser.positionalArguments().size(); ++i) {
        addArgument(i, parser.positionalArguments().at(i));
    }
}

void Application::registerQmlTypes()
{
    qmlRegisterType<MpvObject>("mpv", 1, 0, "MpvObject");
    qRegisterMetaType<PlayListModel*>();
    qRegisterMetaType<PlayListItem*>();
    qRegisterMetaType<QAction*>();
    qRegisterMetaType<TracksModel*>();
    qRegisterMetaType<KFileMetaData::PropertyMap>("KFileMetaData::PropertyMap");
}

void Application::setupQmlSettingsTypes()
{
    auto audioProvider = [](QQmlEngine *, QJSEngine *) -> QObject * { return AudioSettings::self(); };
    qmlRegisterSingletonType<AudioSettings>("org.kde.haruna", 1, 0, "AudioSettings", audioProvider);

    auto generalProvider = [](QQmlEngine *, QJSEngine *) -> QObject * { return GeneralSettings::self(); };
    qmlRegisterSingletonType<GeneralSettings>("org.kde.haruna", 1, 0, "GeneralSettings", generalProvider);

    auto mouseProvider = [](QQmlEngine *, QJSEngine *) -> QObject * { return MouseSettings::self(); };
    qmlRegisterSingletonType<MouseSettings>("org.kde.haruna", 1, 0, "MouseSettings", mouseProvider);

    auto playbackProvider = [](QQmlEngine *, QJSEngine *) -> QObject * { return PlaybackSettings::self(); };
    qmlRegisterSingletonType<PlaybackSettings>("org.kde.haruna", 1, 0, "PlaybackSettings", playbackProvider);

    auto playlistProvider = [](QQmlEngine *, QJSEngine *) -> QObject * { return PlaylistSettings::self(); };
    qmlRegisterSingletonType<PlaylistSettings>("org.kde.haruna", 1, 0, "PlaylistSettings", playlistProvider);

    auto subtitlesProvider = [](QQmlEngine *, QJSEngine *) -> QObject * { return SubtitlesSettings::self(); };
    qmlRegisterSingletonType<SubtitlesSettings>("org.kde.haruna", 1, 0, "SubtitlesSettings", subtitlesProvider);

    auto videoProvider = [](QQmlEngine *, QJSEngine *) -> QObject * { return VideoSettings::self(); };
    qmlRegisterSingletonType<VideoSettings>("org.kde.haruna", 1, 0, "VideoSettings", videoProvider);
}

void Application::setupQmlContextProperties()
{
    std::unique_ptr<LockManager> lockManager = std::make_unique<LockManager>();
    std::unique_ptr<SubtitlesFoldersModel> subsFoldersModel = std::make_unique<SubtitlesFoldersModel>();

    m_engine->rootContext()->setContextProperty(QStringLiteral("app"), this);
    qmlRegisterUncreatableType<Application>("Application", 1, 0, "Application",
                                            QStringLiteral("Application should not be created in QML"));

    m_engine->rootContext()->setContextProperty(QStringLiteral("mediaPlayer2Player"), new MediaPlayer2Player(this));

    m_engine->rootContext()->setContextProperty(QStringLiteral("lockManager"), lockManager.release());
    qmlRegisterUncreatableType<LockManager>("LockManager", 1, 0, "LockManager",
                                            QStringLiteral("LockManager should not be created in QML"));

    m_engine->rootContext()->setContextProperty(QStringLiteral("subsFoldersModel"), subsFoldersModel.release());

    auto customCommandsModel = new CustomCommandsModel();
    m_engine->rootContext()->setContextProperty(QStringLiteral("customCommandsModel"), customCommandsModel);
    auto proxyCustomCommandsModel = new ProxyCustomCommandsModel();
    proxyCustomCommandsModel->setSourceModel(customCommandsModel);
    m_engine->rootContext()->setContextProperty(QStringLiteral("proxyCustomCommandsModel"), proxyCustomCommandsModel);
}

QUrl Application::configFilePath()
{
    auto configPath = QStandardPaths::writableLocation(m_config->locationType());
    auto configFilePath = configPath.append(QStringLiteral("/")).append(m_config->name());
    QUrl url(configFilePath);
    url.setScheme("file");
    return url;
}

QUrl Application::configFolderPath()
{
    auto configPath = QStandardPaths::writableLocation(m_config->locationType());
    auto configFilePath = configPath.append(QStringLiteral("/")).append(m_config->name());
    QFileInfo fileInfo(configFilePath);
    QUrl url(fileInfo.absolutePath());
    url.setScheme("file");
    return url;
}

QString Application::version()
{
    return QStringLiteral("0.6.3");
}

bool Application::hasYoutubeDl()
{
    return !QStandardPaths::findExecutable(QStringLiteral("youtube-dl")).isEmpty();

}

QUrl Application::parentUrl(const QString &path)
{
    QUrl url(path);
    if (!url.isValid()) {
        return QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    }
    QFileInfo fileInfo;
    if (url.isLocalFile()) {
        fileInfo.setFile(url.toLocalFile());
    } else {
        fileInfo.setFile(url.toString());
    }
    QUrl parentFolderUrl(fileInfo.absolutePath());
    parentFolderUrl.setScheme("file");

    return parentFolderUrl;
}

QUrl Application::pathToUrl(const QString &path)
{
    QUrl url(path);
    if (!url.isValid()) {
        return QUrl();
    }
    url.setScheme("file");

    return url;
}

bool Application::isYoutubePlaylist(const QString &path)
{
    return path.contains("youtube.com/playlist?list");
}

QString Application::formatTime(const double time)
{
    QTime t(0, 0, 0);
    QString formattedTime = t.addSecs(static_cast<qint64>(time)).toString("hh:mm:ss");
    return formattedTime;
}

void Application::hideCursor()
{
    QApplication::setOverrideCursor(Qt::BlankCursor);
}

void Application::showCursor()
{
    QApplication::setOverrideCursor(Qt::ArrowCursor);
}

QString Application::argument(int key)
{
    return m_args[key];
}

void Application::addArgument(int key, const QString &value)
{
    m_args.insert(key, value);
}

QAction *Application::action(const QString &name)
{
    auto resultAction = m_collection.action(name);

    if (!resultAction) {
        setupActions(name);
        resultAction = m_collection.action(name);
    }

    return resultAction;
}

QString Application::getFileContent(const QString &file)
{
    QFile f(file);
    f.open(QIODevice::ReadOnly);
    QString content = f.readAll();
    f.close();
    return content;
}

QString Application::mimeType(const QString &file)
{
    QMimeDatabase db;
    QMimeType mime = db.mimeTypeForFile(file);
    return mime.name();
}

QStringList Application::availableGuiStyles()
{
    return QStyleFactory::keys();
}

void Application::setGuiStyle(const QString &style)
{
    if (style == "Default") {
        QApplication::setStyle(m_systemDefaultStyle);
        return;
    }
    QApplication::setStyle(style);
}

QAbstractItemModel *Application::colorSchemesModel()
{
    return m_schemes->model();
}

void Application::activateColorScheme(const QString &name)
{
    m_schemes->activateScheme(m_schemes->indexForScheme(name));
}

void Application::configureShortcuts(const QString &name)
{
    KShortcutsDialog dlg(KShortcutsEditor::ApplicationAction, KShortcutsEditor::LetterShortcutsAllowed, nullptr);
    connect(&dlg, &KShortcutsDialog::accepted, this, [ = ](){
        m_collection.writeSettings(m_shortcuts);
        m_config->sync();
    });

    if (!name.isEmpty()) {
        auto searchLine = dlg.findChild<KTreeWidgetSearchLine *>();
        if (searchLine != nullptr) {
            searchLine->setText(name);
        }
    }

    dlg.setModal(true);
    dlg.addCollection(&m_collection);
    dlg.configure(false);
}

void Application::aboutApplication()
{
    static QPointer<QDialog> dialog;
    if (!dialog) {
        dialog = new KAboutApplicationDialog(KAboutData::applicationData(), nullptr);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    dialog->show();
}

void Application::setupActions(const QString &actionName)
{
    if (actionName == QStringLiteral("screenshot")) {
        auto action = new HAction();
        action->setText(tr("Screenshot"));
        action->setIcon(QIcon::fromTheme("image-x-generic"));
        m_collection.setDefaultShortcut(action, Qt::Key_S);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("file_quit")) {
        auto action = new HAction();
        action->setText(tr("Quit"));
        action->setIcon(QIcon::fromTheme("application-exit"));
        connect(action, &QAction::triggered, m_app, &QApplication::quit);
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::Key_Q);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("options_configure_keybinding")) {
        auto action = new HAction();
        action->setText(tr("Configure Keyboard Shortcuts"));
        action->setIcon(QIcon::fromTheme("configure-shortcuts"));
        connect(action, &QAction::triggered, this, [=]() { configureShortcuts(); });
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::SHIFT + Qt::Key_S);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("configure")) {
        auto action = new HAction();
        action->setText(tr("Configure"));
        action->setIcon(QIcon::fromTheme("configure"));
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::SHIFT + Qt::Key_Comma);
        m_collection.addAction(actionName, action);
    }

    if (actionName == QStringLiteral("togglePlaylist")) {
        auto action = new HAction();
        action->setText(tr("Playlist"));
        action->setIcon(QIcon::fromTheme("view-media-playlist"));
        m_collection.setDefaultShortcut(action, Qt::Key_P);
        m_collection.addAction(actionName, action);
    }

    if (actionName == QStringLiteral("openContextMenu")) {
        auto action = new HAction();
        action->setText(tr("Open Context Menu"));
        action->setIcon(QIcon::fromTheme("application-menu"));
        m_collection.setDefaultShortcut(action, Qt::Key_Menu);
        m_collection.addAction(actionName, action);
    }

    if (actionName == QStringLiteral("toggleFullscreen")) {
        auto action = new HAction();
        action->setText(tr("Toggle Fullscreen"));
        action->setIcon(QIcon::fromTheme("view-fullscreen"));
        m_collection.setDefaultShortcut(action, Qt::Key_F);
        m_collection.addAction(actionName, action);
    }

    if (actionName == QStringLiteral("openFile")) {
        auto action = new HAction();
        action->setText(tr("Open File"));
        action->setIcon(QIcon::fromTheme("folder-videos"));
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::Key_O);
        m_collection.addAction(actionName, action);
    }

    if (actionName == QStringLiteral("openUrl")) {
        auto action = new HAction();
        action->setText(tr("Open Url"));
        action->setIcon(QIcon::fromTheme("internet-services"));
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::SHIFT + Qt::Key_O);
        m_collection.addAction(actionName, action);
    }

    if (actionName == QStringLiteral("aboutHaruna")) {
        auto action = new HAction();
        action->setText(tr("About Haruna"));
        action->setIcon(QIcon::fromTheme("help-about"));
        m_collection.setDefaultShortcut(action, Qt::Key_F1);
        m_collection.addAction(actionName, action);
        connect(action, &QAction::triggered, this, &Application::aboutApplication);
    }

    // mpv actions
    if (actionName == QStringLiteral("contrastUp")) {
        auto action = new HAction();
        action->setText(tr("Contrast Up"));
        action->setIcon(QIcon::fromTheme("contrast"));
        m_collection.setDefaultShortcut(action, Qt::Key_1);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("contrastDown")) {
        auto action = new HAction();
        action->setText(tr("Contrast Down"));
        action->setIcon(QIcon::fromTheme("contrast"));
        m_collection.setDefaultShortcut(action, Qt::Key_2);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("contrastReset")) {
        auto action = new HAction();
        action->setText(tr("Contrast Reset"));
        action->setIcon(QIcon::fromTheme("contrast"));
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::Key_1);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("brightnessUp")) {
        auto action = new HAction();
        action->setText(tr("Brightness Up"));
        action->setIcon(QIcon::fromTheme("contrast"));
        m_collection.setDefaultShortcut(action, Qt::Key_3);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("brightnessDown")) {
        auto action = new HAction();
        action->setText(tr("Brightness Down"));
        action->setIcon(QIcon::fromTheme("contrast"));
        m_collection.setDefaultShortcut(action, Qt::Key_4);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("brightnessReset")) {
        auto action = new HAction();
        action->setText(tr("Brightness Reset"));
        action->setIcon(QIcon::fromTheme("contrast"));
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::Key_3);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("gammaUp")) {
        auto action = new HAction();
        action->setText(tr("Gamma Up"));
        action->setIcon(QIcon::fromTheme("contrast"));
        m_collection.setDefaultShortcut(action, Qt::Key_5);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("gammaDown")) {
        auto action = new HAction();
        action->setText(tr("Gamma Down"));
        action->setIcon(QIcon::fromTheme("contrast"));
        m_collection.setDefaultShortcut(action, Qt::Key_6);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("gammaReset")) {
        auto action = new HAction();
        action->setText(tr("Gamma Reset"));
        action->setIcon(QIcon::fromTheme("contrast"));
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::Key_5);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("saturationUp")) {
        auto action = new HAction();
        action->setText(tr("Saturation Up"));
        action->setIcon(QIcon::fromTheme("contrast"));
        m_collection.setDefaultShortcut(action, Qt::Key_7);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("saturationDown")) {
        auto action = new HAction();
        action->setText(tr("Saturation Down"));
        action->setIcon(QIcon::fromTheme("contrast"));
        m_collection.setDefaultShortcut(action, Qt::Key_8);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("saturationReset")) {
        auto action = new HAction();
        action->setText(tr("Saturation Reset"));
        action->setIcon(QIcon::fromTheme("contrast"));
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::Key_7);
        m_collection.addAction(actionName, action);
    }

    if (actionName == QStringLiteral("playNext")) {
        auto action = new HAction();
        action->setText(tr("Play Next"));
        action->setIcon(QIcon::fromTheme("media-skip-forward"));
        m_collection.setDefaultShortcut(action, Qt::SHIFT + Qt::Key_Period);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("playPrevious")) {
        auto action = new HAction();
        action->setText(tr("Play Previous"));
        action->setIcon(QIcon::fromTheme("media-skip-backward"));
        m_collection.setDefaultShortcut(action, Qt::SHIFT + Qt::Key_Comma);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("volumeUp")) {
        auto action = new HAction();
        action->setText(tr("Volume Up"));
        action->setIcon(QIcon::fromTheme("audio-volume-high"));
        m_collection.setDefaultShortcut(action, Qt::Key_9);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("volumeDown")) {
        auto action = new HAction();
        action->setText(tr("Volume Down"));
        action->setIcon(QIcon::fromTheme("audio-volume-low"));
        m_collection.setDefaultShortcut(action, Qt::Key_0);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("mute")) {
        auto action = new HAction();
        action->setText(tr("Mute"));
        action->setIcon(QIcon::fromTheme("player-volume"));
        m_collection.setDefaultShortcut(action, Qt::Key_M);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("seekForwardSmall")) {
        auto action = new HAction();
        action->setText(tr("Seek Small Step Forward"));
        action->setIcon(QIcon::fromTheme("media-seek-forward"));
        m_collection.setDefaultShortcut(action, Qt::Key_Right);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("seekBackwardSmall")) {
        auto action = new HAction();
        action->setText(tr("Seek Small Step Backward"));
        action->setIcon(QIcon::fromTheme("media-seek-backward"));
        m_collection.setDefaultShortcut(action, Qt::Key_Left);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("seekForwardMedium")) {
        auto action = new HAction();
        action->setText(tr("Seek Medium Step Forward"));
        action->setIcon(QIcon::fromTheme("media-seek-forward"));
        m_collection.setDefaultShortcut(action, Qt::SHIFT + Qt::Key_Right);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("seekBackwardMedium")) {
        auto action = new HAction();
        action->setText(tr("Seek Medium Step Backward"));
        action->setIcon(QIcon::fromTheme("media-seek-backward"));
        m_collection.setDefaultShortcut(action, Qt::SHIFT + Qt::Key_Left);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("seekForwardBig")) {
        auto action = new HAction();
        action->setText(tr("Seek Big Step Forward"));
        action->setIcon(QIcon::fromTheme("media-seek-forward"));
        m_collection.setDefaultShortcut(action, Qt::Key_Up);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("seekBackwardBig")) {
        auto action = new HAction();
        action->setText(tr("Seek Big Step Backward"));
        action->setIcon(QIcon::fromTheme("media-seek-backward"));
        m_collection.setDefaultShortcut(action, Qt::Key_Down);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("seekPreviousChapter")) {
        auto action = new HAction();
        action->setText(tr("Seek Previous Chapter"));
        action->setIcon(QIcon::fromTheme("media-seek-backward"));
        m_collection.setDefaultShortcut(action, Qt::Key_PageDown);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("seekNextChapter")) {
        auto action = new HAction();
        action->setText(tr("Seek Next Chapter"));
        action->setIcon(QIcon::fromTheme("media-seek-forward"));
        m_collection.setDefaultShortcut(action, Qt::Key_PageUp);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("seekNextSubtitle")) {
        auto action = new HAction();
        action->setText(tr("Seek To Next Subtitle"));
        action->setIcon(QIcon::fromTheme("media-seek-forward"));
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::Key_Right);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("seekPreviousSubtitle")) {
        auto action = new HAction();
        action->setText(tr("Seek To Previous Subtitle"));
        action->setIcon(QIcon::fromTheme("media-seek-backward"));
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::Key_Left);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("frameStep")) {
        auto action = new HAction();
        action->setText(tr("Move one frame forward, then pause"));
        m_collection.setDefaultShortcut(action, Qt::Key_Period);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("frameBackStep")) {
        auto action = new HAction();
        action->setText(tr("Move one frame backward, then pause"));
        m_collection.setDefaultShortcut(action, Qt::Key_Comma);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("increasePlayBackSpeed")) {
        auto action = new HAction();
        action->setText(tr("Playback speed increase"));
        m_collection.setDefaultShortcut(action, Qt::Key_BracketRight);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("decreasePlayBackSpeed")) {
        auto action = new HAction();
        action->setText(tr("Playback speed decrease"));
        m_collection.setDefaultShortcut(action, Qt::Key_BracketLeft);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("resetPlayBackSpeed")) {
        auto action = new HAction();
        action->setText(tr("Playback speed reset"));
        m_collection.setDefaultShortcut(action, Qt::Key_Backspace);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("subtitleQuicken")) {
        auto action = new HAction();
        action->setText(tr("Subtitle Quicken"));
        m_collection.setDefaultShortcut(action, Qt::Key_Z);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("subtitleDelay")) {
        auto action = new HAction();
        action->setText(tr("Subtitle Delay"));
        m_collection.setDefaultShortcut(action, Qt::SHIFT + Qt::Key_Z);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("subtitleToggle")) {
        auto action = new HAction();
        action->setText(tr("Subtitle Toggle"));
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::Key_S);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("audioCycleUp")) {
        auto action = new HAction();
        action->setText(tr("Cycle Audio Up"));
        m_collection.setDefaultShortcut(action, Qt::SHIFT + Qt::Key_3);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("audioCycleDown")) {
        auto action = new HAction();
        action->setText(tr("Cycle Audio Down"));
        m_collection.setDefaultShortcut(action, Qt::SHIFT + Qt::Key_2);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("subtitleCycleUp")) {
        auto action = new HAction();
        action->setText(tr("Cycle Subtitle Up"));
        m_collection.setDefaultShortcut(action, Qt::Key_J);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("subtitleCycleDown")) {
        auto action = new HAction();
        action->setText(tr("Cycle Subtitle Down"));
        m_collection.setDefaultShortcut(action, Qt::SHIFT + Qt::Key_J);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("zoomIn")) {
        auto action = new HAction();
        action->setText(tr("Zoom In"));
        action->setIcon(QIcon::fromTheme("zoom-in"));
        m_collection.setDefaultShortcut(action, Qt::ALT + Qt::Key_Plus);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("zoomOut")) {
        auto action = new HAction();
        action->setText(tr("Zoom Out"));
        action->setIcon(QIcon::fromTheme("zoom-out"));
        m_collection.setDefaultShortcut(action, Qt::ALT + Qt::Key_Minus);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("zoomReset")) {
        auto action = new HAction();
        action->setText(tr("Zoom Reset"));
        action->setIcon(QIcon::fromTheme("zoom-original"));
        m_collection.setDefaultShortcut(action, Qt::ALT + Qt::Key_Backspace);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("videoPanXLeft")) {
        auto action = new HAction();
        action->setText(tr("Video pan x left"));
        m_collection.setDefaultShortcut(action, Qt::ALT + Qt::Key_Left);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("videoPanXRight")) {
        auto action = new HAction();
        action->setText(tr("Video pan x right"));
        m_collection.setDefaultShortcut(action, Qt::ALT + Qt::Key_Right);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("videoPanYUp")) {
        auto action = new HAction();
        action->setText(tr("Video pan y up"));
        m_collection.setDefaultShortcut(action, Qt::ALT + Qt::Key_Up);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("videoPanYDown")) {
        auto action = new HAction();
        action->setText(tr("Video pan y down"));
        m_collection.setDefaultShortcut(action, Qt::ALT + Qt::Key_Down);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("toggleMenuBar")) {
        auto action = new HAction();
        action->setText(tr("Toggle Menu Bar"));
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::Key_M);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("toggleHeader")) {
        auto action = new HAction();
        action->setText(tr("Toggle Header"));
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::Key_H);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("setLoop")) {
        auto action = new HAction();
        action->setText(tr("Set Loop"));
        m_collection.setDefaultShortcut(action, Qt::Key_L);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("increaseSubtitleFontSize")) {
        auto action = new HAction();
        action->setText(tr("Increase Subtitle Font Size"));
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::Key_Z);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("decreaseSubtitleFontSize")) {
        auto action = new HAction();
        action->setText(tr("Decrease Subtitle Font Size"));
        m_collection.setDefaultShortcut(action, Qt::CTRL + Qt::Key_X);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("subtitlePositionUp")) {
        auto action = new HAction();
        action->setText(tr("Move Subtitle Up"));
        m_collection.setDefaultShortcut(action, Qt::Key_R);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("subtitlePositionDown")) {
        auto action = new HAction();
        action->setText(tr("Move Subtitle Down"));
        m_collection.setDefaultShortcut(action, Qt::SHIFT + Qt::Key_R);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("toggleDeinterlacing")) {
        auto action = new HAction();
        action->setText(tr("Toggle deinterlacing"));
        m_collection.setDefaultShortcut(action, Qt::Key_D);
        m_collection.addAction(actionName, action);
    }
    if (actionName == QStringLiteral("exitFullscreen")) {
        auto action = new HAction();
        action->setText(tr("Exit Fullscreen"));
        m_collection.setDefaultShortcut(action, Qt::Key_Escape);
        m_collection.addAction(actionName, action);
    }
    m_collection.readSettings(m_shortcuts);
}

void Application::createUserAction(const QString &text)
{
    KSharedConfig::Ptr m_customCommandsConfig;
    QString ccConfig = Global::instance()->appConfigFilePath(Global::ConfigFile::CustomCommands);
    m_customCommandsConfig = KSharedConfig::openConfig(ccConfig, KConfig::SimpleConfig);
    int counter = m_customCommandsConfig->group(QString()).readEntry("Counter", 0);
    const QString &name = QString("Command_%1").arg(counter);

    auto action = new HAction();
    action->setText(text);
    m_collection.addAction(name, action);
    m_collection.readSettings(m_shortcuts);
}

void Application::openDocs(const QString &page)
{
    QDesktopServices::openUrl(QUrl(page));
}

void Application::setupUserActions()
{
    KSharedConfig::Ptr m_customCommandsConfig;
    QString ccConfig = Global::instance()->appConfigFilePath(Global::ConfigFile::CustomCommands);
    m_customCommandsConfig = KSharedConfig::openConfig(ccConfig, KConfig::SimpleConfig);
    QStringList groups = m_customCommandsConfig->groupList();
    for (const QString &_group : qAsConst((groups))) {
        auto configGroup = m_customCommandsConfig->group(_group);
        QString command = configGroup.readEntry("Command", QString());

        if (configGroup.readEntry("Type", QString()) == "shortcut") {
            auto action = new HAction();
            action->setText(command);
            m_collection.addAction(_group, action);
        }
    }
    m_collection.readSettings(m_shortcuts);
}
