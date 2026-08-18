#include "ui/contacts_dialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListView>
#include <QVBoxLayout>

#include "model/contact_model.h"
#include "protocol/client.h"
#include "ui/contact_delegate.h"
#include "ui/dialog_settings.h"
#include "ui/theme.h"

ContactsDialog::ContactsDialog(proto::CompanionClient* client, QWidget* parent)
    : QDialog(parent), client_(client) {
    ui::configureDialogWindow(*this);
    setWindowTitle(QStringLiteral("Contacts"));

    model_ = new model::ContactModel(this);

    list_ = new QListView;
    list_->setObjectName(QStringLiteral("contactList"));
    list_->setModel(model_);
    list_->setItemDelegate(new ContactDelegate(list_));
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Nothing can be done to a contact yet, so a row that highlighted under the
    // pointer would be offering something that is not there.
    list_->setSelectionMode(QAbstractItemView::NoSelection);
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // Every row is the same three lines tall, which is what lets the view size
    // the list from one of them instead of all hundred.
    list_->setUniformItemSizes(true);
    list_->setResizeMode(QListView::Adjust);

    placeholder_ = new QLabel;
    placeholder_->setWordWrap(true);
    placeholder_->setAlignment(Qt::AlignCenter);
    placeholder_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::TextMuted.name()));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);
    layout->addWidget(placeholder_, 1);
    layout->addWidget(list_, 1);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(client_, &proto::CompanionClient::contactsChanged, this,
            [this](const QVector<model::Contact>& contacts) {
                model_->setContacts(contacts);
                updatePlaceholder();
            });
    connect(client_, &proto::CompanionClient::contactChanged, this,
            [this](const model::Contact& contact) {
                model_->upsert(contact);
                updatePlaceholder();
            });
    // The link can go down with this window still up: "nothing heard yet" and
    // "not connected" are very different things to be told about an empty list.
    connect(client_, &proto::CompanionClient::stateChanged, this,
            [this] { updatePlaceholder(); });

    model_->setContacts(client_->contacts());
    updatePlaceholder();

    // Not lockDialogSize(): a list has no content height to be fixed to, and
    // this one is worth dragging taller on a screen with the room. Short enough
    // to leave the uConsole's 480 rows somewhere to put the title bar.
    resize(640, 380);
    setMinimumSize(360, 220);
}

void ContactsDialog::updatePlaceholder() {
    const bool empty = model_->rowCount() == 0;
    list_->setVisible(!empty);
    placeholder_->setVisible(empty);
    if (!empty) return;

    placeholder_->setText(
        client_->state() == proto::CompanionClient::State::Ready
            ? QStringLiteral("Nothing heard yet. A node appears here the first time it "
                             "advertises itself within reach.")
            : QStringLiteral("Not connected. The address book lives on the node."));
}
