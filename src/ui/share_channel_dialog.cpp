#include "ui/share_channel_dialog.h"

#include <QAction>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

#include "ui/dialog_settings.h"
#include "ui/icons.h"
#include "ui/theme.h"

ShareChannelDialog::ShareChannelDialog(const model::Channel& channel, QWidget* parent)
    : QDialog(parent) {
    ui::configureDialogWindow(*this);
    setWindowTitle(QStringLiteral("Share channel"));

    // Hex, which is what the daemon writes down and what the Join tab of the
    // add dialog takes back.
    const QString hex = QString::fromLatin1(channel.secret.toHex());

    auto* title = new QLabel(QStringLiteral("<b>%1</b>").arg(channel.displayName().toHtmlEscaped()));

    auto* key = new QLineEdit(hex);
    key->setReadOnly(true);
    key->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    key->setCursorPosition(0);

    const int copyIconSize = theme::scaled(font(), 16);
    QIcon copyIcon;
    copyIcon.addPixmap(icons::tinted(QStringLiteral("copy"), copyIconSize, theme::TextMuted,
                                    devicePixelRatioF()),
                       QIcon::Normal);
    copyIcon.addPixmap(icons::tinted(QStringLiteral("copy"), copyIconSize, theme::Text,
                                    devicePixelRatioF()),
                       QIcon::Active);
    QAction* copy = key->addAction(copyIcon, QLineEdit::TrailingPosition);
    copy->setObjectName(QStringLiteral("copyKeyAction"));
    copy->setText(QStringLiteral("Copy key"));
    copy->setToolTip(QStringLiteral("Copy key"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);
    layout->addWidget(title);
    layout->addWidget(key);
    layout->addWidget(buttons);

    connect(copy, &QAction::triggered, key,
            [key] { QGuiApplication::clipboard()->setText(key->text()); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // A 32-character key on one line.
    ui::lockDialogSize(*this, *layout, 420);
}
