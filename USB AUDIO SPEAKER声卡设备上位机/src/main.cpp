#include "audiocontroller.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTimer>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("STM32 Audio Lab"));
    QCoreApplication::setApplicationName(QStringLiteral("USB Audio Speaker 控制台"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    AudioController audioController;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("audio"), &audioController);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("USBAUDIOSPEAKER", "Main");

    // 无界面冒烟测试：验证 QML、Qt Multimedia 与控制器均能正常初始化。
    if (QCoreApplication::arguments().contains(QStringLiteral("--smoke-test")))
        QTimer::singleShot(1200, &app, &QCoreApplication::quit);

    return QGuiApplication::exec();
}
