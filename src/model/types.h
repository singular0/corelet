#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QString>

namespace model {

// Mirrors the companion protocol's channel secret field. Kept here rather than
// pulled from protocol.h so the value types stay independent of the wire code.
inline constexpr int ChannelSecretSize = 16;

// The well-known key every node ships with in slot 0, so a fresh install can
// talk to anyone. A constant, not a secret: joining it is knowing it.
inline QByteArray publicChannelKey() {
    return QByteArray::fromHex("8b3387e9c5cdea6ac9e5edbaa115cd72");
}

// A hashtag channel's key is derived from its name, which is what makes it
// joinable by word of mouth. The '#' is part of the hashed name, so `#jokes`
// and `jokes` are different channels.
inline QByteArray hashtagChannelKey(const QString& name) {
    return QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256)
        .left(ChannelSecretSize);
}

// Persistent channel identity is derived from the key, never from the device's
// slot number. Hashing keeps the key itself out of settings and message history
// while still following a channel if it moves to another slot.
inline QByteArray channelKeyFingerprint(const QByteArray& secret) {
    if (secret.size() != ChannelSecretSize) return {};
    return QCryptographicHash::hash(secret, QCryptographicHash::Sha256);
}

// Nothing on the wire says what kind of channel a slot holds — GET_CHANNEL
// answers with a name and a key and no more — so the kind is deduced from the
// key itself, which is exactly how the daemon builds them.
enum class ChannelType {
    Public,   // the well-known key every node ships with in slot 0
    Hashtag,  // key derived from the name: knowing `#jokes` is joining it
    Private,  // a key that arrived some other way
};

// One configured channel. `index` is only its current wire address; persistent
// state follows the key fingerprint so moving a channel between slots does not
// move another channel's history with it.
struct Channel {
    int index = 0;
    QString name;
    QByteArray secret;  // 16 bytes; all-zero means the slot is unused
    // Present only for a channel reconstructed from the offline cache, where
    // the real key deliberately is not stored. Live channels derive the same
    // value from `secret`.
    QByteArray cachedKeyFingerprint;
    // Classified once, when the key is in hand: the offline channel cache holds
    // names only, so a Channel rebuilt from it could not work this out itself.
    ChannelType type = ChannelType::Private;

    bool configured() const {
        if (secret.size() != ChannelSecretSize) return false;
        for (char c : secret)
            if (c != 0) return true;
        return false;
    }

    // Empty names are legal on the wire; show something selectable instead.
    QString displayName() const {
        return name.isEmpty() ? QStringLiteral("Channel %1").arg(index) : name;
    }

    QByteArray keyFingerprint() const {
        const QByteArray live = channelKeyFingerprint(secret);
        return live.isEmpty() ? cachedKeyFingerprint : live;
    }

    static ChannelType classify(const QString& name, const QByteArray& secret) {
        // Mirrors umeshcore's mesh::Channel: a key that can be re-derived from
        // something public is a public channel, and anything else arrived by a
        // route only the two ends know.
        if (secret == publicChannelKey()) return ChannelType::Public;
        if (secret == hashtagChannelKey(name)) return ChannelType::Hashtag;
        return ChannelType::Private;
    }

    // Best guess for a cache entry written before types were recorded, which is
    // wrong at worst until the device answers with the real keys.
    static ChannelType classifyByName(const QString& name) {
        if (name.startsWith(QLatin1Char('#'))) return ChannelType::Hashtag;
        if (name.compare(QStringLiteral("Public"), Qt::CaseInsensitive) == 0)
            return ChannelType::Public;
        return ChannelType::Private;
    }
};

// Settings hand back plain ints, and an unrecognised one must not index off the
// end of anything: treat it as an ordinary keyed channel.
inline ChannelType channelTypeFromInt(int value) {
    switch (value) {
        case int(ChannelType::Public): return ChannelType::Public;
        case int(ChannelType::Hashtag): return ChannelType::Hashtag;
        default: return ChannelType::Private;
    }
}

struct Message {
    // How far one of our own sends has got. Meaningless for incoming messages,
    // and for anything read back from history: the app writes a message down
    // only once the daemon has taken it, so a stored message is always sent.
    enum class SendState {
        Sent,     // the daemon acknowledged the command
        Pending,  // shown optimistically, still waiting for that answer
    };

    int channelIndex = 0;
    // Channel messages carry no per-sender key, only a name the sender put in
    // the text, so this is unauthenticated and must never be treated as identity.
    QString sender;
    QString text;
    QDateTime timestamp;
    bool outgoing = false;
    SendState sendState = SendState::Sent;
    // Ties the row on screen to the answer that will settle it. The protocol
    // tags nothing, so the app supplies its own tag; it lives only while the
    // send is in flight and is never persisted.
    int sendToken = 0;
    // Radio quality of the packet that carried it. Meaningless for our own
    // messages, so `hasSignal` gates display rather than a sentinel value.
    bool hasSignal = false;
    float snr = 0.0f;
    // 0xFF on the wire means "arrived by flood"; otherwise it is a hop count.
    int pathLen = 0xFF;

    bool flooded() const { return pathLen == 0xFF; }
};

}  // namespace model
