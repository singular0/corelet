#include "ui/share_channel_dialog.h"

#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

ShareChannelDialog::ShareChannelDialog(const model::Channel& channel, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Share channel"));

    // Hex, which is what the daemon writes down and what the Join tab of the
    // add dialog takes back.
    const QString hex = QString::fromLatin1(channel.secret.toHex());

    auto* title = new QLabel(QStringLiteral("<b>%1</b>").arg(channel.displayName().toHtmlEscaped()));

    auto* key = new QLineEdit(hex);
    key->setReadOnly(true);
    key->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    key->setCursorPosition(0);

    auto* copy = new QPushButton(QStringLiteral("Copy"));
    copy->setAutoDefault(false);

    auto* keyRow = new QHBoxLayout;
    keyRow->setSpacing(6);
    keyRow->addWidget(key, 1);
    keyRow->addWidget(copy);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);
    layout->addWidget(title);
    layout->addLayout(keyRow);
    layout->addWidget(buttons);

    connect(copy, &QPushButton::clicked, this, [copy, hex] {
        QGuiApplication::clipboard()->setText(hex);
        // The clipboard says nothing on its own, and a key looks identical
        // before and after copying it.
        copy->setText(QStringLiteral("Copied"));
        QTimer::singleShot(1500, copy, [copy] { copy->setText(QStringLiteral("Copy")); });
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // A 32-character key on one line.
    setMinimumWidth(420);
}
