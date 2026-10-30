#include "ui/node_config_dialog.h"

#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>

#include "model/radio_presets.h"
#include "protocol/protocol.h"
#include "protocol/text_limits.h"
#include "ui/byte_limit.h"
#include "ui/dialog_settings.h"
#include "ui/icons.h"
#include "ui/theme.h"

namespace {

using Setting = proto::CompanionClient::Setting;

constexpr int NoPreset = -1;

// Numbers are typed the way the wire and every map spell them: a point for the
// decimal and nothing between thousands. Half of Europe's locales want a comma,
// and a coordinate pasted out of a map would then not parse at all.
QLocale numberLocale() {
    QLocale c = QLocale::c();
    c.setNumberOptions(QLocale::OmitGroupSeparator | QLocale::RejectGroupSeparator);
    return c;
}

// A text field rather than a spin box: arrows that step a frequency by a kHz or
// a latitude by a millionth of a degree are no use to anybody, and an empty
// field is the only honest way to show a frequency the node does not have yet.
QLineEdit* decimalField(double bottom, double top, int decimals, const QString& placeholder) {
    auto* field = new QLineEdit;
    auto* validator = new QDoubleValidator(bottom, top, decimals, field);
    validator->setNotation(QDoubleValidator::StandardNotation);
    validator->setLocale(numberLocale());
    field->setValidator(validator);
    field->setPlaceholderText(placeholder);
    return field;
}

std::optional<double> parsed(const QLineEdit* field) {
    if (!field->hasAcceptableInput()) return std::nullopt;
    bool ok = false;
    const double value = numberLocale().toDouble(field->text().trimmed(), &ok);
    if (!ok) return std::nullopt;
    return value;
}

// The wire's units, which is what two values are compared in: a frequency in
// kHz and a position in millionths of a degree are what the node stores, so
// anything finer is not a change.
quint32 toKhz(double mhz) { return quint32(std::lround(mhz * 1000.0)); }
qint32 toE6(double degrees) { return qint32(std::lround(degrees * 1000000.0)); }

// The bandwidth a reported one is, by nearness: SELF_INFO rounds the SX1262's
// 7.81 kHz to 7810 Hz where the list names it 7800.
int nearestBandwidth(double khz) {
    int best = 0;
    for (int i = 1; i < int(std::size(model::LoraBandwidthsHz)); i++)
        if (std::fabs(model::LoraBandwidthsHz[i] - khz * 1000.0) <
            std::fabs(model::LoraBandwidthsHz[best] - khz * 1000.0))
            best = i;
    return best;
}

QString dutyText(int factorMilli) {
    if (factorMilli == 0) return QStringLiteral("100% (no limit)");
    QString text = QString::number(100.0 / (1.0 + factorMilli / 1000.0), 'f', 1);
    if (text.endsWith(QLatin1String(".0"))) text.chop(2);
    return text + QLatin1Char('%');
}

QString settingName(Setting setting) {
    switch (setting) {
        case Setting::Name: return QStringLiteral("Name");
        case Setting::Position: return QStringLiteral("Position");
        case Setting::Radio: return QStringLiteral("Radio");
        case Setting::TxPower: return QStringLiteral("Transmit power");
        case Setting::AirtimeFactor: return QStringLiteral("Duty cycle");
    }
    return {};
}

// The section titles the node pane uses for its own, so the two read as the
// same node.
QLabel* heading(const QString& text, const QFont& base) {
    auto* label = new QLabel(text);
    QFont font = theme::secondaryFont(base);
    font.setBold(true);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    label->setFont(font);
    label->setStyleSheet(QStringLiteral("color: %1;").arg(theme::TextMuted.name()));
    return label;
}

QFormLayout* formLayout() {
    auto* layout = new QFormLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    return layout;
}

QVBoxLayout* column(QLabel* title, QFormLayout* fields) {
    auto* layout = new QVBoxLayout;
    layout->setSpacing(6);
    layout->addWidget(title);
    layout->addLayout(fields);
    layout->addStretch(1);
    return layout;
}

}  // namespace

