#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>

// Generated into the build directory by cmake/version.cmake on every build:
// the string is git describe's, so it names the tag this was built against.
#include "version.h"

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

    // The endpoint the daemon offers unless it is told otherwise. Its default
    // path is carried by ConnectTarget so the dialog and the command line
    // cannot disagree about it.
    const proto::ConnectTarget defaults;
    QCommandLineOption socketOption(
        {QStringLiteral("s"), QStringLiteral("socket")},
        QStringLiteral("MeshCore daemon Unix socket (default %1)").arg(defaults.socketPath),
        QStringLiteral("path"), defaults.socketPath);
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
    parser.addOption(socketOption);
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

    // Three ways to name one node, and no sensible precedence between them: say
    // so rather than silently honouring whichever the code happens to test
    // first.
    const bool wantsSocket = parser.isSet(socketOption);
    const bool wantsTcp = parser.isSet(hostOption) || parser.isSet(portOption);
    const bool wantsBle = parser.isSet(bleOption);
    if (int(wantsSocket) + int(wantsTcp) + int(wantsBle) > 1) {
        qCritical("name one target: --socket, --host/--port or --ble");
        return 2;
    }

    theme::apply(app);

    // Naming a target on the command line skips the dialog: the app is launched
    // that way from the desktop file and from scripts, and both want to come up
    // connected rather than waiting on a click.
    proto::ConnectTarget target;
    if (wantsBle) {
        target = proto::bleTarget(parser.value(bleOption));
    } else if (wantsSocket) {
        target.kind = proto::ConnectTarget::Kind::Unix;
        target.socketPath = parser.value(socketOption);
        if (!target.isValid()) {
            qCritical("socket path must be absolute: %s", qPrintable(target.socketPath));
            return 2;
        }
    } else if (wantsTcp) {
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
