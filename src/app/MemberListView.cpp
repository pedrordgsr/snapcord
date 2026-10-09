#include "MemberListView.h"

#include "Avatar.h"
#include "ImageCache.h"
#include "Motion.h"
#include "Theme.h"
#include "core/Session.h"

#include <QAbstractListModel>
#include <QCoreApplication>
#include <QHash>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>

namespace {

constexpr int RowHeight = 44;
constexpr int AvatarSize = 32;
constexpr int LoadingRows = 12; // placeholders shown until the Gateway sends the list

bool isOffline(UserStatus status)
{
    return status == UserStatus::Offline || status == UserStatus::Invisible || status == UserStatus::Unknown;
}

// The line under a member's name: their custom status, or else what they are playing, listening to...
QString activityText(const Presence& presence)
{
    if (const Activity* custom = presence.customStatus()) {
        // Custom emojis are images; only Unicode ones fit in the text.
        const QString emoji = custom->emojiId.isEmpty() ? custom->emojiName : QString();
        if (!custom->state.isEmpty() || !emoji.isEmpty())
            return emoji.isEmpty() ? custom->state : custom->state.isEmpty() ? emoji : emoji + u' ' + custom->state;
    }
    for (const Activity& activity : presence.activities) {
        switch (activity.type) {
        case Activity::Playing:
            return QCoreApplication::translate("MemberListView", "Playing %1").arg(activity.name);
        case Activity::Streaming:
            return QCoreApplication::translate("MemberListView", "Streaming %1").arg(activity.details.isEmpty() ? activity.name : activity.details);
        case Activity::Listening:
            return QCoreApplication::translate("MemberListView", "Listening to %1").arg(activity.name);
        case Activity::Watching:
            return QCoreApplication::translate("MemberListView", "Watching %1").arg(activity.name);
        case Activity::Competing:
            return QCoreApplication::translate("MemberListView", "Competing in %1").arg(activity.name);
        default:
            break;
        }
    }
    return {};
}

} // namespace

// Rows of the list straight from the Session; the view only keeps the row count in step.
class MemberListModel : public QAbstractListModel
{
public:
    MemberListModel(Session* session, QObject* parent)
        : QAbstractListModel(parent)
        , m_session(session)
    {
    }

    QString guildId() const { return m_guildId; }

    void setChannel(const QString& guildId, const QString& channelId)
    {
        beginResetModel();
        m_guildId = guildId;
        m_channelId = channelId;
        m_rows = currentRowCount();
        endResetModel();
    }

    // Brings the row count up to date (keeping the scroll position) and repaints every row.
    void refresh()
    {
        const int rows = currentRowCount();
        if (rows > m_rows) {
            beginInsertRows({}, m_rows, rows - 1);
            m_rows = rows;
            endInsertRows();
        } else if (rows < m_rows) {
            beginRemoveRows({}, rows, m_rows - 1);
            m_rows = rows;
            endRemoveRows();
        }
        if (m_rows > 0)
            emit dataChanged(index(0), index(m_rows - 1));
    }

    MemberListItem item(int row) const
    {
        const MemberList* list = m_session->memberList(m_guildId, m_channelId);
        return list ? list->items.value(row) : MemberListItem();
    }

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : m_rows; }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole)
            return {};
        // The full name, for rows too narrow to show all of it.
        const MemberListItem row = item(index.row());
        if (!row.isMember())
            return {};
        return row.nick.isEmpty() ? m_session->user(row.userId).displayName() : row.nick;
    }

private:
    int currentRowCount() const
    {
        const MemberList* list = m_session->memberList(m_guildId, m_channelId);
        return list ? int(list->items.size()) : LoadingRows;
    }

    Session* m_session;
    QString m_guildId;
    QString m_channelId;
    int m_rows = 0;
};

namespace {

// Paints group headers ("ONLINE — 12"), members (avatar with status, name in their role color, activity)
// and placeholders for rows that have not arrived yet.
class MemberDelegate : public QStyledItemDelegate
{
public:
    MemberDelegate(Session* session, ImageCache* images, MemberListModel* model, QAbstractItemView* view)
        : QStyledItemDelegate(view)
        , m_session(session)
        , m_images(images)
        , m_model(model)
        , m_animator(new Motion::ItemAnimator(view->viewport()))
    {
    }

    void clearAvatars() { m_avatars.clear(); }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return {0, RowHeight}; }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        painter->save();
        painter->setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
        const MemberListItem item = m_model->item(index.row());
        if (item.isGroup())
            paintGroup(painter, option, item);
        else if (item.isMember())
            paintMember(painter, option, item);
        else
            paintPlaceholder(painter, option, index.row());
        painter->restore();
    }