NodeConfigDialog::NodeConfigDialog(proto::CompanionClient* client, QWidget* parent)
    : QDialog(parent), client_(client), baseline_(client->device()) {
    ui::configureDialogWindow(*this);
    setWindowTitle(QStringLiteral("Node configuration"));

    // A fresh coreletd starts radio-silent and waits for an app to give it a
    // preset, as a new MeshCore radio waits for its phone. The window opens on
    // its own then, and this is what says why.
    banner_ = new QLabel(QStringLiteral("This node has no radio frequency yet and transmits "
                                        "nothing until it has one. Choose your region."));
    banner_->setWordWrap(true);
    banner_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::Warning.name()));

    note_ = new QLabel;
    note_->setWordWrap(true);
    note_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::TextMuted.name()));

    error_ = new QLabel;
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::Error.name()));

    auto* buttons = new QDialogButtonBox;
    closeButton_ = buttons->addButton(QStringLiteral("Close"), QDialogButtonBox::RejectRole);
    saveButton_ = buttons->addButton(QStringLiteral("Save"), QDialogButtonBox::AcceptRole);
    saveButton_->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &NodeConfigDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &NodeConfigDialog::reject);

    // Side by side rather than stacked: the panel is 480 rows high and 1280
    // wide, and the radio's eight rows under the node's six would not fit it.
    auto* columns = new QHBoxLayout;
    columns->setSpacing(24);
    columns->addLayout(buildNodeColumn(), 1);
    columns->addLayout(buildRadioColumn());

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);
    layout->addWidget(banner_);
    layout->addLayout(columns);
    layout->addWidget(note_);
    layout->addWidget(error_);
    layout->addWidget(buttons);

    connect(client_, &proto::CompanionClient::settingsApplied, this,
            &NodeConfigDialog::onSettingsApplied);
    connect(client_, &proto::CompanionClient::deviceInfoChanged, this,
            [this](const DeviceInfo& info) {
                // A reconnect that reached a different node: nothing on screen
                // describes it, and a save from here would change it.
                if (info.pubkey != baseline_.pubkey) {
                    QDialog::reject();
                    return;
                }
                // Battery polls arrive all the time and say nothing about the
                // settings; only the re-read after a save reloads those, so
                // nothing being typed is overwritten.
                loadBattery(info);
            });
    connect(client_, &proto::CompanionClient::stateChanged, this, [this] { revalidate(); });

    load(baseline_);
    ui::lockDialogSize(*this, *layout, 0);
    if (baseline_.freqMhz <= 0) preset_->setFocus(Qt::OtherFocusReason);
}

QLayout* NodeConfigDialog::buildNodeColumn() {
    name_ = new QLineEdit;
    // The most the advert can ever carry. Beside a position it carries less,
    // which inputError() says rather than cutting what was already typed when
    // the position is switched on.
    new ByteLimit(name_, proto::MaxAdvertNameBytes);

    const QString hex = QString::fromLatin1(baseline_.pubkey.toHex());
    auto* publicKey = new QLineEdit(hex);
    publicKey->setReadOnly(true);
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    publicKey->setFont(fixed);
    publicKey->setCursorPosition(0);
    const int copyIconSize = theme::scaled(font(), 16);
    QIcon copyIcon;
    copyIcon.addPixmap(icons::tinted(QStringLiteral("copy"), copyIconSize, theme::TextMuted,
                                    devicePixelRatioF()),
                       QIcon::Normal);
    copyIcon.addPixmap(
        icons::tinted(QStringLiteral("copy"), copyIconSize, theme::Text, devicePixelRatioF()),
        QIcon::Active);
    QAction* copy = publicKey->addAction(copyIcon, QLineEdit::TrailingPosition);
    copy->setText(QStringLiteral("Copy public key"));
    copy->setToolTip(QStringLiteral("Copy public key"));
    connect(copy, &QAction::triggered, publicKey,
            [publicKey] { QGuiApplication::clipboard()->setText(publicKey->text()); });
    // The whole key on one line: it is read off and compared by eye as often as
    // it is copied, and it still leaves the window inside the uConsole's 1280.
    publicKey->setMinimumWidth(QFontMetrics(fixed).horizontalAdvance(hex) + copyIconSize + 32);

    battery_ = new QLabel;
    battery_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    position_ = new QCheckBox(QStringLiteral("Advertise a position"));
    latitude_ = decimalField(-90.0, 90.0, 6, QStringLiteral("Degrees north"));
    longitude_ = decimalField(-180.0, 180.0, 6, QStringLiteral("Degrees east"));

    QFormLayout* fields = formLayout();
    fields->addRow(QStringLiteral("Name"), name_);
    fields->addRow(QStringLiteral("Public key"), publicKey);
    fields->addRow(QStringLiteral("Battery"), battery_);
    fields->addRow(QStringLiteral("Position"), position_);
    fields->addRow(QStringLiteral("Latitude"), latitude_);
    fields->addRow(QStringLiteral("Longitude"), longitude_);

    // textChanged for the name, connected after its ByteLimit, so the check
    // reads text already held inside the budget.
    connect(name_, &QLineEdit::textChanged, this, &NodeConfigDialog::edited);
    connect(position_, &QCheckBox::clicked, this, &NodeConfigDialog::edited);
    for (QLineEdit* field : {latitude_, longitude_})
        connect(field, &QLineEdit::textEdited, this, &NodeConfigDialog::edited);

    return column(heading(QStringLiteral("NODE"), font()), fields);
}

