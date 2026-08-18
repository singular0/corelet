#include "ui/contacts_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QSortFilterProxyModel>
#include <QToolButton>
#include <QVBoxLayout>

#include "model/contact_model.h"
#include "protocol/client.h"
#include "ui/contact_delegate.h"
#include "ui/dialog_settings.h"
#include "ui/icons.h"
#include "ui/theme.h"

namespace {

class ContactSortFilterModel final : public QSortFilterProxyModel {
public:
    enum class SortMode {
        NameAscending,
        NameDescending,
        LastHeardDescending,
        LastHeardAscending,
    };

    explicit ContactSortFilterModel(QObject* parent = nullptr)
        : QSortFilterProxyModel(parent) {
        setDynamicSortFilter(true);
    }

    void setTypeVisible(model::ContactType type, bool visible) {
        const int flag = typeFlag(type);
        if (visible)
            visibleTypes_ |= flag;
        else
            visibleTypes_ &= ~flag;
        invalidate();
    }

    void setSortMode(SortMode mode) {
        if (sortMode_ == mode) return;
        sortMode_ = mode;
        invalidate();
        sort(0);
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override {
        const QModelIndex sourceIndex = sourceModel()->index(sourceRow, 0, sourceParent);
        const auto type = model::contactTypeFromInt(
            sourceIndex.data(model::ContactModel::TypeRole).toInt());
        return visibleTypes_ & typeFlag(type);
    }

    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override {
        if (sortMode_ == SortMode::LastHeardAscending ||
            sortMode_ == SortMode::LastHeardDescending) {
            const QDateTime leftTime =
                left.data(model::ContactModel::LastAdvertRole).toDateTime();
            const QDateTime rightTime =
                right.data(model::ContactModel::LastAdvertRole).toDateTime();

            // A missing advert time is not either old or new, so keep unknowns
            // after dated contacts in both directions.
            if (leftTime.isValid() != rightTime.isValid()) return leftTime.isValid();
            if (leftTime.isValid() && leftTime != rightTime) {
                return sortMode_ == SortMode::LastHeardAscending
                           ? leftTime < rightTime
                           : leftTime > rightTime;
            }
        }

        const QString leftName =
            left.data(model::ContactModel::NameRole).toString().toCaseFolded();
        const QString rightName =
            right.data(model::ContactModel::NameRole).toString().toCaseFolded();
        if (leftName != rightName) {
            if (sortMode_ == SortMode::NameDescending) return leftName > rightName;
            return leftName < rightName;
        }

        // Duplicate display names and equal timestamps are legal. Identity is
        // the final comparison so their order is deterministic.
        return left.data(model::ContactModel::PublicKeyRole).toByteArray() <
               right.data(model::ContactModel::PublicKeyRole).toByteArray();
    }

private:
    static int typeFlag(model::ContactType type) { return 1 << int(type); }

    static constexpr int AllContactTypes =
        (1 << (int(model::ContactType::Sensor) + 1)) - 1;

    int visibleTypes_ = AllContactTypes;
    SortMode sortMode_ = SortMode::NameAscending;
};

}  // namespace

ContactsDialog::ContactsDialog(proto::CompanionClient* client, QWidget* parent)
    : QDialog(parent), client_(client) {
    ui::configureDialogWindow(*this);
    setWindowTitle(QStringLiteral("Contacts"));

    model_ = new model::ContactModel(this);
    auto* visibleContacts = new ContactSortFilterModel(this);
    visibleContacts->setSourceModel(model_);
    visibleContacts->sort(0);

    list_ = new QListView;
    list_->setObjectName(QStringLiteral("contactList"));
    list_->setModel(visibleContacts);
    list_->setItemDelegate(new ContactDelegate(list_));
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Nothing can be done to a contact yet, so a row that highlighted under the
    // pointer would be offering something that is not there.
    list_->setSelectionMode(QAbstractItemView::NoSelection);
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // Every row is the same three lines tall, which is what lets the view size
    // the list from one of them instead of all hundred.
    list_->setUniformItemSizes(true);
    list_->setResizeMode(QListView::Adjust);

    placeholder_ = new QLabel;
    placeholder_->setWordWrap(true);
    placeholder_->setAlignment(Qt::AlignCenter);
    placeholder_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::TextMuted.name()));

    auto* controls = new QHBoxLayout;
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(4);

    struct TypeOption {
        model::ContactType type;
        const char* icon;
        const char* name;
        const char* tooltip;
    };
    const TypeOption typeOptions[] = {
        {model::ContactType::Chat, "users", "Chat contacts",
         "Show chat contacts: nodes representing people"},
        {model::ContactType::Repeater, "radio-tower", "Repeaters",
         "Show repeaters: nodes that relay mesh traffic"},
        {model::ContactType::Room, "house", "Rooms",
         "Show rooms: shared rooms for group posts"},
        {model::ContactType::Sensor, "gauge", "Sensors",
         "Show sensors: nodes that report readings"},
        {model::ContactType::Unknown, "circle-help", "Unknown contact types",
         "Show contact types this version does not recognise"},
    };
    const int filterIconSize = theme::scaled(font(), 16);
    for (const TypeOption& option : typeOptions) {
        auto* button = new QToolButton;
        button->setObjectName(QStringLiteral("filterButton"));
        button->setCheckable(true);
        button->setChecked(true);
        button->setAutoRaise(false);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize(QSize(filterIconSize, filterIconSize));
        button->setText(QString::fromLatin1(option.name));
        button->setToolTip(QString::fromLatin1(option.tooltip));

        QIcon icon;
        const QString iconName = QString::fromLatin1(option.icon);
        icon.addPixmap(icons::tinted(iconName, filterIconSize, theme::TextMuted,
                                    devicePixelRatioF()),
                       QIcon::Normal, QIcon::Off);
        icon.addPixmap(icons::tinted(iconName, filterIconSize, theme::Accent,
                                    devicePixelRatioF()),
                       QIcon::Normal, QIcon::On);
        icon.addPixmap(icons::tinted(iconName, filterIconSize, theme::Text,
                                    devicePixelRatioF()),
                       QIcon::Active, QIcon::Off);
        icon.addPixmap(icons::tinted(iconName, filterIconSize, theme::Accent,
                                    devicePixelRatioF()),
                       QIcon::Active, QIcon::On);
        button->setIcon(icon);

        controls->addWidget(button);
        connect(button, &QToolButton::toggled, this,
                [this, visibleContacts, type = option.type](bool checked) {
                    visibleContacts->setTypeVisible(type, checked);
                    updatePlaceholder();
                });
    }
    controls->addStretch(1);

    auto* sort = new QComboBox;
    sort->setToolTip(QStringLiteral("Sort contacts"));
    sort->setAccessibleName(QStringLiteral("Sort contacts"));
    sort->addItem(QStringLiteral("Name: A–Z"),
                  int(ContactSortFilterModel::SortMode::NameAscending));
    sort->addItem(QStringLiteral("Name: Z–A"),
                  int(ContactSortFilterModel::SortMode::NameDescending));
    sort->addItem(QStringLiteral("Last heard: newest"),
                  int(ContactSortFilterModel::SortMode::LastHeardDescending));
    sort->addItem(QStringLiteral("Last heard: oldest"),
                  int(ContactSortFilterModel::SortMode::LastHeardAscending));
    controls->addWidget(sort);
    connect(sort, &QComboBox::currentIndexChanged, this,
            [visibleContacts, sort](int index) {
                visibleContacts->setSortMode(
                    ContactSortFilterModel::SortMode(sort->itemData(index).toInt()));
            });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);
    layout->addLayout(controls);
    layout->addWidget(placeholder_, 1);
    layout->addWidget(list_, 1);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(client_, &proto::CompanionClient::contactsChanged, this,
            [this](const QVector<model::Contact>& contacts) {
                model_->setContacts(contacts);
                updatePlaceholder();
            });
    connect(client_, &proto::CompanionClient::contactChanged, this,
            [this](const model::Contact& contact) {
                model_->upsert(contact);
                updatePlaceholder();
            });
    // The link can go down with this window still up: "nothing heard yet" and
    // "not connected" are very different things to be told about an empty list.
    connect(client_, &proto::CompanionClient::stateChanged, this,
            [this] { updatePlaceholder(); });

    model_->setContacts(client_->contacts());
    updatePlaceholder();

    // Not lockDialogSize(): a list has no content height to be fixed to, and
    // this one is worth dragging taller on a screen with the room. Short enough
    // to leave the uConsole's 480 rows somewhere to put the title bar.
    resize(480, 380);
    setMinimumSize(320, 220);
}

void ContactsDialog::updatePlaceholder() {
    const bool empty = list_->model()->rowCount() == 0;
    list_->setVisible(!empty);
    placeholder_->setVisible(empty);
    if (!empty) return;

    if (model_->rowCount() != 0) {
        placeholder_->setText(QStringLiteral("No contacts match the selected types."));
    } else {
        placeholder_->setText(
            client_->state() == proto::CompanionClient::State::Ready
                ? QStringLiteral("Nothing heard yet. A node appears here the first time it "
                                 "advertises itself within reach.")
                : QStringLiteral("Not connected. The address book lives on the node."));
    }
}
