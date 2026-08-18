#pragma once

#include <QDialog>

namespace proto {
class CompanionClient;
}

namespace model {
class ContactModel;
}

class QLabel;
class QListView;

// Everyone the node has heard an advert from. Its own window rather than a
// second column: at 480 rows there is nowhere to put one, and this is a list to
// consult rather than one to keep on screen.
//
// It follows the link while it is open -- adverts arrive all day, and a list
// that were only correct at the moment it opened would be quietly wrong the
// longer it were left up.
class ContactsDialog : public QDialog {
    Q_OBJECT

public:
    explicit ContactsDialog(proto::CompanionClient* client, QWidget* parent = nullptr);

private:
    // An empty address book and a link that has not answered yet look identical
    // on screen, so the placeholder says which it is.
    void updatePlaceholder();

    proto::CompanionClient* client_ = nullptr;
    model::ContactModel* model_ = nullptr;
    QListView* list_ = nullptr;
    QLabel* placeholder_ = nullptr;
};
