#include "ui/add_channel_dialog.h"

#include <QAction>
#include <QButtonGroup>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRandomGenerator>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cctype>
#include <optional>

#include "protocol/protocol.h"
#include "protocol/text_limits.h"
#include "ui/byte_limit.h"
#include "ui/dialog_settings.h"
#include "ui/icons.h"
#include "ui/theme.h"

namespace {

constexpr int CreateTab = 0;
constexpr int JoinTab = 1;
constexpr int PublicTab = 2;

// Lowest slot nothing occupies, or -1 when all eight are taken. Lowest rather
// than next-highest because slots are sparse: clearing slot 3 and adding a
// channel should reuse it instead of running out at the top.
int firstFreeSlot(const QVector<model::Channel>& existing) {
    for (int slot = 0; slot < proto::MaxChannels; slot++) {
        bool taken = false;
        for (const model::Channel& ch : existing)
            if (ch.index == slot) taken = true;
        if (!taken) return slot;
    }
    return -1;
}

// A channel key is a shared secret, so it comes from the system CSPRNG rather
// than the default global generator, which is seeded for speed and not secrecy.
QByteArray randomSecret() {
    quint32 words[model::ChannelSecretSize / sizeof(quint32)];
    QRandomGenerator::system()->fillRange(words);
    return QByteArray(reinterpret_cast<const char*>(words), model::ChannelSecretSize);
}

bool isHex(const QByteArray& text) {
    for (const char c : text)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return !text.isEmpty();
}

// Keys are passed around as hex — what the daemon writes in its state file —
// and as base64, which is what the other MeshCore apps put on a QR code.
// Accepting both beats making someone convert one to the other by hand.
std::optional<QByteArray> parseSecret(const QString& text) {
    QByteArray raw;
    for (const QChar c : text)
        if (!c.isSpace() && c != QLatin1Char(':')) raw.append(char(c.toLatin1()));

    if (raw.size() == model::ChannelSecretSize * 2 && isHex(raw))
        return QByteArray::fromHex(raw);

    // Sixteen bytes encode to 24 characters with padding, which a copy out of a
    // chat message often loses.
    while (raw.size() % 4 != 0) raw.append('=');
    // Decoding defaults to skipping anything it does not recognise, which would
    // turn a typo into a plausible-looking key; insist on well-formed input.
    for (const QByteArray::Base64Option alphabet :
         {QByteArray::Base64Encoding, QByteArray::Base64UrlEncoding}) {
        const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
            raw, alphabet | QByteArray::AbortOnBase64DecodingErrors);
        if (decoded && decoded.decoded.size() == model::ChannelSecretSize) return decoded.decoded;
    }
    return std::nullopt;
}

bool isZero(const QByteArray& secret) {
    for (const char c : secret)
        if (c != 0) return false;
    return true;
}

// The channel name a typed hashtag amounts to, or empty when it does not name
// one yet. The '#' is part of the hashed name -- a tag typed without one would
// otherwise derive a different key than everyone else's -- and so also a byte of
// the 32 the name field holds, which is why the field's limit reserves one.
QString hashtagName(const QString& typed) {
    QString tag = typed.trimmed();
    if (!tag.startsWith(QLatin1Char('#'))) tag.prepend(QLatin1Char('#'));
    return tag.size() > 1 ? tag : QString();
}

}  // namespace

AddChannelDialog::AddChannelDialog(const QVector<model::Channel>& existing, QWidget* parent)
    : QDialog(parent), existing_(existing), freeSlot_(firstFreeSlot(existing)) {
    ui::configureDialogWindow(*this);
    setWindowTitle(QStringLiteral("Add channel"));
    buildUi();
    regenerateKey();
    updateOkButton();
}

bool AddChannelDialog::hasFreeSlot(const QVector<model::Channel>& existing) {
    return firstFreeSlot(existing) >= 0;
}

