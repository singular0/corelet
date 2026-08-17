#include "ui/add_channel_dialogs.h"

#include <QAction>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRandomGenerator>
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

// Wide enough to hold a 32-character key on one line without making the dialog
// too wide for the uConsole. The hashtag dialog has no key in it and is sized
// for its one short field instead.
constexpr int KeyDialogWidth = 460;
constexpr int TagDialogWidth = 380;

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

// A channel name field. Not setMaxLength: the wire limit is 32 encoded bytes,
// which QLineEdit cannot express -- it counts characters, and a name in Cyrillic
// or with an emoji in it runs out of field long before it runs out of
// characters. A ByteLimit caps it in the right unit.
QLineEdit* nameField() {
    auto* field = new QLineEdit;
    field->setPlaceholderText(QStringLiteral("Kitchen table"));
    new ByteLimit(field, proto::MaxChannelNameBytes);
    return field;
}

// A trailing icon action, drawn muted until it is pointed at. QIcon picks the
// mode itself, which is cheaper than restyling on hover.
QAction* fieldAction(QLineEdit* field, const QString& icon, const QString& text, qreal dpr) {
    const int size = theme::scaled(field->font(), 16);
    QIcon set;
    set.addPixmap(icons::tinted(icon, size, theme::TextMuted, dpr), QIcon::Normal);
    set.addPixmap(icons::tinted(icon, size, theme::Text, dpr), QIcon::Active);

    QAction* action = field->addAction(set, QLineEdit::TrailingPosition);
    action->setText(text);
    action->setToolTip(text);
    return action;
}

// The body of a two-field dialog. Zero margins: the frame around it supplies
// the dialog's own.
QFormLayout* formBody(QWidget* body) {
    auto* layout = new QFormLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    return layout;
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared frame and checks
// ---------------------------------------------------------------------------

AddChannelDialog::AddChannelDialog(const QVector<model::Channel>& existing, const QString& title,
                                   const QString& acceptText, QWidget* parent)
    : QDialog(parent), existing_(existing), freeSlot_(firstFreeSlot(existing)) {
    ui::configureDialogWindow(*this);
    setWindowTitle(title);

    error_ = new QLabel;
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::Error.name()));

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Cancel);
    acceptButton_ = buttons_->addButton(acceptText, QDialogButtonBox::AcceptRole);
    acceptButton_->setDefault(true);
    // Nothing is typed yet, and revalidate() cannot run until the subclass has
    // built the fields it reads.
    acceptButton_->setEnabled(false);

    connect(buttons_, &QDialogButtonBox::accepted, this, &AddChannelDialog::onAccepted);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

bool AddChannelDialog::hasFreeSlot(const QVector<model::Channel>& existing) {
    return firstFreeSlot(existing) >= 0;
}

bool AddChannelDialog::publicChannelJoined(const QVector<model::Channel>& existing) {
    return std::any_of(existing.cbegin(), existing.cend(), [](const model::Channel& ch) {
        return ch.secret == model::publicChannelKey();
    });
}

model::Channel AddChannelDialog::publicChannel(const QVector<model::Channel>& existing) {
    model::Channel ch;
    if (const int slot = firstFreeSlot(existing); slot >= 0) {
        ch.index = slot;
        ch.name = QStringLiteral("Public");
        ch.secret = model::publicChannelKey();
        ch.type = model::Channel::classify(ch.name, ch.secret);
    }
    return ch;
}

void AddChannelDialog::buildFrame(QWidget* body, int width) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);
    layout->addWidget(body, 1);
    layout->addWidget(error_);
    layout->addWidget(buttons_);
    ui::lockDialogSize(*this, *layout, width);
}

void AddChannelDialog::revalidate() {
    const model::Channel ch = describedChannel();

    // An unfinished form is not a mistake worth complaining about; the disabled
    // button says enough. A field that cannot be finished at all is another
    // matter, and that is what inputError() speaks for.
    if (ch.name.isEmpty() || ch.secret.isEmpty()) {
        setError(inputError());
        acceptButton_->setEnabled(false);
        return;
    }

    if (freeSlot_ < 0) {
        setError(QStringLiteral("All %1 channel slots are in use.").arg(proto::MaxChannels));
        acceptButton_->setEnabled(false);
        return;
    }

    // Nothing checks the name's length here: every field it can come from is
    // capped at the 32-byte wire field by a ByteLimit, with the hashtag's '#'
    // reserved out of the same 32. CompanionClient::setChannel refuses an
    // over-long name in any case, which is where a caller that is not one of
    // these dialogs would find out.
    if (isZero(ch.secret)) {
        // An all-zero key is how SET_CHANNEL clears a slot, so it would delete
        // rather than join.
        setError(QStringLiteral("An all-zero key does not name a channel."));
        acceptButton_->setEnabled(false);
        return;
    }

    for (const model::Channel& other : existing_) {
        if (other.secret != ch.secret) continue;
        setError(QStringLiteral("Already joined as \"%1\" in slot %2.")
                     .arg(other.displayName())
                     .arg(other.index));
        acceptButton_->setEnabled(false);
        return;
    }

    setError({});
    acceptButton_->setEnabled(true);
}

void AddChannelDialog::setError(const QString& text) {
    error_->setText(text);
}

void AddChannelDialog::onAccepted() {
    const model::Channel ch = describedChannel();
    if (!acceptButton_->isEnabled() || !ch.configured()) return;
    result_ = ch;
    accept();
}

// ---------------------------------------------------------------------------
// Create a private channel
// ---------------------------------------------------------------------------

