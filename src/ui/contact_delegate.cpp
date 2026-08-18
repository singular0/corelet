#include "ui/contact_delegate.h"

#include <QApplication>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QStyle>
#include <QWidget>

#include "model/contact_model.h"
#include "ui/icons.h"
#include "ui/row_format.h"
#include "ui/theme.h"

namespace {

// Empty for anything that should be drawn as a person instead. A node kind this
// version has no word for is drawn as a person too: an initial and a name say
// more about it than a glyph chosen to mean "no idea" would.
QString typeIcon(model::ContactType type) {
    switch (type) {
        case model::ContactType::Repeater: return QStringLiteral("radio-tower");
        case model::ContactType::Room: return QStringLiteral("house");
        case model::ContactType::Sensor: return QStringLiteral("gauge");
        case model::ContactType::Chat:
        case model::ContactType::Unknown: break;
    }
    return {};
}

// The bottom line: when the node was last heard from, and how far the packet
// that said so had come.
QString heardText(const QDateTime& lastAdvert, int pathLen) {
    const QString stamp = ui::activityStamp(lastAdvert);
    return QStringLiteral("%1 · %2")
        .arg(stamp.isEmpty() ? QStringLiteral("Advert time unknown")
                             : QStringLiteral("Heard %1").arg(stamp),
             ui::hopText(pathLen));
}

}  // namespace

ContactDelegate::ContactDelegate(QObject* parent)
    : QStyledItemDelegate(parent), avatar_(QApplication::font()) {
    nameFont_ = QApplication::font();
    nameFont_.setBold(true);
    metaFont_ = theme::secondaryFont(QApplication::font());
    keyFont_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    // Matched to the metadata line beside it rather than to whatever size the
    // fixed-pitch family happens to default to.
    if (metaFont_.pointSizeF() > 0.0)
        keyFont_.setPointSizeF(metaFont_.pointSizeF());
    else if (metaFont_.pixelSize() > 0)
        keyFont_.setPixelSize(metaFont_.pixelSize());
}

QSize ContactDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const {
    const int text = QFontMetrics(nameFont_).height() + LineGap +
                     QFontMetrics(keyFont_).height() + LineGap +
                     QFontMetrics(metaFont_).height();
    return QSize(0, qMax(text, theme::scaled(nameFont_, IconSize)) + 2 * VerticalPadding);
}

void ContactDelegate::paint(QPainter* p, const QStyleOptionViewItem& option,
                            const QModelIndex& index) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);

    // Only ever true when this list is being asked to choose somebody, which is
    // the one time a contact row means anything to click. Drawn exactly as the
    // sidebar draws its selection so picking a person and opening a
    // conversation with one look like the same act.
    if (option.state & QStyle::State_Selected) {
        p->fillRect(option.rect, theme::SidebarSelected);
        p->fillRect(QRect(option.rect.left(), option.rect.top(), 2, option.rect.height()),
                    theme::Accent);
    }

    QRect r = option.rect.adjusted(10, 0, -10, 0);

    // --- the disc, spanning all three lines ----------------------------------
    const auto type = model::contactTypeFromInt(index.data(model::ContactModel::TypeRole).toInt());
    const QString name = index.data(model::ContactModel::NameRole).toString();
    const int iconSize = theme::scaled(nameFont_, IconSize);
    const QRect iconRect(r.left(), r.top() + (r.height() - iconSize) / 2, iconSize, iconSize);

    const QString glyph = typeIcon(type);
    if (glyph.isEmpty()) {
        avatar_.paint(p, iconRect, name);
    } else {
        // The sidebar's channel disc exactly: infrastructure is not somebody, so
        // it does not get a name colour of its own.
        const qreal dpr = option.widget ? option.widget->devicePixelRatioF() : 1.0;
        const int glyphInset = theme::scaled(nameFont_, GlyphInset);
        p->setPen(Qt::NoPen);
        p->setBrush(theme::IconBackground);
        p->drawEllipse(iconRect);
        const QRect glyphRect = iconRect.adjusted(glyphInset, glyphInset, -glyphInset, -glyphInset);
        p->drawPixmap(glyphRect,
                      icons::tinted(glyph, glyphRect.width(), theme::TextMuted, dpr));
    }
    r.setLeft(iconRect.right() + 10);

    const QFontMetrics nameFm(nameFont_);
    const QFontMetrics keyFm(keyFont_);
    const QFontMetrics metaFm(metaFont_);

    // All three lines as a block, centred against the disc beside them.
    const int block = nameFm.height() + LineGap + keyFm.height() + LineGap + metaFm.height();
    int y = r.top() + (r.height() - block) / 2;

    p->setFont(nameFont_);
    p->setPen(theme::Text);
    p->drawText(QRect(r.left(), y, r.width(), nameFm.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                nameFm.elidedText(name, Qt::ElideRight, r.width()));
    y += nameFm.height() + LineGap;

    // The full key is too long to scan in a list. Keep enough from both ends to
    // compare it with another client while making the abbreviation explicit.
    const QString keyHex = QString::fromLatin1(
        index.data(model::ContactModel::PublicKeyRole).toByteArray().toHex());
    const QString key = keyHex.size() > 16
                            ? QStringLiteral("<%1...%2>").arg(keyHex.left(8), keyHex.right(8))
                            : QStringLiteral("<%1>").arg(keyHex);
    p->setFont(keyFont_);
    p->setPen(theme::TextMuted);
    p->drawText(QRect(r.left(), y, r.width(), keyFm.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                keyFm.elidedText(key, Qt::ElideMiddle, r.width()));
    y += keyFm.height() + LineGap;

    const QString heard = heardText(index.data(model::ContactModel::LastAdvertRole).toDateTime(),
                                    index.data(model::ContactModel::PathLenRole).toInt());
    p->setFont(metaFont_);
    p->setPen(theme::TextMuted);
    p->drawText(QRect(r.left(), y, r.width(), metaFm.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                metaFm.elidedText(heard, Qt::ElideRight, r.width()));

    p->restore();
}