private:
    void paintGroup(QPainter* painter, const QStyleOptionViewItem& option, const MemberListItem& item) const
    {
        const Theme::Palette& colors = Theme::instance().palette();
        QString name;
        if (item.groupId == u"online") {
            name = QCoreApplication::translate("MemberListView", "Online");
        } else if (item.groupId == u"offline") {
            name = QCoreApplication::translate("MemberListView", "Offline");
        } else {
            const Guild* guild = m_session->guild(m_model->guildId());
            name = guild && guild->roles.contains(item.groupId) ? guild->roles.value(item.groupId).name : item.groupId;
        }
        QFont font = option.font;
        font.setPixelSize(12);
        font.setWeight(QFont::DemiBold);
        painter->setFont(font);
        painter->setPen(colors.textMuted);
        const QRect rect = option.rect.adjusted(16, 0, -8, -6);
        // Role groups show only the name; Online/Offline keep their counts.
        const bool roleGroup = item.groupId != u"online" && item.groupId != u"offline";
        const QString text = roleGroup ? name.toUpper()
                                       : QStringLiteral("%1 — %2").arg(name.toUpper()).arg(item.groupCount);
        painter->drawText(rect, Qt::AlignLeft | Qt::AlignBottom, painter->fontMetrics().elidedText(text, Qt::ElideRight, rect.width()));
    }

    void paintMember(QPainter* painter, const QStyleOptionViewItem& option, const MemberListItem& item) const
    {
        const Theme::Palette& colors = Theme::instance().palette();
        const bool hovered = option.state & QStyle::State_MouseOver;
        const qreal hover = m_animator->level(item.userId, hovered, option.rect);
        const QRect row = option.rect.adjusted(8, 1, -8, -1);
        if (hover > 0.0) {
            QColor background = colors.hover;
            background.setAlphaF(background.alphaF() * hover);
            painter->setPen(Qt::NoPen);
            painter->setBrush(background);
            painter->drawRoundedRect(row, 4, 4);
        }

        const User user = m_session->user(item.userId);
        const QString name = item.nick.isEmpty() ? user.displayName() : item.nick;
        const Presence presence = m_session->presence(item.userId);
        const bool offline = isOffline(presence.status);
        // Like Discord, offline members are faded until hovered.
        if (offline)
            painter->setOpacity(0.35 + 0.65 * hover);

        const QRect avatarRect(row.left() + 8, row.center().y() - AvatarSize / 2, AvatarSize, AvatarSize);
        painter->drawPixmap(avatarRect.topLeft(), avatar(user, name, offline ? UserStatus::Unknown : presence.status,
                                                         painter->device()->devicePixelRatioF()));

        const int textLeft = avatarRect.right() + 13;
        const int textWidth = row.right() - 8 - textLeft;
        const QString subtitle = offline ? QString() : activityText(presence);

        QFont font = option.font;
        font.setPixelSize(15);
        font.setWeight(QFont::Medium);
        painter->setFont(font);
        const QColor roleColor = this->roleColor(item.roleIds);
        painter->setPen(roleColor.isValid() ? roleColor : Motion::mix(colors.textMuted, colors.text, hover));
        const QRect nameRect = subtitle.isEmpty() ? QRect(textLeft, row.top(), textWidth, row.height())
                                                  : QRect(textLeft, row.top() + 3, textWidth, row.height() / 2);
        painter->drawText(nameRect, Qt::AlignLeft | (subtitle.isEmpty() ? Qt::AlignVCenter : Qt::AlignBottom),
                          painter->fontMetrics().elidedText(name, Qt::ElideRight, textWidth));
        if (subtitle.isEmpty())
            return;

        font.setPixelSize(12);
        font.setWeight(QFont::Normal);
        painter->setFont(font);
        painter->setPen(colors.textMuted);
        const QRect subtitleRect(textLeft, row.center().y() + 1, textWidth, row.height() / 2 - 2);
        painter->drawText(subtitleRect, Qt::AlignLeft | Qt::AlignTop,
                          painter->fontMetrics().elidedText(subtitle.simplified(), Qt::ElideRight, textWidth));
    }

    void paintPlaceholder(QPainter* painter, const QStyleOptionViewItem& option, int row) const
    {
        const Theme::Palette& colors = Theme::instance().palette();
        const QRect rect = option.rect.adjusted(8, 1, -8, -1);
        painter->setPen(Qt::NoPen);
        painter->setBrush(colors.hover);
        painter->drawEllipse(QRect(rect.left() + 8, rect.center().y() - AvatarSize / 2, AvatarSize, AvatarSize));
        // Bars of varying length, so the skeleton does not look like a grid.
        const int width = 60 + (row * 37) % 70;
        painter->drawRoundedRect(QRect(rect.left() + 8 + AvatarSize + 13, rect.center().y() - 5, width, 10), 5, 5);
    }

    // The color of the highest role that has one, like names in Discord.
    QColor roleColor(const QStringList& roleIds) const
    {
        const Guild* guild = m_session->guild(m_model->guildId());
        if (!guild)
            return {};
        const Role* best = nullptr;
        for (const QString& id : roleIds) {
            const auto it = guild->roles.constFind(id);
            if (it != guild->roles.cend() && it->color != 0 && (!best || it->position > best->position))
                best = &*it;
        }
        return best ? QColor::fromRgb(QRgb(best->color)) : QColor();
    }

    QPixmap avatar(const User& user, const QString& name, UserStatus status, qreal ratio) const
    {
        const QImage picture = m_images->image(ImageCache::avatarUrl(user));
        const QString key = QStringLiteral("%1/%2/%3/%4").arg(user.id).arg(int(status)).arg(picture.isNull()).arg(ratio);
        const auto it = m_avatars.constFind(key);
        if (it != m_avatars.cend())
            return *it;
        if (m_avatars.size() > 400)
            m_avatars.clear();
        const QPixmap pixmap = status == UserStatus::Unknown
            ? makeAvatar(name, picture, AvatarSize, ratio)
            : makeAvatar(name, picture, AvatarSize, ratio, false, Theme::instance().palette().bg1, status);
        m_avatars.insert(key, pixmap);
        return pixmap;
    }

    Session* m_session;
    ImageCache* m_images;
    MemberListModel* m_model;
    Motion::ItemAnimator* m_animator;
    mutable QHash<QString, QPixmap> m_avatars;
};

} // namespace