QLayout* NodeConfigDialog::buildRadioColumn() {
    preset_ = new QComboBox;
    for (int i = 0; i < int(std::size(model::RadioPresets)); i++)
        preset_->addItem(QString::fromUtf8(model::RadioPresets[i].title), i);
    preset_->insertSeparator(preset_->count());
    preset_->addItem(QStringLiteral("Custom"), NoPreset);
    // Shown while no frequency is set, which is the one state where neither a
    // preset nor Custom is true.
    preset_->setPlaceholderText(QStringLiteral("Choose your region"));
    preset_->setToolTip(QStringLiteral("The presets the official MeshCore app offers. Nodes hear "
                                       "each other only on identical radio settings."));

    frequency_ = decimalField(proto::MinFreqMhz, proto::MaxFreqMhz, 3, QStringLiteral("Not set"));

    bandwidth_ = new QComboBox;
    for (const quint32 hz : model::LoraBandwidthsHz)
        bandwidth_->addItem(QStringLiteral("%1 kHz").arg(hz / 1000.0), int(hz));

    sf_ = new QComboBox;
    for (int sf = 5; sf <= 12; sf++) sf_->addItem(QString::number(sf), sf);

    cr_ = new QComboBox;
    for (int cr = 5; cr <= 8; cr++) cr_->addItem(QStringLiteral("4/%1").arg(cr), cr);

    txPower_ = new QSpinBox;
    txPower_->setSuffix(QStringLiteral(" dBm"));

    airtime_ = new QComboBox;
    airtime_->setToolTip(QStringLiteral("How much of the time the node may spend transmitting. "
                                        "MeshCore sets it as an airtime factor: after each "
                                        "transmission the node keeps quiet for that many times "
                                        "as long."));

    repeat_ = new QCheckBox(QStringLiteral("Relay other nodes' traffic"));

    QFormLayout* fields = formLayout();
    fields->addRow(QStringLiteral("Region"), preset_);
    fields->addRow(QStringLiteral("Frequency (MHz)"), frequency_);
    fields->addRow(QStringLiteral("Bandwidth"), bandwidth_);
    fields->addRow(QStringLiteral("Spreading factor"), sf_);
    fields->addRow(QStringLiteral("Coding rate"), cr_);
    fields->addRow(QStringLiteral("Transmit power"), txPower_);
    fields->addRow(QStringLiteral("Duty cycle"), airtime_);
    fields->addRow(QStringLiteral("Repeat"), repeat_);

    // Only what the user does, never what loading does: a preset fills four
    // fields one after another, and matching after each of them would land on
    // whichever preset the half-filled fields happened to resemble.
    connect(preset_, &QComboBox::activated, this, &NodeConfigDialog::choosePreset);
    connect(frequency_, &QLineEdit::textEdited, this, &NodeConfigDialog::radioFieldEdited);
    for (QComboBox* field : {bandwidth_, sf_, cr_})
        connect(field, &QComboBox::activated, this, &NodeConfigDialog::radioFieldEdited);
    connect(repeat_, &QCheckBox::clicked, this, &NodeConfigDialog::edited);
    connect(txPower_, &QSpinBox::valueChanged, this, &NodeConfigDialog::edited);
    connect(airtime_, &QComboBox::activated, this, &NodeConfigDialog::edited);

    return column(heading(QStringLiteral("RADIO"), font()), fields);
}

// ---------------------------------------------------------------------------
// What the node reports
// ---------------------------------------------------------------------------

void NodeConfigDialog::load(const DeviceInfo& info) {
    baseline_ = info;
    loadName(info);
    loadPosition(info);
    loadRadio(info);
    loadTxPower(info);
    loadAirtime(info);
    loadBattery(info);
    revalidate();
}

void NodeConfigDialog::loadName(const DeviceInfo& info) { name_->setText(info.name); }

