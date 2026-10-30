#pragma once

#include <QDialog>

#include "protocol/client.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

// The node as it describes itself and as an app may change it, in one window:
// every value somebody can change here is also one they open it to read, and two
// windows showing the same name and frequency would drift. What is the node's
// alone -- its key, its battery -- sits beside the rest read-only.
//
// Each Save sends only what differs from what the node last reported, and then
// believes the node rather than the form: what comes back from the re-read is
// what the fields show. A change that did not take keeps what was typed, so it
// can be corrected rather than retyped.
class NodeConfigDialog : public QDialog {
    Q_OBJECT

public:
    NodeConfigDialog(proto::CompanionClient* client, QWidget* parent = nullptr);

    // Whether a save changed what the node's advert carries. Neither a name nor
    // a position is announced by changing it -- as in MeshCore, nothing is
    // advertised on a settings change -- so the caller offers to.
    bool advertChanged() const { return advertChanged_; }

public Q_SLOTS:
    // Not while a save is in flight: its answer is what says which of the
    // changes took, and closing would throw that away.
    void reject() override;

private:
    using DeviceInfo = proto::CompanionClient::DeviceInfo;
    using Setting = proto::CompanionClient::Setting;
    using SettingsChange = proto::CompanionClient::SettingsChange;
    using SettingsResult = proto::CompanionClient::SettingsResult;

    QLayout* buildNodeColumn();
    QLayout* buildRadioColumn();

    // Puts what the node reports into the fields, one group per command, so a
    // save that partly failed can refresh what took and leave the rest.
    void load(const DeviceInfo& info);
    void loadName(const DeviceInfo& info);
    void loadPosition(const DeviceInfo& info);
    void loadRadio(const DeviceInfo& info);
    void loadTxPower(const DeviceInfo& info);
    void loadAirtime(const DeviceInfo& info);
    void loadBattery(const DeviceInfo& info);

    // What the fields ask for that the node does not already have. A group
    // whose fields do not parse is left out, and inputError() says why.
    SettingsChange pendingChange() const;
    // Whether the radio fields differ from what the node reports, whether or
    // not they are complete enough to send.
    bool radioEdited() const;
    QString inputError() const;

    // The preset the radio fields amount to, as an index into RadioPresets, or
    // -1 when they match none.
    int matchingPreset() const;
    int currentPreset() const;
    void choosePreset(int index);
    void radioFieldEdited();

    void edited();
    void revalidate();
    void updateEditable();
    void save();
    void onSettingsApplied(const SettingsResult& result);

    proto::CompanionClient* client_ = nullptr;
    // What the node last reported, which is what a field is compared with to
    // decide whether it has changed.
    DeviceInfo baseline_;
    bool saving_ = false;
    bool advertChanged_ = false;
    // What the last save could not do, until the next edit.
    QString failure_;

    QLabel* banner_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLabel* battery_ = nullptr;
    QCheckBox* position_ = nullptr;
    QLineEdit* latitude_ = nullptr;
    QLineEdit* longitude_ = nullptr;
    QComboBox* preset_ = nullptr;
    QLineEdit* frequency_ = nullptr;
    QComboBox* bandwidth_ = nullptr;
    QComboBox* sf_ = nullptr;
    QComboBox* cr_ = nullptr;
    QCheckBox* repeat_ = nullptr;
    QSpinBox* txPower_ = nullptr;
    QComboBox* airtime_ = nullptr;
    QLabel* note_ = nullptr;
    QLabel* error_ = nullptr;
    QPushButton* closeButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
};