CreatePrivateChannelDialog::CreatePrivateChannelDialog(const QVector<model::Channel>& existing,
                                                       QWidget* parent)
    : AddChannelDialog(existing, QStringLiteral("Create a private channel"),
                       QStringLiteral("Create"), parent) {
    auto* body = new QWidget;
    QFormLayout* layout = formBody(body);

    name_ = nameField();

    key_ = new QLineEdit;
    key_->setReadOnly(true);
    key_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    const qreal dpr = devicePixelRatioF();
    QAction* copy = fieldAction(key_, QStringLiteral("copy"), QStringLiteral("Copy"), dpr);
    copy->setObjectName(QStringLiteral("copyKeyAction"));
    QAction* regenerate =
        fieldAction(key_, QStringLiteral("refresh-cw"), QStringLiteral("New key"), dpr);
    regenerate->setObjectName(QStringLiteral("newKeyAction"));

    layout->addRow(QStringLiteral("Name"), name_);
    layout->addRow(QStringLiteral("Key"), key_);

    connect(regenerate, &QAction::triggered, this,
            &CreatePrivateChannelDialog::regenerateKey);
    connect(copy, &QAction::triggered, this,
            [this] { QGuiApplication::clipboard()->setText(key_->text()); });
    connect(name_, &QLineEdit::textChanged, this, [this] { revalidate(); });

    buildFrame(body, KeyDialogWidth);
    regenerateKey();
}

void CreatePrivateChannelDialog::regenerateKey() {
    key_->setText(QString::fromLatin1(randomSecret().toHex()));
    revalidate();
}

model::Channel CreatePrivateChannelDialog::describedChannel() const {
    model::Channel ch;
    ch.index = freeSlot();
    ch.name = name_->text().trimmed();
    ch.secret = QByteArray::fromHex(key_->text().toLatin1());
    ch.type = model::Channel::classify(ch.name, ch.secret);
    return ch;
}

// ---------------------------------------------------------------------------
// Join a private channel
// ---------------------------------------------------------------------------

JoinPrivateChannelDialog::JoinPrivateChannelDialog(const QVector<model::Channel>& existing,
                                                   QWidget* parent)
    : AddChannelDialog(existing, QStringLiteral("Join a private channel"),
                       QStringLiteral("Join"), parent) {
    auto* body = new QWidget;
    QFormLayout* layout = formBody(body);

    name_ = nameField();

    // No limit on the key: it is an exact size rather than a budget, and a key
    // of the wrong length is not nearly right, it is the wrong key. Refusing
    // characters would only make that look like a length to grow into.
    key_ = new QLineEdit;
    key_->setPlaceholderText(QStringLiteral("32 hex characters, or base64"));
    key_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    layout->addRow(QStringLiteral("Name"), name_);
    layout->addRow(QStringLiteral("Key"), key_);

    for (QLineEdit* field : {name_, key_})
        connect(field, &QLineEdit::textChanged, this, [this] { revalidate(); });

    buildFrame(body, KeyDialogWidth);
    revalidate();
}

model::Channel JoinPrivateChannelDialog::describedChannel() const {
    model::Channel ch;
    ch.index = freeSlot();
    ch.name = name_->text().trimmed();
    if (const std::optional<QByteArray> secret = parseSecret(key_->text())) ch.secret = *secret;
    ch.type = model::Channel::classify(ch.name, ch.secret);
    return ch;
}

QString JoinPrivateChannelDialog::inputError() const {
    // Only about the key: a form waiting for the name is unfinished rather than
    // wrong, and saying anything about the key there would be saying it about a
    // key that is perfectly good.
    if (key_->text().isEmpty() || parseSecret(key_->text())) return {};
    return QStringLiteral("That key is neither 32 hex characters nor base64.");
}

// ---------------------------------------------------------------------------
// Join a hashtag channel
// ---------------------------------------------------------------------------

JoinHashtagChannelDialog::JoinHashtagChannelDialog(const QVector<model::Channel>& existing,
                                                   QWidget* parent)
    : AddChannelDialog(existing, QStringLiteral("Join a hashtag channel"),
                       QStringLiteral("Join"), parent) {
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    tag_ = new QLineEdit;
    tag_->setPlaceholderText(QStringLiteral("#jokes"));
    // One byte short of the field: the '#' this dialog puts in front of a tag
    // typed without one is part of the name that reaches the slot, and reserving
    // it always beats a budget that moves by a byte as the user types. Someone
    // who types their own '#' is out a byte of a 32-byte name, which no one will
    // ever meet.
    new ByteLimit(tag_, proto::MaxChannelNameBytes - 1);

    // Two things the field cannot show: that the '#' is optional -- it is part
    // of the hashed name, so a tag typed without one has to grow it or derive
    // somebody else's key -- and that the tab this used to share with the public
    // channel was what said a hashtag is not private.
    auto* note = new QLabel(QStringLiteral(
        "A leading '#' is added if you leave it out. The key comes from the tag itself, "
        "so anyone who knows it is already in."));
    note->setWordWrap(true);
    note->setFont(theme::secondaryFont(font()));
    note->setStyleSheet(QStringLiteral("color: %1;").arg(theme::TextMuted.name()));

    layout->addWidget(tag_);
    layout->addWidget(note);

    connect(tag_, &QLineEdit::textChanged, this, [this] { revalidate(); });

    buildFrame(body, TagDialogWidth);
    revalidate();
}

model::Channel JoinHashtagChannelDialog::describedChannel() const {
    model::Channel ch;
    ch.index = freeSlot();
    if (const QString tag = hashtagName(tag_->text()); !tag.isEmpty()) {
        ch.name = tag;
        ch.secret = model::hashtagChannelKey(tag);
    }
    ch.type = model::Channel::classify(ch.name, ch.secret);
    return ch;
}
