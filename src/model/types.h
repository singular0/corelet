#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QHashFunctions>
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
        // Mirrors coreletd's mesh::Channel: a key that can be re-derived from
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

// What kind of node a contact is. Unlike a channel's kind, which has to be
// deduced from its key, this is on the wire: an advert declares it, and the
// numbers mirror the advert app-data types the daemon masks out of its flags.
enum class ContactType {
    Unknown,   // 0, and whatever a later firmware adds
    Chat,      // 1, somebody to talk to
    Repeater,  // 2, a mast that relays for everyone else
    Room,      // 3, a shared room to post in
    Sensor,    // 4, a node that reports readings
};

// The wire byte, and settings or a QVariant handing back a plain int, are both
// values this end does not control: anything unrecognised is a node of a kind
// this version has no word for, not an index off the end of something.
inline ContactType contactTypeFromInt(int value) {
    switch (value) {
        case int(ContactType::Chat): return ContactType::Chat;
        case int(ContactType::Repeater): return ContactType::Repeater;
        case int(ContactType::Room): return ContactType::Room;
        case int(ContactType::Sensor): return ContactType::Sensor;
        default: return ContactType::Unknown;
    }
}

// One node the device has heard an advert from. Unlike a channel message's
// sender, this is a real identity: an advert is signed by the key it carries,
// so the public key is what a contact *is* and the name is only what it calls
// itself.
struct Contact {
    QByteArray pubkey;  // 32 bytes
    QString name;
    ContactType type = ContactType::Unknown;
    // Hops in the route home. 0xFF means no route is known and anything sent
    // there floods the mesh; 0 is a direct neighbour.
    int pathLen = 0xFF;
    // The timestamp the node's most recent advert carried. That is the sender's
    // own clock rather than ours, and a node that has never been told the time
    // has none to give, so this can legitimately be invalid.
    QDateTime lastAdvert;

    bool flooded() const { return pathLen == 0xFF; }

    // An advert need not carry a name at all. The first bytes of the key are
    // how the daemon's logs name such a node, and they are at least unique.
    QString displayName() const {
        if (!name.isEmpty()) return name;
        if (pubkey.isEmpty()) return QStringLiteral("Unknown node");
        return QString::fromLatin1(pubkey.left(3).toHex());
    }
};

// What a conversation is with. Persisted as the number, so never renumber
// these; append only.
enum class ConversationKind {
    Channel = 0,  // everyone on a shared key
    Direct = 1,   // one peer, addressed by its own key
};

// Settings and a stored column both hand back plain ints, and an unrecognised
// one must not index off the end of anything.
inline ConversationKind conversationKindFromInt(int value) {
    return value == int(ConversationKind::Direct) ? ConversationKind::Direct
                                                  : ConversationKind::Channel;
}

// One conversation, by what kind it is and who it is with. This is the identity
// history is stored under and the sidebar is built from; a channel's slot number
// is only its current wire address and says nothing about which conversation it
// holds.
struct Conversation {
    // A channel is identified by the SHA-256 of its key and a peer by its
    // public key, so one size serves both.
    static constexpr int IdSize = 32;
    // What the wire gives for a direct message: the daemon matches the peer on
    // the first six bytes of its key and passes on no more than that. It is
    // also all CMD_SEND_TXT_MSG needs to address a reply, so a peer known only
    // by its prefix is a usable conversation rather than a broken one -- just
    // one with no name until the address book catches up with it.
    static constexpr int PeerPrefixSize = 6;

    ConversationKind kind = ConversationKind::Channel;
    QByteArray id;

    static Conversation channel(const QByteArray& keyFingerprint) {
        return {ConversationKind::Channel, keyFingerprint};
    }
    static Conversation direct(const QByteArray& peerKey) {
        return {ConversationKind::Direct, peerKey};
    }

    bool isChannel() const { return kind == ConversationKind::Channel; }
    bool isDirect() const { return kind == ConversationKind::Direct; }
    // Whether the whole identity is in hand, as against a peer still carrying
    // only the prefix the wire gave for it.
    bool resolved() const { return id.size() == IdSize; }
    bool isValid() const {
        return resolved() || (isDirect() && id.size() == PeerPrefixSize);
    }
    // How a peer is addressed on the wire, whether or not the whole key is
    // known here.
    QByteArray peerPrefix() const { return id.left(PeerPrefixSize); }

    bool operator==(const Conversation& other) const = default;
};

inline size_t qHash(const Conversation& conversation, size_t seed = 0) {
    return qHashMulti(seed, int(conversation.kind), conversation.id);
}

struct Message {
    // How far one of our own sends has got. Meaningless for incoming messages,
    // and for anything read back from history: the app writes a message down
    // only once the daemon has taken it, so a stored message is always sent.
    enum class SendState {
        Sent,     // the daemon acknowledged the command
        Pending,  // shown optimistically, still waiting for that answer
    };

    // Where this belongs. Invalid when the app could not work that out -- a slot
    // holding no configured channel -- which is a message to store out of the
    // way rather than one to drop, since collecting it took it off the node.
    Conversation conversation;
    // The wire slot a channel message arrived on, or -1 for a direct message.
    // Only an address, and only the current one: what the message belongs to is
    // `conversation`.
    int channelIndex = -1;
    // Channel messages carry no per-sender key, only a name the sender put in
    // the text, so this is unauthenticated and must never be treated as identity.
    // A direct message's sender is the peer, whose identity is the conversation
    // itself; this holds the name that peer went by when the message arrived.
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

// The sidebar carries a conversation through QAbstractItemModel::data(), which
// speaks only QVariant.
Q_DECLARE_METATYPE(model::Conversation)
