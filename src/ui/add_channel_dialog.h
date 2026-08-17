#pragma once

#include <QDialog>
#include <QVector>

#include "model/types.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTabWidget;
class ByteCounter;

// Adds a channel to a free slot on the device: a new private one, an existing
// private one someone shared the key for, or a public channel whose key anyone
// can derive.
//
// The three cases differ only in where the 16-byte key comes from, which is why
// they are tabs of one dialog rather than three commands: the daemon has a
// single SET_CHANNEL and no notion of channel kind at all.
class AddChannelDialog : public QDialog {
    Q_OBJECT

public:
    // `existing` is the device's channel list, for picking a free slot and for
    // catching a channel that is already joined.
    explicit AddChannelDialog(const QVector<model::Channel>& existing,
                              QWidget* parent = nullptr);

    // Valid once the dialog has been accepted. `index` is the slot it should be
    // written to.
    model::Channel channel() const { return result_; }

    // Whether there is anywhere to put a channel at all. All eight slots full
    // means the dialog has nothing to offer.
    static bool hasFreeSlot(const QVector<model::Channel>& existing);

private Q_SLOTS:
    void onAccepted();

private:
    void buildUi();
    void regenerateKey();
    void updateOkButton();
    void setError(const QString& text);
    // The channel the current tab describes, or an empty name/key when what is
    // typed is not usable yet.
    model::Channel currentChannel() const;

    QVector<model::Channel> existing_;
    model::Channel result_;
    int freeSlot_ = -1;

    QTabWidget* tabs_ = nullptr;
    QLineEdit* createName_ = nullptr;
    ByteCounter* createNameCount_ = nullptr;
    QLineEdit* createKey_ = nullptr;
    QLineEdit* joinName_ = nullptr;
    ByteCounter* joinNameCount_ = nullptr;
    QLineEdit* joinKey_ = nullptr;
    QRadioButton* publicWellKnown_ = nullptr;
    QRadioButton* publicHashtag_ = nullptr;
    QLineEdit* hashtag_ = nullptr;
    ByteCounter* hashtagCount_ = nullptr;
    QLabel* error_ = nullptr;
    QPushButton* addButton_ = nullptr;
};
