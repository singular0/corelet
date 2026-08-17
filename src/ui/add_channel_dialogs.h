#pragma once

#include <QDialog>
#include <QVector>

#include "model/types.h"

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;

// Adding a channel is one command — the daemon has a single SET_CHANNEL and no
// notion of channel kind at all — but four ways of arriving at the 16-byte key
// it writes, and they have almost nothing to fill in in common. Each is its own
// dialog behind its own item of the add menu rather than a tab of one dialog:
// the choice is made before anything is typed, so it belongs to the menu, and
// what is left is a form of one or two fields. The public channel needs no
// dialog at all — its key is a constant — and MainWindow adds `publicChannel()`
// straight from the menu.
//
// This base holds what all of them do share: the slot to write into, the checks
// that a key is usable and not already joined, and the error line and accept
// button those drive.
class AddChannelDialog : public QDialog {
    Q_OBJECT

public:
    // Valid once the dialog has been accepted. `index` is the slot it should be
    // written to.
    model::Channel channel() const { return result_; }

    // Whether there is anywhere to put a channel at all. All eight slots full
    // means every way of adding one has nothing to offer.
    static bool hasFreeSlot(const QVector<model::Channel>& existing);

    // Whether the well-known public key is already in one of the slots, which is
    // what disables the menu item that would join it again.
    static bool publicChannelJoined(const QVector<model::Channel>& existing);

    // The public channel as it would be written to the first free slot, or an
    // unconfigured channel when there is none.
    static model::Channel publicChannel(const QVector<model::Channel>& existing);

protected:
    // `acceptText` names the button in the verb of the dialog — creating a
    // channel and joining one are not the same act, and "Add" says neither.
    AddChannelDialog(const QVector<model::Channel>& existing, const QString& title,
                     const QString& acceptText, QWidget* parent);

    // Called by a subclass once its fields exist: puts `body` above the shared
    // error line and buttons and fixes the dialog at `width`.
    void buildFrame(QWidget* body, int width);
    // Re-runs the checks against what is on screen now. A subclass connects
    // every field it owns to this, after that field's ByteLimit so this reads
    // text already held inside its budget.
    void revalidate();

    // The channel the fields describe, with an empty name or key while what is
    // typed does not name one yet.
    virtual model::Channel describedChannel() const = 0;
    // What is wrong with what is typed, when the form is incomplete *because* of
    // a mistake — a key that is not a key. An unfinished form is not a mistake
    // worth complaining about, so this is empty by default.
    virtual QString inputError() const { return {}; }

    // The slot a new channel goes to, or -1 when all eight are taken.
    int freeSlot() const { return freeSlot_; }

private Q_SLOTS:
    void onAccepted();

private:
    void setError(const QString& text);

    QVector<model::Channel> existing_;
    model::Channel result_;
    int freeSlot_ = -1;

    QLabel* error_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    QPushButton* acceptButton_ = nullptr;
};

// A private channel of one's own. The key is generated here, and the only way
// anyone else gets into the channel is being handed a copy of it.
class CreatePrivateChannelDialog : public AddChannelDialog {
    Q_OBJECT

public:
    explicit CreatePrivateChannelDialog(const QVector<model::Channel>& existing,
                                        QWidget* parent = nullptr);

protected:
    model::Channel describedChannel() const override;

private:
    void regenerateKey();

    QLineEdit* name_ = nullptr;
    QLineEdit* key_ = nullptr;
};

// The other end of that: a key someone passed on, in the hex the daemon writes
// down or the base64 the other MeshCore apps put on a QR code.
class JoinPrivateChannelDialog : public AddChannelDialog {
    Q_OBJECT

public:
    explicit JoinPrivateChannelDialog(const QVector<model::Channel>& existing,
                                      QWidget* parent = nullptr);

protected:
    model::Channel describedChannel() const override;
    QString inputError() const override;

private:
    QLineEdit* name_ = nullptr;
    QLineEdit* key_ = nullptr;
};

// A hashtag channel's key is derived from its name, so the tag is the whole
// form: knowing `#jokes` is joining it.
class JoinHashtagChannelDialog : public AddChannelDialog {
    Q_OBJECT

public:
    explicit JoinHashtagChannelDialog(const QVector<model::Channel>& existing,
                                      QWidget* parent = nullptr);

protected:
    model::Channel describedChannel() const override;

private:
    QLineEdit* tag_ = nullptr;
};