MemberListView::MemberListView(Session* session, ImageCache* images, QWidget* parent)
    : QListView(parent)
    , m_session(session)
    , m_model(new MemberListModel(session, this))
{
    setObjectName(QStringLiteral("memberList"));
    setFixedWidth(240);
    setFrameShape(QFrame::NoFrame);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::NoSelection);
    setFocusPolicy(Qt::NoFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    verticalScrollBar()->setSingleStep(RowHeight / 2);
    setMouseTracking(true);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setModel(m_model);
    auto* delegate = new MemberDelegate(session, images, m_model, this);
    setItemDelegate(delegate);

    // Bursts of Gateway events (a whole chunk of presences, say) end in one repaint.
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(50);
    connect(&m_refreshTimer, &QTimer::timeout, this, &MemberListView::refresh);
    auto scheduleRefresh = [this] {
        if (isVisible())
            m_refreshTimer.start();
    };
    connect(session, &Session::memberListChanged, this, [this, scheduleRefresh](const QString& guildId) {
        if (guildId == m_guildId)
            scheduleRefresh();
    });
    connect(session, &Session::presenceChanged, this, scheduleRefresh);
    connect(session, &Session::usersChanged, this, scheduleRefresh);
    connect(session, &Session::guildChanged, this, scheduleRefresh);
    connect(images, &ImageCache::imageLoaded, this, scheduleRefresh);
    connect(&Theme::instance(), &Theme::changed, this, [this, delegate] {
        delegate->clearAvatars();
        viewport()->update();
    });

    // Scrolling asks for the rows coming into view once it settles.
    m_subscribeTimer.setSingleShot(true);
    m_subscribeTimer.setInterval(150);
    connect(&m_subscribeTimer, &QTimer::timeout, this, &MemberListView::subscribe);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, &m_subscribeTimer, qOverload<>(&QTimer::start));

    connect(this, &QListView::clicked, this, [this](const QModelIndex& index) {
        const MemberListItem item = m_model->item(index.row());
        if (item.isMember())
            emit memberClicked(item.userId, viewport()->mapToGlobal(visualRect(index).topLeft()));
    });
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        const QString userId = userAt(position);
        if (!userId.isEmpty())
            emit memberContextMenuRequested(userId, viewport()->mapToGlobal(position));
    });
}

void MemberListView::setChannel(const QString& guildId, const QString& channelId)
{
    if (guildId != m_guildId || channelId != m_channelId) {
        m_guildId = guildId;
        m_channelId = channelId;
        m_model->setChannel(guildId, channelId);
        scrollToTop();
    }
    subscribe();
}

void MemberListView::showEvent(QShowEvent* event)
{
    QListView::showEvent(event);
    refresh();
}

void MemberListView::subscribe()
{
    const QModelIndex last = indexAt(QPoint(0, viewport()->height() - 1));
    const int lastRow = last.isValid() ? last.row() : m_model->rowCount() - 1;
    m_session->subscribeMemberList(m_guildId, m_channelId, std::max(0, lastRow));
}

void MemberListView::refresh()
{
    m_model->refresh();
    viewport()->update();
}

QString MemberListView::userAt(const QPoint& position) const
{
    const QModelIndex index = indexAt(position);
    return index.isValid() ? m_model->item(index.row()).userId : QString();
}