void AddChannelDialog::buildUi() {
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const qreal dpr = devicePixelRatioF();
    tabs_ = new QTabWidget;

    // --- new private channel ------------------------------------------------
    auto* createTab = new QWidget;
    auto* createLayout = new QFormLayout(createTab);
    createLayout->setContentsMargins(12, 10, 12, 10);
    createLayout->setSpacing(6);

    // Not setMaxLength on any of the three name fields: the wire limit is 32
    // encoded bytes, which QLineEdit cannot express -- it counts characters, and
    // a name in Cyrillic or with an emoji in it runs out of field long before it
    // runs out of characters. A ByteLimit caps each one in the right unit.
    createName_ = new QLineEdit;
    createName_->setPlaceholderText(QStringLiteral("Kitchen table"));
    new ByteLimit(createName_, proto::MaxChannelNameBytes);

    createKey_ = new QLineEdit;
    createKey_->setReadOnly(true);
    createKey_->setFont(mono);

    const int keyActionIconSize = theme::scaled(font(), 16);
    QIcon regenerateIcon;
    regenerateIcon.addPixmap(
        icons::tinted(QStringLiteral("refresh-cw"), keyActionIconSize, theme::TextMuted, dpr),
        QIcon::Normal);
    regenerateIcon.addPixmap(
        icons::tinted(QStringLiteral("refresh-cw"), keyActionIconSize, theme::Text, dpr),
        QIcon::Active);
    QIcon copyIcon;
    copyIcon.addPixmap(
        icons::tinted(QStringLiteral("copy"), keyActionIconSize, theme::TextMuted, dpr),
        QIcon::Normal);
    copyIcon.addPixmap(
        icons::tinted(QStringLiteral("copy"), keyActionIconSize, theme::Text, dpr),
        QIcon::Active);

    QAction* copy = createKey_->addAction(copyIcon, QLineEdit::TrailingPosition);
    copy->setObjectName(QStringLiteral("copyKeyAction"));
    copy->setText(QStringLiteral("Copy"));
    copy->setToolTip(QStringLiteral("Copy"));
    QAction* regenerate = createKey_->addAction(regenerateIcon, QLineEdit::TrailingPosition);
    regenerate->setObjectName(QStringLiteral("newKeyAction"));
    regenerate->setText(QStringLiteral("New key"));
    regenerate->setToolTip(QStringLiteral("New key"));

    createLayout->addRow(QStringLiteral("Name"), createName_);
    createLayout->addRow(QStringLiteral("Key"), createKey_);

    // --- join with a key ----------------------------------------------------
    auto* joinTab = new QWidget;
    auto* joinLayout = new QFormLayout(joinTab);
    joinLayout->setContentsMargins(12, 10, 12, 10);
    joinLayout->setSpacing(6);

    joinName_ = new QLineEdit;
    joinName_->setPlaceholderText(QStringLiteral("Kitchen table"));
    new ByteLimit(joinName_, proto::MaxChannelNameBytes);

    // No limit on the key: it is an exact size rather than a budget, and a key
    // of the wrong length is not nearly right, it is the wrong key. Refusing
    // characters would only make that look like a length to grow into.
    joinKey_ = new QLineEdit;
    joinKey_->setPlaceholderText(QStringLiteral("32 hex characters, or base64"));
    joinKey_->setFont(mono);

    joinLayout->addRow(QStringLiteral("Name"), joinName_);
    joinLayout->addRow(QStringLiteral("Key"), joinKey_);

    // --- public and hashtag -------------------------------------------------
    auto* publicTab = new QWidget;
    auto* publicLayout = new QVBoxLayout(publicTab);
    publicLayout->setContentsMargins(12, 10, 12, 10);
    publicLayout->setSpacing(6);

    const bool publicAlreadyJoined = std::any_of(
        existing_.cbegin(), existing_.cend(), [](const model::Channel& ch) {
            return ch.secret == model::publicChannelKey();
        });
    publicWellKnown_ = new QRadioButton(
        publicAlreadyJoined ? QStringLiteral("MeshCore Public (already joined)")
                            : QStringLiteral("MeshCore Public"));
    publicWellKnown_->setEnabled(!publicAlreadyJoined);
    publicHashtag_ = new QRadioButton(QStringLiteral("Hashtag"));
    publicHashtag_->setChecked(true);

    auto* group = new QButtonGroup(this);
    group->addButton(publicWellKnown_);
    group->addButton(publicHashtag_);

    hashtag_ = new QLineEdit;
    hashtag_->setPlaceholderText(QStringLiteral("#jokes"));
    // One byte short of the field: the '#' this dialog puts in front of a tag
    // typed without one is part of the name that reaches the slot, and reserving
    // it always beats a budget that moves by a byte as the user types. Someone
    // who types their own '#' is out a byte of a 32-byte name, which no one will
    // ever meet.
    new ByteLimit(hashtag_, proto::MaxChannelNameBytes - 1);

    auto* hashtagRow = new QHBoxLayout;
    hashtagRow->setSpacing(6);
    hashtagRow->addWidget(publicHashtag_);
    hashtagRow->addWidget(hashtag_, 1);

    publicLayout->addWidget(publicWellKnown_);
    publicLayout->addLayout(hashtagRow);
    publicLayout->addStretch(1);

    tabs_->addTab(createTab, QStringLiteral("Create Private"));
    tabs_->addTab(joinTab, QStringLiteral("Join Private"));
    tabs_->addTab(publicTab, QStringLiteral("Join Public"));

    error_ = new QLabel;
    error_->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    addButton_ = buttons->addButton(QStringLiteral("Add"), QDialogButtonBox::AcceptRole);
    addButton_->setDefault(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);
    layout->addWidget(tabs_, 1);
    layout->addWidget(error_);
    layout->addWidget(buttons);

    connect(regenerate, &QAction::triggered, this, &AddChannelDialog::regenerateKey);
    connect(copy, &QAction::triggered, this,
            [this] { QGuiApplication::clipboard()->setText(createKey_->text()); });
    connect(tabs_, &QTabWidget::currentChanged, this, [this] { updateOkButton(); });
    connect(publicWellKnown_, &QRadioButton::toggled, this, [this](bool on) {
        hashtag_->setEnabled(!on);
        updateOkButton();
    });
    // Connected after each field's ByteLimit, so this reads fields already held
    // inside their budgets.
    for (QLineEdit* field : {createName_, joinName_, joinKey_, hashtag_})
        connect(field, &QLineEdit::textChanged, this, [this] { updateOkButton(); });
    connect(buttons, &QDialogButtonBox::accepted, this, &AddChannelDialog::onAccepted);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Keep a 32-character key on one line without making the dialog too wide
    // for the uConsole.
    ui::lockDialogSize(*this, *layout, 460);
}

