#pragma once

#include <QAbstractListModel>

#include "model/types.h"

namespace model {

// The address-book source model. Its base order is by name; a view may put a
// sort/filter proxy in front of it when the reader explicitly asks for a
// different order or only some node types.
class ContactModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        PublicKeyRole,   // the 32 raw bytes; the identity, not a display string
        TypeRole,        // model::ContactType
        LastAdvertRole,  // QDateTime, invalid when the node advertised no usable clock
        PathLenRole,     // hops home; 0xFF means anything sent there floods
    };

    explicit ContactModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void setContacts(const QVector<Contact>& contacts);
    // One contact an advert refreshed. Repaints its row rather than rebuilding
    // the list, so hearing from somebody does not throw away where the reader
    // had scrolled to; only a new node or a renamed one reorders anything.
    void upsert(const Contact& contact);

private:
    // Case-insensitive, because a list sorted by ASCII would file every
    // lowercase name after every uppercase one.
    static QString sortKey(const Contact& contact);
    int rowForKey(const QByteArray& pubkey) const;

    QVector<Contact> contacts_;
};

}  // namespace model
