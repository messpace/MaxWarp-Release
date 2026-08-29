#include "SimService.h"
#include "ThemeController.h"

#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("MaxWarp"));
    app.setApplicationName(QStringLiteral("MaxWarp"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    qSetMessagePattern(QStringLiteral("[%{time HH:mm:ss.zzz}] %{message}"));
    qInfo().noquote() << "MaxWarp" << app.applicationVersion() << "starting";

    // Identity fonts (T6): Martian Mono numerals + Instrument Sans UI, bundled
    // so the design never falls back to system faces.
    const QDir fontDir(QStringLiteral(":/fonts"));
    for (const QString &file : fontDir.entryList({QStringLiteral("*.ttf")}))
        if (QFontDatabase::addApplicationFont(fontDir.filePath(file)) < 0)
            qWarning() << "font failed to load:" << file;
    app.setFont(QFont(QStringLiteral("Instrument Sans")));

    ThemeController theme;
    SimService sim;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("Sim"), &sim);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("MaxWarp", "Main");

    sim.start();
    return app.exec();
}