void AddChannelDialog::regenerateKey() {
    createKey_->setText(QString::fromLatin1(randomSecret().toHex()));
    updateOkButton();
}

// ---------------------------------------------------------------------------
// What the tabs describe
// ---------------------------------------------------------------------------

model::Channel AddChannelDialog::currentChannel() const {
    model::Channel ch;
    ch.index = freeSlot_;

    switch (tabs_->currentIndex()) {
        case CreateTab:
            ch.name = createName_->text().trimmed();
            ch.secret = QByteArray::fromHex(createKey_->text().toLatin1());
            break;
        case JoinTab:
            ch.name = joinName_->text().trimmed();
            if (const std::optional<QByteArray> secret = parseSecret(joinKey_->text()))
                ch.secret = *secret;
            break;
        case PublicTab:
            if (publicWellKnown_->isChecked()) {
                ch.name = QStringLiteral("Public");
                ch.secret = model::publicChannelKey();
            } else if (const QString tag = hashtagName(hashtag_->text()); !tag.isEmpty()) {
                ch.name = tag;
                ch.secret = model::hashtagChannelKey(tag);
            }
            break;
    }

    ch.type = model::Channel::classify(ch.name, ch.secret);
    return ch;
}

void AddChannelDialog::updateOkButton() {
    const model::Channel ch = currentChannel();

    // An unfinished form is not a mistake worth complaining about; the disabled
    // Add button says enough.
    if (ch.name.isEmpty() || ch.secret.isEmpty()) {
        const bool keyTyped = tabs_->currentIndex() == JoinTab && !joinKey_->text().isEmpty();
        setError(keyTyped ? QStringLiteral("That key is neither 32 hex characters nor base64.")
                          : QString());
        addButton_->setEnabled(false);
        return;
    }

    if (freeSlot_ < 0) {
        setError(QStringLiteral("All %1 channel slots are in use.").arg(proto::MaxChannels));
        addButton_->setEnabled(false);
        return;
    }

    // Nothing checks the name's length here: every field it can come from is
    // capped at the 32-byte wire field by a ByteLimit, with the hashtag's '#'
    // reserved out of the same 32. CompanionClient::setChannel refuses an
    // over-long name in any case, which is where a caller that is not this
    // dialog would find out.
    if (isZero(ch.secret)) {
        // An all-zero key is how SET_CHANNEL clears a slot, so it would delete
        // rather than join.
        setError(QStringLiteral("An all-zero key does not name a channel."));
        addButton_->setEnabled(false);
        return;
    }

    for (const model::Channel& other : existing_) {
        if (other.secret != ch.secret) continue;
        setError(QStringLiteral("Already joined as \"%1\" in slot %2.")
                     .arg(other.displayName())
                     .arg(other.index));
        addButton_->setEnabled(false);
        return;
    }

    setError({});
    addButton_->setEnabled(true);
}

void AddChannelDialog::setError(const QString& text) {
    error_->setText(text);
    error_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::Error.name()));
}

void AddChannelDialog::onAccepted() {
    const model::Channel ch = currentChannel();
    if (!addButton_->isEnabled() || !ch.configured()) return;
    result_ = ch;
    accept();
}
