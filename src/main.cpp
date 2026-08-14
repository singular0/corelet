#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>

#include "protocol/ble_transport.h"
#include "protocol/transport.h"
#include "ui/connect_dialog.h"
#include "ui/main_window.h"
#include "ui/theme.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("corelet"));
    // There is no organization, but leaving this unset is not an option: Qt files
    // the settings under com.trolltech.unknown-organization rather than skipping
    // the level. The author's handle is the honest answer and keeps the settings
    // domain in step with the bundle identifier in CMakeLists.txt.
    QApplication::setOrganizationName(QStringLiteral("singular0"));
    QApplication::setApplicationVersion(QStringLiteral(CORELET_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("MeshCore companion app for the ClockworkPi uConsole.\n"
                       "Talks to a MeshCore daemon over the companion protocol, or\n"
                       "to a MeshCore device over Bluetooth LE."));
    parser.addHelpOption();
    parser.addVersionOption();

    // The daemon binds loopback by default, and its protocol has no
    // authentication, so a local default is the only safe one.
    QCommandLineOption hostOption({QStringLiteral("H"), QStringLiteral("host")},
                                  QStringLiteral("MeshCore daemon host (default 127.0.0.1)"),
                                  QStringLiteral("host"), QStringLiteral("127.0.0.1"));
    QCommandLineOption portOption({QStringLiteral("p"), QStringLiteral("port")},
                                  QStringLiteral("MeshCore daemon port (default 5000)"),
                                  QStringLiteral("port"), QStringLiteral("5000"));
    QCommandLineOption bleOption(
        {QStringLiteral("b"), QStringLiteral("ble")},
        QStringLiteral("MeshCore device to reach over Bluetooth LE, by advertised name "
                       "or by the address this machine knows it as"),
        QStringLiteral("device"));
    parser.addOption(hostOption);
    parser.addOption(portOption);
    parser.addOption(bleOption);
    parser.process(app);

    bool portOk = false;
    const uint port = parser.value(portOption).toUInt(&portOk);
    if (!portOk || port == 0 || port > 65535) {
        qCritical("invalid port: %s", qPrintable(parser.value(portOption)));
        return 2;
    }

    theme::apply(app);

    // Naming a target on the command line skips the dialog: the app is launched
    // that way from the desktop file and from scripts, and both want to come up
    // connected rather than waiting on a click.
    proto::ConnectTarget target;
    if (parser.isSet(bleOption)) {
        target = proto::bleTarget(parser.value(bleOption));
    } else if (parser.isSet(hostOption) || parser.isSet(portOption)) {
        target.host = parser.value(hostOption);
        target.port = quint16(port);
    } else {
        ConnectDialog dialog;
        if (dialog.exec() != QDialog::Accepted) return 0;
        target = dialog.target();
    }

    MainWindow window(target);
    window.show();
    return app.exec();
}
