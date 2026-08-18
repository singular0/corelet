#pragma once

#include <QByteArray>
#include <QDialog>

namespace proto {
class CompanionClient;
}

namespace model {
class ContactModel;
}

class QLabel;
class QListView;
class QPushButton;

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

    // The same list, asked to choose somebody: who a new direct conversation is
    // with. Returns their public key, or nothing when the window was closed
    // without a choice. Deliberately not a second dialog -- picking a node and
    // looking one up want the same rows, the same type filters and the same
    // sorting, and two of them would drift apart.
    static QByteArray pickContact(proto::CompanionClient* client, QWidget* parent);

private:
    enum class Mode {
        Browse,  // nothing can be done to a row
        Pick,    // one row is the answer
    };

    ContactsDialog(proto::CompanionClient* client, Mode mode, QWidget* parent);

    // An empty address book and a link that has not answered yet look identical
    // on screen, so the placeholder says which it is.
    void updatePlaceholder();
    // In Pick mode the accept button means "this one", so it stays disabled
    // until there is a one to mean.
    void updateChoice();
    QByteArray chosenKey() const;

    proto::CompanionClient* client_ = nullptr;
    Mode mode_ = Mode::Browse;
    model::ContactModel* model_ = nullptr;
    QListView* list_ = nullptr;
    QLabel* placeholder_ = nullptr;
    QPushButton* accept_ = nullptr;
};
