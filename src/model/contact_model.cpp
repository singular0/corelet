#include "model/contact_model.h"

#include <algorithm>

namespace model {

ContactModel::ContactModel(QObject* parent) : QAbstractListModel(parent) {}

QString ContactModel::sortKey(const Contact& contact) {
    return contact.displayName().toCaseFolded();
}

int ContactModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(contacts_.size());
}

QVariant ContactModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= contacts_.size()) return {};
    const Contact& c = contacts_.at(index.row());

    switch (role) {
        case Qt::DisplayRole:
        case NameRole: return c.displayName();
        case PublicKeyRole: return c.pubkey;
        case TypeRole: return int(c.type);
        case LastAdvertRole: return c.lastAdvert;
        case PathLenRole: return c.pathLen;
        default: return {};
    }
}

void ContactModel::setContacts(const QVector<Contact>& contacts) {
    beginResetModel();
    contacts_ = contacts;
    std::sort(contacts_.begin(), contacts_.end(), [](const Contact& a, const Contact& b) {
        const QString ka = sortKey(a);
        const QString kb = sortKey(b);
        // Two nodes may well advertise the same name -- nothing stops them, and
        // an unnamed one is named after its key -- so the key breaks the tie and
        // the order stays the same from one enumeration to the next.
        return ka == kb ? a.pubkey < b.pubkey : ka < kb;
    });
    endResetModel();
}

int ContactModel::rowForKey(const QByteArray& pubkey) const {
    for (int i = 0; i < contacts_.size(); i++)
        if (contacts_.at(i).pubkey == pubkey) return i;
    return -1;
}

void ContactModel::upsert(const Contact& contact) {
    const int row = rowForKey(contact.pubkey);
    if (row >= 0 && sortKey(contacts_.at(row)) == sortKey(contact)) {
        // The usual case: another advert from a node already listed, which moves
        // its time and possibly its hop count and nothing else.
        contacts_[row] = contact;
        Q_EMIT dataChanged(index(row), index(row));
        return;
    }

    QVector<Contact> updated = contacts_;
    if (row >= 0)
        updated[row] = contact;
    else
        updated.append(contact);
    setContacts(updated);
}

}  // namespace model