void NodeConfigDialog::loadPosition(const DeviceInfo& info) {
    position_->setChecked(info.hasLocation);
    latitude_->setText(info.hasLocation ? QString::number(info.latitude, 'f', 6) : QString());
    longitude_->setText(info.hasLocation ? QString::number(info.longitude, 'f', 6) : QString());
}

void NodeConfigDialog::loadRadio(const DeviceInfo& info) {
    frequency_->setText(info.freqMhz > 0 ? QString::number(toKhz(info.freqMhz) / 1000.0, 'f', 3)
                                         : QString());
    bandwidth_->setCurrentIndex(nearestBandwidth(info.bwKhz));
    sf_->setCurrentIndex(sf_->findData(info.sf));
    cr_->setCurrentIndex(cr_->findData(info.cr));
    repeat_->setChecked(info.repeat.value_or(false));

    const int match = matchingPreset();
    preset_->setCurrentIndex(match != NoPreset     ? preset_->findData(match)
                             : info.freqMhz > 0    ? preset_->findData(NoPreset)
                                                   : -1);
}

void NodeConfigDialog::loadTxPower(const DeviceInfo& info) {
    // Bounded by what the radio says it can do, but never so tightly that the
    // power it is already running at falls outside: a ceiling reported wrong
    // would otherwise move the value as the window opened.
    txPower_->setRange(proto::MinTxPowerDbm, std::max(info.maxTxPowerDbm, info.txPowerDbm));
    txPower_->setValue(info.txPowerDbm);
}

void NodeConfigDialog::loadAirtime(const DeviceInfo& info) {
    airtime_->clear();
    if (!info.airtimeFactorMilli) {
        airtime_->setPlaceholderText(QStringLiteral("Not reported by this node"));
        airtime_->setCurrentIndex(-1);
        return;
    }

    // MeshCore offers whole factors. A duty cycle written into coreletd's
    // settings by hand need not be one, and is shown as it is rather than
    // rounded to a neighbour that a save would then quietly send.
    const int current = *info.airtimeFactorMilli;
    QVector<int> factors;
    for (int factor = 0; factor <= proto::MaxAirtimeFactorMilli; factor += 1000)
        factors.append(factor);
    const bool whole = factors.contains(current);
    if (!whole) factors.append(current);
    std::sort(factors.begin(), factors.end());
    for (const int factor : factors)
        airtime_->addItem(factor == current && !whole
                              ? QStringLiteral("%1 (set elsewhere)").arg(dutyText(factor))
                              : dutyText(factor),
                          factor);
    airtime_->setCurrentIndex(airtime_->findData(current));
}

void NodeConfigDialog::loadBattery(const DeviceInfo& info) {
    battery_->setText(info.batteryMillivolts < 0
                          ? QStringLiteral("Unavailable")
                          : QStringLiteral("%1 V (%2%)")
                                .arg(info.batteryMillivolts / 1000.0, 0, 'f', 3)
                                .arg(info.batteryPercent));
}

// ---------------------------------------------------------------------------
// What the fields ask for
// ---------------------------------------------------------------------------

proto::CompanionClient::SettingsChange NodeConfigDialog::pendingChange() const {
    SettingsChange change;

    const QString name = name_->text().trimmed();
    if (name != baseline_.name) change.name = name;

    if (!position_->isChecked()) {
        if (baseline_.hasLocation) change.position = SettingsChange::Position {};
    } else if (const auto lat = parsed(latitude_), lon = parsed(longitude_); lat && lon) {
        if (!baseline_.hasLocation || toE6(*lat) != toE6(baseline_.latitude) ||
            toE6(*lon) != toE6(baseline_.longitude))
            change.position = SettingsChange::Position {true, *lat, *lon};
    }

    if (radioEdited()) {
        if (const auto freq = parsed(frequency_)) {
            // Repeat goes with every radio change, as it is now unless it was
            // changed here too: the wire takes a missing repeat byte as off.
            change.radio = SettingsChange::Radio {*freq, bandwidth_->currentData().toInt() / 1000.0,
                                                  sf_->currentData().toInt(),
                                                  cr_->currentData().toInt(), repeat_->isChecked()};
        }
    }

    if (txPower_->value() != baseline_.txPowerDbm) change.txPowerDbm = txPower_->value();

    if (baseline_.airtimeFactorMilli && airtime_->currentIndex() >= 0 &&
        airtime_->currentData().toInt() != *baseline_.airtimeFactorMilli)
        change.airtimeFactorMilli = airtime_->currentData().toInt();

    return change;
}

bool NodeConfigDialog::radioEdited() const {
    const quint32 reportedKhz = baseline_.freqMhz > 0 ? toKhz(baseline_.freqMhz) : 0;
    if (const auto freq = parsed(frequency_)) {
        if (toKhz(*freq) != reportedKhz) return true;
    } else if (!(frequency_->text().trimmed().isEmpty() && reportedKhz == 0)) {
        // Half a frequency, or one cleared away: not sendable, but not what
        // the node has either.
        return true;
    }
    return bandwidth_->currentIndex() != nearestBandwidth(baseline_.bwKhz) ||
           sf_->currentData().toInt() != baseline_.sf ||
           cr_->currentData().toInt() != baseline_.cr ||
           repeat_->isChecked() != baseline_.repeat.value_or(false);
}

QString NodeConfigDialog::inputError() const {
    const QString name = name_->text().trimmed();
    const bool advertised = position_->isChecked();

    if (name != baseline_.name && name.isEmpty()) return QStringLiteral("A node needs a name.");

    if (advertised) {
        const auto lat = parsed(latitude_);
        const auto lon = parsed(longitude_);
        if (!lat || !lon)
            return QStringLiteral(
                "Enter a latitude from -90 to 90 and a longitude from -180 to 180.");
        if (toE6(*lat) == 0 && toE6(*lon) == 0)
            return QStringLiteral("0, 0 is how a node is told not to advertise a position.");
    }

    // Only worth saying when one of the two is about to be sent: a node
    // already advertising both is the node's business.
    const int budget = proto::maxAdvertNameBytes(advertised);
    const int bytes = proto::utf8Bytes(name);
    if (bytes > budget && (name != baseline_.name || advertised != baseline_.hasLocation))
        return QStringLiteral("Beside a position the advert holds %1 bytes of name, and this one "
                              "takes %2.")
            .arg(budget)
            .arg(bytes);

    if (radioEdited()) {
        const auto freq = parsed(frequency_);
        if (!freq)
            return QStringLiteral("Choose a region, or enter a frequency from %1 to %2 MHz.")
                .arg(proto::MinFreqMhz)
                .arg(proto::MaxFreqMhz);
        // Both ends refuse the whole command over it, frequency and all.
        if (repeat_->isChecked() && !proto::repeatAllowedAt(toKhz(*freq)))
            return QStringLiteral("Repeat can only be on at 433.000, 869.495 or 918.000 MHz.");
    }
    return {};
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------

int NodeConfigDialog::matchingPreset() const {
    const auto freq = parsed(frequency_);
    if (!freq) return NoPreset;
    for (int i = 0; i < int(std::size(model::RadioPresets)); i++) {
        const model::RadioPreset& preset = model::RadioPresets[i];
        if (preset.freqKhz == toKhz(*freq) &&
            quint32(bandwidth_->currentData().toInt()) == preset.bwHz &&
            sf_->currentData().toInt() == preset.sf && cr_->currentData().toInt() == preset.cr)
            return i;
    }
    return NoPreset;
}

int NodeConfigDialog::currentPreset() const {
    // An empty combo has no current data, and an invalid QVariant reads as 0 --
    // which is a preset, not the absence of one.
    return preset_->currentIndex() < 0 ? NoPreset : preset_->currentData().toInt();
}

void NodeConfigDialog::choosePreset(int index) {
    const int chosen = preset_->itemData(index).toInt();
    // Custom changes nothing by itself; it is what the fields already say.
    if (chosen != NoPreset) {
        const model::RadioPreset& preset = model::RadioPresets[chosen];
        frequency_->setText(QString::number(preset.freqMhz(), 'f', 3));
        bandwidth_->setCurrentIndex(bandwidth_->findData(int(preset.bwHz)));
        sf_->setCurrentIndex(sf_->findData(preset.sf));
        cr_->setCurrentIndex(cr_->findData(preset.cr));
    }
    edited();
}

void NodeConfigDialog::radioFieldEdited() {
    // A preset the fields still match stays chosen, even where another shares
    // its settings: somebody who picked Brazil should not see it turn into the
    // Australian preset that happens to be identical.
    const int current = currentPreset();
    const int match = matchingPreset();
    if (current == NoPreset || current != match) {
        preset_->setCurrentIndex(match != NoPreset                      ? preset_->findData(match)
                                 : frequency_->text().trimmed().isEmpty() ? -1
                                                                         : preset_->findData(NoPreset));
    }
    edited();
}

// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------

void NodeConfigDialog::edited() {
    failure_.clear();
    revalidate();
}

void NodeConfigDialog::revalidate() {
    const bool editable = !saving_ && !baseline_.settingsDisabled;
    const bool ready = client_->state() == proto::CompanionClient::State::Ready;
    const SettingsChange change = pendingChange();
    const bool dirty = change.name || change.position || change.radio || change.txPowerDbm ||
                       change.airtimeFactorMilli;
    const QString error = editable ? inputError() : QString();

    const QString shown = error.isEmpty() ? failure_ : error;
    error_->setText(shown);
    error_->setVisible(!shown.isEmpty());

    QString note;
    if (baseline_.settingsDisabled)
        note = QStringLiteral("This node does not take settings from an app, so they are "
                              "shown as it reports them.");
    else if (saving_)
        note = QStringLiteral("Saving...");
    else if (!ready)
        note = QStringLiteral("Waiting for the connection...");
    note_->setText(note);
    note_->setVisible(!note.isEmpty());

    banner_->setVisible(baseline_.freqMhz <= 0);

    saveButton_->setVisible(!baseline_.settingsDisabled);
    saveButton_->setEnabled(editable && ready && dirty && error.isEmpty());
    // The same window is where the node is only looked at, and "Cancel" there
    // would suggest something is being abandoned.
    closeButton_->setText(dirty || !error.isEmpty() ? QStringLiteral("Cancel")
                                                    : QStringLiteral("Close"));
    closeButton_->setEnabled(!saving_);
    updateEditable();
}

void NodeConfigDialog::updateEditable() {
    const bool editable = !saving_ && !baseline_.settingsDisabled;
    for (QWidget* field : std::initializer_list<QWidget*> {name_, position_, preset_, frequency_,
                                                           bandwidth_, sf_, cr_, txPower_})
        field->setEnabled(editable);
    latitude_->setEnabled(editable && position_->isChecked());
    longitude_->setEnabled(editable && position_->isChecked());
    airtime_->setEnabled(editable && baseline_.airtimeFactorMilli.has_value());

    // Offered where it can be turned on, and wherever it is already on so it
    // can be turned off: a repeat set in coreletd's settings file by hand is not
    // held to the frequencies an app is.
    const auto freq = parsed(frequency_);
    const bool allowedHere = freq && proto::repeatAllowedAt(toKhz(*freq));
    repeat_->setEnabled(editable && baseline_.repeat.has_value() &&
                        (allowedHere || repeat_->isChecked()));
    repeat_->setToolTip(
        !baseline_.repeat
            ? QStringLiteral("This node does not report repeat; its firmware predates it.")
            : QStringLiteral("Pass on flood traffic, and messages routed through this node, as "
                             "a repeater does. Only at 433.000, 869.495 or 918.000 MHz."));
}

void NodeConfigDialog::save() {
    if (!saveButton_->isEnabled()) return;
    const SettingsChange change = pendingChange();
    saving_ = true;
    failure_.clear();
    revalidate();
    // May answer before it returns, when nothing needed the link.
    client_->applySettings(change);
}

void NodeConfigDialog::onSettingsApplied(const SettingsResult& result) {
    if (!saving_) return;
    saving_ = false;

    if (result.applied.contains(Setting::Name) || result.applied.contains(Setting::Position))
        advertChanged_ = true;

    const DeviceInfo& now = client_->device();
    if (result.disabled) {
        // Nothing was changed, and nothing can be: show the node as it is.
        load(now);
    } else {
        // What took, and anything not asked about, comes from the node. What
        // did not take keeps what was typed, so it can be put right rather
        // than typed again.
        baseline_ = now;
        if (!result.refused.contains(Setting::Name)) loadName(now);
        if (!result.refused.contains(Setting::Position)) loadPosition(now);
        if (!result.refused.contains(Setting::Radio)) loadRadio(now);
        if (!result.refused.contains(Setting::TxPower)) loadTxPower(now);
        if (!result.refused.contains(Setting::AirtimeFactor)) loadAirtime(now);
        loadBattery(now);
    }

    if (result.refused.isEmpty() && !result.disabled) {
        accept();
        return;
    }

    // After the loads above, each of which counts as an edit and clears it.
    QStringList lines;
    for (auto it = result.refused.cbegin(); it != result.refused.cend(); ++it)
        lines.append(QStringLiteral("%1: %2").arg(settingName(it.key()), it.value()));
    failure_ = lines.join(QLatin1Char('\n'));
    revalidate();
}

void NodeConfigDialog::reject() {
    if (saving_) return;
    QDialog::reject();
}
