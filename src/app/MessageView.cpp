#include "MessageView.h"

#include "Avatar.h"
#include "ImageCache.h"
#include "Theme.h"
#include "core/GuildFolders.h"
#include "core/Markdown.h"
#include "core/MessageStore.h"
#include "core/Session.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <vector>

namespace {

constexpr int ContentLeft = 72;
constexpr int RightMargin = 24;
constexpr int AvatarSize = 40;
constexpr int GroupGapMs = 7 * 60 * 1000;
constexpr int MaxPictureWidth = 400;
constexpr int MaxPictureHeight = 300;
constexpr int MaxEmbedWidth = 432;
constexpr int InviteCardHeight = 110;
constexpr int ReactionHeight = 26;

const Theme::Palette& themeColors()
{
    return Theme::instance().palette();
}

QString documentStyle()
{
    return QStringLiteral("code, pre { font-family: Consolas, 'Cascadia Mono', 'Courier New', monospace; font-size: 13px; }"
                          "a { color: %1; text-decoration: none; }")
        .arg(themeColors().link.name(QColor::HexRgb));
}

QFont messageFont(const QFont& base, int pixelSize, QFont::Weight weight = QFont::Normal)
{
    QFont font = base;
    font.setPixelSize(pixelSize);
    font.setWeight(weight);
    return font;
}

QString formatTime(const QDateTime& time)
{
    const QLocale locale;
    const QDate today = QDate::currentDate();
    const QString clock = locale.toString(time.time(), QLocale::ShortFormat);
    if (time.date() == today)
        return QCoreApplication::translate("MessageView", "Today at %1").arg(clock);
    if (time.date() == today.addDays(-1))
        return QCoreApplication::translate("MessageView", "Yesterday at %1").arg(clock);
    return locale.toString(time.date(), QLocale::ShortFormat) + u' ' + clock;
}

QSize fitInto(int width, int height, int maxWidth, int maxHeight)
{
    if (width <= 0 || height <= 0)
        return {maxWidth, maxHeight / 2};
    const double scale = std::min({1.0, double(maxWidth) / width, double(maxHeight) / height});
    return {std::max(1, int(width * scale)), std::max(1, int(height * scale))};
}

// Direct video files get a still poster. Discord .mp4/.webm play in-app; other videos open in the browser.
bool isDirectVideoFile(const QString& url)
{
    const QString path = QUrl(url).path().toLower();
    return path.endsWith(u".mp4") || path.endsWith(u".webm") || path.endsWith(u".mov")
        || path.endsWith(u".m4v") || path.endsWith(u".mkv");
}

// media.discordapp.net (and cdn) accept width/height. images-ext-* often 404s when those are added.
bool supportsDiscordSizeQuery(const QString& url)
{
    const QString host = QUrl(url).host();
    return host == u"media.discordapp.net" || host.endsWith(u".media.discordapp.net")
        || host == u"cdn.discordapp.com" || host.endsWith(u".cdn.discordapp.com");
}

// Discord's media proxy resizes on the server. For videos, `format=jpeg` asks for a still poster —
// without it the proxy returns the mp4 itself and the image cache paints a black box.
QUrl previewUrl(const QString& proxyUrl, const QSize& size, bool videoPoster = false)
{
    QUrl url(proxyUrl);
    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("width"), QString::number(std::max(1, size.width())));
    query.addQueryItem(QStringLiteral("height"), QString::number(std::max(1, size.height())));
    if (videoPoster)
        query.addQueryItem(QStringLiteral("format"), QStringLiteral("jpeg"));
    url.setQuery(query);
    return url;
}

QUrl mediaPreview(const QString& url, const QSize& size, bool videoPoster = false)
{
    if (url.isEmpty())
        return {};
    const bool poster = videoPoster || isDirectVideoFile(url);
    return supportsDiscordSizeQuery(url) ? previewUrl(url, size, poster) : QUrl(url);
}

QString youtubeVideoId(const QUrl& url)
{
    const QString host = url.host().toLower();
    if (host == u"youtu.be" || host.endsWith(u".youtu.be")) {
        const QString id = url.path().mid(1).section(u'/', 0, 0);
        return id.section(u'?', 0, 0);
    }
    if (host.contains(u"youtube.com") || host.contains(u"youtube-nocookie.com")) {
        const QString path = url.path();
        if (path.startsWith(u"/embed/"))
            return path.mid(7).section(u'/', 0, 0);
        if (path.startsWith(u"/shorts/"))
            return path.mid(8).section(u'/', 0, 0);
        if (path.startsWith(u"/live/"))
            return path.mid(6).section(u'/', 0, 0);
        return QUrlQuery(url).queryItemValue(QStringLiteral("v"));
    }
    return {};
}

// Best still frame for an embed preview (YouTube CDN first — Discord's images-ext is unreliable).
QUrl embedPreviewSource(const Embed& embed, const QSize& size)
{
    const QString yt = [&] {
        QString id = youtubeVideoId(QUrl(embed.url));
        if (id.isEmpty())
            id = youtubeVideoId(QUrl(embed.videoUrl));
        return id;
    }();
    if (!yt.isEmpty())
        return QUrl(QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(yt));

    // Prefer the original host for non-Discord images (Twitch, etc.).
    if (!embed.imageOriginalUrl.isEmpty()) {
        const QString host = QUrl(embed.imageOriginalUrl).host();
        if (!host.contains(u"discordapp") && !host.contains(u"discord.com"))
            return QUrl(embed.imageOriginalUrl);
    }
    if (!embed.imageUrl.isEmpty())
        return mediaPreview(embed.imageUrl, size);
    // gifv / Discord-hosted video with no separate thumbnail: ask the proxy for a poster frame.
    if (isDirectVideoFile(embed.videoUrl) && supportsDiscordSizeQuery(embed.videoUrl))
        return previewUrl(embed.videoUrl, size, true);
    return {};
}

QString formatSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

} // namespace

// --- MessageModel -----------------------------------------------------------------------------------

MessageModel::MessageModel(MessageStore* store, QObject* parent)
    : QAbstractListModel(parent)
    , m_store(store)
{
    // Except for removals, the store has already changed when it signals, so rows are announced right away.
    connect(m_store, &MessageStore::reset, this, [this](const QString& channelId) {
        if (channelId != m_channelId)
            return;
        beginResetModel();
        endResetModel();
    });
    connect(m_store, &MessageStore::olderLoaded, this, [this](const QString& channelId, int count) {
        if (channelId != m_channelId || count <= 0)
            return;
        emit aboutToPrepend();
        beginInsertRows({}, 0, count - 1);
        endInsertRows();
        // The first old message may now start or continue a group with the previous first message.
        emit messageChanged(message(count).id);
        emit prepended();
    });
    connect(m_store, &MessageStore::inserted, this, [this](const QString& channelId, int index) {
        if (channelId != m_channelId)
            return;
        beginInsertRows({}, index, index);
        endInsertRows();
        if (index + 1 < rowCount())
            emit messageChanged(message(index + 1).id);
    });
    connect(m_store, &MessageStore::changed, this, [this](const QString& channelId, int index) {
        if (channelId != m_channelId)
            return;
        emit messageChanged(message(index).id);
        emit dataChanged(this->index(index), this->index(index));
    });
    // Removals are announced before the message leaves the store: views still read it while the row goes away.
    connect(m_store, &MessageStore::aboutToRemove, this, [this](const QString& channelId, int index) {
        if (channelId == m_channelId)
            beginRemoveRows({}, index, index);
    });
    connect(m_store, &MessageStore::removed, this, [this](const QString& channelId, int index) {
        if (channelId != m_channelId)
            return;
        endRemoveRows();
        if (index < rowCount())
            emit messageChanged(message(index).id);
    });
}

void MessageModel::setChannel(const QString& channelId)
{
    beginResetModel();
    m_channelId = channelId;
    endResetModel();
}

int MessageModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_store->messages(m_channelId).size());
}

QVariant MessageModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount())
        return {};
    if (role == Qt::UserRole)
        return message(index.row()).id;
    return {};
}

const Message& MessageModel::message(int row) const
{
    return m_store->messages(m_channelId).at(row);
}

int MessageModel::rowOf(const QString& messageId) const
{
    const auto& messages = m_store->messages(m_channelId);
    for (qsizetype i = messages.size() - 1; i >= 0; --i) {
        if (messages[i].id == messageId)
            return static_cast<int>(i);
    }
    return -1;
}

bool MessageModel::startsGroup(int row) const
{
    if (row == 0 || startsDay(row))
        return true;
    const Message& current = message(row);
    const Message& previous = message(row - 1);
    if (current.isSystemMessage() || previous.isSystemMessage() || current.type == Message::Reply)
        return true;
    return current.author.id != previous.author.id
        || previous.timestamp.msecsTo(current.timestamp) > GroupGapMs;
}

bool MessageModel::startsDay(int row) const
{
    return row == 0 || message(row).timestamp.date() != message(row - 1).timestamp.date();
}

// --- MessageDelegate --------------------------------------------------------------------------------

struct MessageDelegate::Layout
{
    struct Picture
    {
        QRect rect;
        QUrl source;
        QString imageUrl; // full-resolution URL for the in-app viewer
        QString openUrl;  // browser / download link (files, embed pages)
        bool file = false;
        bool video = false;     // direct file; Discord .mp4/.webm play in-app
        bool webEmbed = false;  // YouTube etc. — opened in the browser
        QString name;
        QString detail;
        int progress = -1; // upload progress (0-100) of a file being sent
    };
    struct EmbedBox
    {
        QRect box;
        QColor color;
        QString author;
        QRect authorRect;
        QString title;
        QString titleUrl;
        QRect titleRect;
        std::unique_ptr<QTextDocument> description;
        QPoint descriptionPos;
        bool hasPicture = false;
        Picture picture;
    };

    int width = 0;
    int height = 0;
    bool groupStart = false;
    bool dayStart = false;
    bool system = false;
    bool mentioned = false;
    int top = 0;
    QString dayText;
    QRect avatarRect;
    QRect nameRect;
    QString name;
    QString time;
    QRect replyRect;
    QString replyName;
    QString replyText;
    std::unique_ptr<QTextDocument> content;
    QPoint contentPos;
    QList<Picture> pictures;
    std::vector<EmbedBox> embeds;
    struct InviteBox
    {
        QRect box;
        QRect button;
        QString code;
    };
    QList<InviteBox> invites;
    QList<QRect> reactionRects;
    QString stickers;
    QRect stickerRect;
};

MessageDelegate::MessageDelegate(Session* session, ImageCache* images, MessageModel* model, QObject* parent)
    : QStyledItemDelegate(parent)
    , m_session(session)
    , m_images(images)
    , m_model(model)
    , m_layouts(400)
{
    connect(m_model, &MessageModel::messageChanged, this, &MessageDelegate::invalidate);
    connect(m_model, &QAbstractItemModel::modelReset, this, &MessageDelegate::invalidateAll);
}

MessageDelegate::~MessageDelegate() = default;

void MessageDelegate::invalidate(const QString& messageId)
{
    m_layouts.remove(messageId);
}

void MessageDelegate::invalidateAll()
{
    m_layouts.clear();
    m_avatars.clear();
}

void MessageDelegate::revealSpoilers(const QString& messageId)
{
    m_revealedSpoilers.insert(messageId);
    invalidate(messageId);
}

QPixmap MessageDelegate::avatar(const User& user, int size) const
{
    const QString key = user.id + u'/' + QString::number(size);
    const auto it = m_avatars.constFind(key);
    if (it != m_avatars.cend())
        return *it;
    const QImage picture = m_images->image(ImageCache::avatarUrl(user));
    const QPixmap pixmap = makeAvatar(user.displayName(), picture, size, 2.0);
    if (!picture.isNull())
        m_avatars.insert(key, pixmap); // placeholders are not cached, so the real picture replaces them
    return pixmap;
}

QString MessageDelegate::systemText(const Message& message) const
{
    const QString name = message.author.displayName();
    switch (message.type) {
    case Message::UserJoin:
        return tr("%1 joined the server.").arg(name);
    case Message::ChannelPinnedMessage:
        return tr("%1 pinned a message to this channel.").arg(name);
    case Message::Call:
        return tr("%1 started a call.").arg(name);
    case Message::RecipientAdd:
        return tr("%1 added someone to the group.").arg(name);
    case Message::RecipientRemove:
        return tr("%1 left the group.").arg(name);
    case Message::ChannelNameChange:
        return tr("%1 changed the channel name: %2").arg(name, message.content);
    default:
        return message.content.isEmpty() ? tr("%1 did something Snapcord can't show yet.").arg(name) : message.content;
    }
}

MessageDelegate::Layout& MessageDelegate::layout(const QModelIndex& index, int width) const
{
    const Message& message = m_model->message(index.row());
    const bool groupStart = m_model->startsGroup(index.row());
    const bool dayStart = m_model->startsDay(index.row());
    if (Layout* cached = m_layouts.object(message.id)) {
        if (cached->width == width && cached->groupStart == groupStart && cached->dayStart == dayStart)
            return *cached;
    }

    auto* l = new Layout;
    l->width = width;
    l->groupStart = groupStart;
    l->dayStart = dayStart;
    l->system = message.isSystemMessage();
    const QFont base = QGuiApplication::font();
    const int contentWidth = std::max(120, width - ContentLeft - RightMargin);

    int y = 0;
    if (dayStart) {
        l->dayText = QLocale().toString(message.timestamp.date(), QLocale::LongFormat);
        y += 40;
    }
    y += groupStart ? Theme::instance().messageGroupGap() : Theme::instance().messageTightGap();
    l->top = y;

    Markdown::Context context;
    context.selfUserId = m_session->self().id;
    context.revealSpoilers = m_revealedSpoilers.contains(message.id);
    context.userName = [this](const QString& id) { return m_session->user(id).displayName(); };
    const QString guildId = message.guildId;
    context.channelName = [this, guildId](const QString& id) {
        const Channel* channel = m_session->channel(guildId, id);
        return channel ? channel->name : QString();
    };
    context.roleName = [this, guildId](const QString& id) {
        const Guild* guild = m_session->guild(guildId);
        return guild ? guild->roles.value(id).name : QString();
    };
    l->mentioned = message.mentionedUserIds.contains(context.selfUserId) || message.mentionsEveryone;

    if (l->system) {
        l->avatarRect = QRect(ContentLeft - 40, y + 2, 16, 16);
        QFontMetrics metrics(messageFont(base, 15));
        l->name = systemText(message);
        const QRect bounds = metrics.boundingRect(QRect(ContentLeft, y, contentWidth, 10000), Qt::TextWordWrap, l->name);
        l->nameRect = QRect(ContentLeft, y, contentWidth, bounds.height());
        l->time = formatTime(message.timestamp);
        y += bounds.height() + 4;
        l->height = y;
        m_layouts.insert(message.id, l);
        return *l;
    }

    if (message.type == Message::Reply && (!message.referencedMessageId.isEmpty() || message.referencedDeleted)) {
        l->replyRect = QRect(ContentLeft, y, contentWidth, 20);
        if (message.referencedDeleted) {
            l->replyText = tr("Original message was deleted");
        } else {
            l->replyName = message.referencedAuthor.displayName();
            l->replyText = Markdown::toPlainText(message.referencedContent, context);
            if (l->replyText.isEmpty())
                l->replyText = tr("Click to see attachment");
        }
        y += 24;
    }

    if (groupStart) {
        l->avatarRect = QRect(16, y, AvatarSize, AvatarSize);
        l->name = message.author.displayName();
        l->time = formatTime(message.timestamp);
        const QFontMetrics nameMetrics(messageFont(base, 16, QFont::DemiBold));
        l->nameRect = QRect(ContentLeft, y, nameMetrics.horizontalAdvance(l->name), 22);
        y += 22;
    } else {
        l->time = QLocale().toString(message.timestamp.time(), QLocale::ShortFormat);
    }

    if (!message.content.isEmpty() || message.editedTimestamp.isValid()) {
        const Markdown::Result markdown = Markdown::toHtml(message.content, context);
        QString html = markdown.html;
        if (message.editedTimestamp.isValid())
            html += QStringLiteral(" <span style=\"font-size:10px;color:#949ba4;\">%1</span>").arg(tr("(edited)"));
        l->content = std::make_unique<QTextDocument>();
        l->content->setDefaultFont(messageFont(base, 15));
        l->content->setDefaultStyleSheet(documentStyle());
        l->content->setDocumentMargin(0);
        // Custom emoji images: real ones when loaded, transparent placeholders (same size) until then.
        for (const QUrl& url : markdown.images) {
            QImage image = m_images->image(url);
            if (image.isNull()) {
                image = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
            }
            l->content->addResource(QTextDocument::ImageResource, url, image);
        }
        l->content->setHtml(html);
        l->content->setTextWidth(contentWidth);
        l->contentPos = QPoint(ContentLeft, y);
        y += int(std::ceil(l->content->size().height()));
    }

    for (const Attachment& attachment : message.attachments) {
        Layout::Picture picture;
        picture.openUrl = attachment.url;
        if (attachment.isMedia()) {
            const QSize size = fitInto(attachment.width, attachment.height, std::min(MaxPictureWidth, contentWidth), MaxPictureHeight);
            picture.rect = QRect(ContentLeft, y + 4, size.width(), size.height());
            // Discord's media proxy still serves a still frame for videos when sized.
            picture.source = mediaPreview(attachment.proxyUrl.isEmpty() ? attachment.url : attachment.proxyUrl, size,
                                           attachment.isVideo());
            picture.imageUrl = attachment.url;
            picture.video = attachment.isVideo();
        } else {
            picture.file = true;
            picture.name = attachment.filename;
            picture.detail = formatSize(attachment.size);
            if (message.pending && message.uploadProgress >= 0) {
                picture.progress = message.uploadProgress;
                picture.detail = tr("Uploading… %1%").arg(message.uploadProgress) + QStringLiteral(" · ") + picture.detail;
            }
            picture.rect = QRect(ContentLeft, y + 4, std::min(400, contentWidth), 56);
        }
        l->pictures.append(picture);
        y += picture.rect.height() + 6;
    }

    for (const Embed& embed : message.embeds) {
        // Plain image/video links are shown as the picture alone, like Discord does.
        const bool mediaOnly = (embed.type == u"image" || embed.type == u"gifv" || embed.type == u"video")
            && (!embed.imageUrl.isEmpty() || !embed.videoUrl.isEmpty());
        Layout::EmbedBox box;
        const int boxWidth = std::min(MaxEmbedWidth, contentWidth);
        const int innerLeft = ContentLeft + (mediaOnly ? 0 : 16);
        const int innerWidth = boxWidth - (mediaOnly ? 0 : 32);
        int by = y + 4 + (mediaOnly ? 0 : 10);
        box.color = embed.color >= 0 ? QColor::fromRgb(QRgb(embed.color)) : themeColors().bg0;
        if (!mediaOnly) {
            if (!embed.authorName.isEmpty() || !embed.providerName.isEmpty()) {
                box.author = embed.authorName.isEmpty() ? embed.providerName : embed.authorName;
                box.authorRect = QRect(innerLeft, by, innerWidth, 18);
                by += 22;
            }
            if (!embed.title.isEmpty()) {
                box.title = embed.title;
                box.titleUrl = embed.url;
                const QFontMetrics metrics(messageFont(base, 15, QFont::DemiBold));
                const QRect bounds = metrics.boundingRect(QRect(innerLeft, by, innerWidth, 1000), Qt::TextWordWrap, embed.title);
                box.titleRect = QRect(innerLeft, by, innerWidth, bounds.height());
                by += bounds.height() + 4;
            }
            if (!embed.description.isEmpty()) {
                box.description = std::make_unique<QTextDocument>();
                box.description->setDefaultFont(messageFont(base, 14));
                box.description->setDefaultStyleSheet(documentStyle());
                box.description->setDocumentMargin(0);
                box.description->setHtml(Markdown::toHtml(embed.description, context).html);
                box.description->setTextWidth(innerWidth);
                box.descriptionPos = QPoint(innerLeft, by);
                by += int(std::ceil(box.description->size().height())) + 6;
            }
        }
        if (!embed.imageUrl.isEmpty() || !embed.videoUrl.isEmpty()) {
            box.hasPicture = true;
            const int iw = embed.imageWidth > 0 ? embed.imageWidth : 400;
            const int ih = embed.imageHeight > 0 ? embed.imageHeight : 300;
            const QSize size = embed.imageIsThumbnail && !mediaOnly
                ? fitInto(iw, ih, 80, 80)
                : fitInto(iw, ih, std::min(MaxPictureWidth, innerWidth), MaxPictureHeight);
            box.picture.rect = QRect(innerLeft, by, size.width(), size.height());
            // Never feed a video/page URL to the image cache — that paints a black box.
            box.picture.source = embedPreviewSource(embed, size);
            const bool playable = isDirectVideoFile(embed.videoUrl);
            box.picture.video = playable;
            // YouTube / Twitch / etc.: no in-app player, so clicking opens the page in the browser.
            box.picture.webEmbed = !playable
                && (!embed.videoUrl.isEmpty() || embed.type == u"video" || embed.type == u"gifv");
            if (playable)
                box.picture.imageUrl = embed.videoUrl;
            else if (!embed.imageUrl.isEmpty() && !box.picture.webEmbed)
                box.picture.imageUrl = embed.imageUrl;
            box.picture.openUrl = embed.url.isEmpty()
                ? (embed.imageUrl.isEmpty() ? embed.videoUrl : embed.imageUrl)
                : embed.url;
            by += size.height() + 6;
        }
        const int boxHeight = by - y - 4 + (mediaOnly ? 0 : 6);
        box.box = QRect(ContentLeft, y + 4, mediaOnly ? box.picture.rect.width() : boxWidth, boxHeight);
        y += boxHeight + 8;
        l->embeds.push_back(std::move(box));
    }

    // Invite links get a card with the server and a "Join" button, like Discord shows them.
    for (const QString& code : Invites::codesInMessage(message.content).mid(0, 3)) {
        Layout::InviteBox invite;
        invite.code = code;
        invite.box = QRect(ContentLeft, y + 4, std::min(MaxEmbedWidth, contentWidth), InviteCardHeight);
        invite.button = QRect(invite.box.right() - 16 - 92, invite.box.top() + 44 + 5, 92, 40);
        l->invites.append(invite);
        y += InviteCardHeight + 8;
    }

    if (!message.stickerNames.isEmpty()) {
        l->stickers = tr("Sticker: %1").arg(message.stickerNames.join(QStringLiteral(", ")));
        l->stickerRect = QRect(ContentLeft, y + 2, contentWidth, 20);
        y += 24;
    }

    if (!message.reactions.isEmpty()) {
        const QFontMetrics metrics(messageFont(base, 13, QFont::DemiBold));
        int x = ContentLeft;
        int rowTop = y + 4;
        for (const Reaction& reaction : message.reactions) {
            const int chipWidth = 8 + 18 + 6 + metrics.horizontalAdvance(QString::number(reaction.count)) + 8;
            if (x + chipWidth > ContentLeft + contentWidth && x > ContentLeft) {
                x = ContentLeft;
                rowTop += ReactionHeight + 4;
            }
            l->reactionRects.append(QRect(x, rowTop, chipWidth, ReactionHeight));
            x += chipWidth + 4;
        }
        y = rowTop + ReactionHeight + 2;
    }

    if (groupStart)
        y = std::max(y, l->avatarRect.bottom() + 1);
    l->height = y + 3;
    m_layouts.insert(message.id, l);
    return *l;
}

QSize MessageDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const
{
    return {m_viewWidth, layout(index, m_viewWidth).height};
}

static QColor memberNameColor(Session* session, const QString& guildId, const QString& userId,
                              const QStringList& onMessage)
{
    const int color = session ? session->memberColor(guildId, userId, onMessage) : 0;
    return color ? QColor::fromRgb(QRgb(color)) : QColor();
}

void MessageDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const Message& message = m_model->message(index.row());
    const Layout& l = layout(index, m_viewWidth);
    painter->save();
    painter->setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    painter->translate(option.rect.topLeft());
    const QFont base = option.font;

    if (l.dayStart) {
        const int lineY = 20;
        painter->setFont(messageFont(base, 12, QFont::DemiBold));
        const int textWidth = painter->fontMetrics().horizontalAdvance(l.dayText) + 16;
        const int center = l.width / 2;
        const auto& colors = themeColors();
        painter->setPen(colors.border);
        painter->drawLine(16, lineY, center - textWidth / 2, lineY);
        painter->drawLine(center + textWidth / 2, lineY, l.width - 16, lineY);
        painter->setPen(colors.textMuted);
        painter->drawText(QRect(center - textWidth / 2, lineY - 10, textWidth, 20), Qt::AlignCenter, l.dayText);
    }

    const auto& colors = themeColors();

    // Row background: mention highlight, then hover.
    const QRect row(0, l.top - (l.groupStart ? 2 : 0), l.width, l.height - l.top + (l.groupStart ? 2 : 0));
    if (l.mentioned) {
        QColor mention = colors.warning;
        mention.setAlpha(26);
        painter->fillRect(row, mention);
        painter->fillRect(QRect(row.left(), row.top(), 2, row.height()), colors.warning);
    } else if (option.state & QStyle::State_MouseOver) {
        painter->fillRect(row, colors.hover);
    }

    const QColor textColor = message.failed ? colors.danger : message.pending ? colors.textMuted : colors.text;

    if (l.system) {
        QIcon(QStringLiteral(":/icons/arrow-right.svg")).paint(painter, l.avatarRect);
        painter->setFont(messageFont(base, 15));
        painter->setPen(colors.textMuted);
        painter->drawText(l.nameRect, Qt::TextWordWrap, l.name);
        painter->restore();
        return;
    }

    if (!l.replyRect.isNull()) {
        // Connector from the avatar column to the replied message.
        painter->setPen(QPen(colors.button, 2));
        QPainterPath spine;
        spine.moveTo(36, l.replyRect.top() + 18);
        spine.lineTo(36, l.replyRect.center().y() + 2);
        spine.quadTo(36, l.replyRect.center().y() - 2, 42, l.replyRect.center().y() - 2);
        spine.lineTo(ContentLeft - 6, l.replyRect.center().y() - 2);
        painter->drawPath(spine);
        int x = l.replyRect.left();
        painter->setFont(messageFont(base, 13, QFont::DemiBold));
        if (!l.replyName.isEmpty()) {
            const QString replyGuild = message.guildId.isEmpty() ? m_guildId : message.guildId;
            const QColor replyColor = memberNameColor(m_session, replyGuild, message.referencedAuthor.id,
                                                      message.referencedMemberRoleIds);
            painter->setPen(replyColor.isValid() ? replyColor : colors.textBright);
            const QString name = u'@' + l.replyName;
            painter->drawText(QRect(x, l.replyRect.top(), 300, 20), Qt::AlignVCenter, name);
            x += painter->fontMetrics().horizontalAdvance(name) + 6;
        }
        painter->setFont(messageFont(base, 13));
        painter->setPen(colors.textMuted);
        painter->drawText(QRect(x, l.replyRect.top(), l.replyRect.right() - x, 20), Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(l.replyText, Qt::ElideRight, l.replyRect.right() - x));
    }

    if (l.groupStart) {
        painter->drawPixmap(l.avatarRect, avatar(message.author, AvatarSize));
        painter->setFont(messageFont(base, 16, QFont::DemiBold));
        const QString authorGuild = message.guildId.isEmpty() ? m_guildId : message.guildId;
        const QColor authorColor = memberNameColor(m_session, authorGuild, message.author.id, message.memberRoleIds);
        painter->setPen(authorColor.isValid() ? authorColor : colors.textBright);
        painter->drawText(l.nameRect, Qt::AlignVCenter, l.name);
        painter->setFont(messageFont(base, 12));
        painter->setPen(colors.textMuted);
        painter->drawText(QRect(l.nameRect.right() + 8, l.nameRect.top() + 1, 300, l.nameRect.height()), Qt::AlignVCenter, l.time);
    } else if (option.state & QStyle::State_MouseOver) {
        // Grouped messages show their time in the avatar column on hover.
        painter->setFont(messageFont(base, 11));
        painter->setPen(colors.textMuted);
        painter->drawText(QRect(0, l.top + 2, ContentLeft - 6, 18), Qt::AlignRight | Qt::AlignVCenter, l.time);
    }

    if (l.content) {
        painter->save();
        painter->translate(l.contentPos);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, textColor);
        const auto [from, to] = selectionRange(index.row(), l.content->characterCount() - 1);
        if (from < to) {
            QAbstractTextDocumentLayout::Selection selection;
            selection.cursor = QTextCursor(l.content.get());
            selection.cursor.setPosition(from);
            selection.cursor.setPosition(to, QTextCursor::KeepAnchor);
            QColor highlight = colors.accent;
            highlight.setAlpha(110);
            selection.format.setBackground(highlight);
            context.selections.append(selection);
        }
        l.content->documentLayout()->draw(painter, context);
        painter->restore();
    }

    auto drawPicture = [&](const Layout::Picture& picture) {
        if (picture.file) {
            painter->setPen(colors.border);
            painter->setBrush(colors.bg1);
            painter->drawRoundedRect(picture.rect, 6, 6);
            QIcon(QStringLiteral(":/icons/file.svg")).paint(painter, QRect(picture.rect.left() + 12, picture.rect.center().y() - 16, 24, 32));
            painter->setFont(messageFont(base, 15));
            painter->setPen(colors.link);
            const QRect nameRect(picture.rect.left() + 48, picture.rect.top() + 8, picture.rect.width() - 60, 22);
            painter->drawText(nameRect, Qt::AlignVCenter, painter->fontMetrics().elidedText(picture.name, Qt::ElideMiddle, nameRect.width()));
            painter->setFont(messageFont(base, 12));
            painter->setPen(colors.textMuted);
            painter->drawText(QRect(nameRect.left(), nameRect.bottom(), nameRect.width(), 18), Qt::AlignVCenter, picture.detail);
            if (picture.progress >= 0) {
                const QRect track(picture.rect.left() + 8, picture.rect.bottom() - 5, picture.rect.width() - 16, 3);
                painter->setPen(Qt::NoPen);
                painter->setBrush(colors.hover);
                painter->drawRoundedRect(track, 1.5, 1.5);
                painter->setBrush(colors.accent);
                painter->drawRoundedRect(QRect(track.topLeft(), QSize(track.width() * picture.progress / 100, track.height())), 1.5, 1.5);
            }
            return;
        }
        const QImage image = m_images->image(picture.source, picture.rect.size() * 2);
        QPainterPath clip;
        clip.addRoundedRect(picture.rect, 6, 6);
        painter->save();
        painter->setClipPath(clip);
        if (image.isNull())
            painter->fillRect(picture.rect, colors.bg1);
        else
            painter->drawImage(picture.rect, image);
        if (picture.video || picture.webEmbed) {
            // Soft play affordance so video previews read differently from stills.
            const int r = std::min(36, std::min(picture.rect.width(), picture.rect.height()) / 3);
            const QPoint c = picture.rect.center();
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(0, 0, 0, 140));
            painter->drawEllipse(c, r, r);
            QPainterPath triangle;
            const int s = r / 2;
            triangle.moveTo(c.x() - s / 2 + 2, c.y() - s);
            triangle.lineTo(c.x() - s / 2 + 2, c.y() + s);
            triangle.lineTo(c.x() + s, c.y());
            triangle.closeSubpath();
            painter->setBrush(Qt::white);
            painter->drawPath(triangle);
        }
        painter->restore();
    };
    for (const Layout::Picture& picture : l.pictures)
        drawPicture(picture);

    for (const Layout::EmbedBox& box : l.embeds) {
        const bool mediaOnly = box.title.isEmpty() && box.author.isEmpty() && !box.description && box.hasPicture
            && box.box.width() == box.picture.rect.width();
        if (!mediaOnly) {
            QPainterPath shape;
            shape.addRoundedRect(box.box, 4, 4);
            painter->fillPath(shape, colors.bg1);
            painter->fillRect(QRect(box.box.left(), box.box.top(), 4, box.box.height()), box.color);
        }
        if (!box.author.isEmpty()) {
            painter->setFont(messageFont(base, 13, QFont::DemiBold));
            painter->setPen(colors.textBright);
            painter->drawText(box.authorRect, Qt::AlignVCenter, painter->fontMetrics().elidedText(box.author, Qt::ElideRight, box.authorRect.width()));
        }
        if (!box.title.isEmpty()) {
            painter->setFont(messageFont(base, 15, QFont::DemiBold));
            painter->setPen(box.titleUrl.isEmpty() ? colors.textBright : colors.link);
            painter->drawText(box.titleRect, Qt::TextWordWrap, box.title);
        }
        if (box.description) {
            painter->save();
            painter->translate(box.descriptionPos);
            QAbstractTextDocumentLayout::PaintContext context;
            context.palette.setColor(QPalette::Text, colors.text);
            box.description->documentLayout()->draw(painter, context);
            painter->restore();
        }
        if (box.hasPicture)
            drawPicture(box.picture);
    }

    for (const Layout::InviteBox& invite : l.invites)
        paintInvite(painter, base, invite.box, invite.button, invite.code);

    if (!l.stickers.isEmpty()) {
        painter->setFont(messageFont(base, 13));
        painter->setPen(colors.textMuted);
        painter->drawText(l.stickerRect, Qt::AlignVCenter, l.stickers);
    }

    for (qsizetype i = 0; i < l.reactionRects.size() && i < message.reactions.size(); ++i) {
        const Reaction& reaction = message.reactions[i];
        const QRect chip = l.reactionRects[i];
        painter->setPen(reaction.me ? QPen(colors.accent) : Qt::NoPen);
        painter->setBrush(reaction.me ? colors.accentMuted : colors.bg1);
        painter->drawRoundedRect(QRectF(chip).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
        const QRect emojiRect(chip.left() + 8, chip.center().y() - 9, 18, 18);
        if (reaction.emoji.isCustom()) {
            const QImage image = m_images->image(QUrl(Markdown::customEmojiUrl(reaction.emoji.id, reaction.emoji.animated)));
            if (!image.isNull())
                painter->drawImage(emojiRect, image);
        } else {
            painter->setFont(messageFont(base, 14));
            painter->setPen(colors.text);
            painter->drawText(emojiRect.adjusted(-2, -2, 2, 2), Qt::AlignCenter, reaction.emoji.name);
        }
        painter->setFont(messageFont(base, 13, QFont::DemiBold));
        painter->setPen(reaction.me ? colors.textBright : colors.textDim);
        painter->drawText(QRect(emojiRect.right() + 6, chip.top(), chip.right() - emojiRect.right() - 6, chip.height()),
                          Qt::AlignVCenter, QString::number(reaction.count));
    }
    painter->restore();
}

MessageDelegate::Hit MessageDelegate::hitTest(const QModelIndex& index, const QRect& itemRect, const QPoint& position) const
{
    Hit hit;
    if (!index.isValid())
        return hit;
    const Message& message = m_model->message(index.row());
    const Layout& l = layout(index, m_viewWidth);
    const QPoint p = position - itemRect.topLeft();
    hit.messageId = message.id;

    for (const Layout::InviteBox& invite : l.invites) {
        const InviteInfo* info = m_session->cachedInvite(invite.code);
        if (info && invite.button.contains(p)) {
            hit.kind = Hit::Invite;
            hit.url = invite.code;
            return hit;
        }
    }
    for (qsizetype i = 0; i < l.reactionRects.size(); ++i) {
        if (l.reactionRects[i].contains(p)) {
            hit.kind = Hit::Reaction;
            hit.reactionIndex = static_cast<int>(i);
            return hit;
        }
    }
    for (const Layout::Picture& picture : l.pictures) {
        if (picture.rect.contains(p)) {
            if (picture.webEmbed) {
                hit.kind = Hit::Image;
                hit.url = picture.openUrl;
                hit.web = true;
                return hit;
            }
            hit.kind = picture.file ? Hit::File : Hit::Image;
            hit.url = picture.file || picture.imageUrl.isEmpty() ? picture.openUrl : picture.imageUrl;
            hit.video = picture.video;
            return hit;
        }
    }
    for (const Layout::EmbedBox& box : l.embeds) {
        if (box.hasPicture && box.picture.rect.contains(p)) {
            if (box.picture.webEmbed) {
                hit.kind = Hit::Image;
                hit.url = box.picture.openUrl;
                hit.web = true;
                return hit;
            }
            hit.kind = Hit::Image;
            hit.url = box.picture.imageUrl.isEmpty() ? box.picture.openUrl : box.picture.imageUrl;
            hit.video = box.picture.video;
            return hit;
        }
        if (!box.titleUrl.isEmpty() && box.titleRect.contains(p)) {
            hit.kind = Hit::Link;
            hit.url = box.titleUrl;
            return hit;
        }
        if (box.description) {
            const QString anchor = box.description->documentLayout()->anchorAt(p - box.descriptionPos);
            if (!anchor.isEmpty()) {
                hit.kind = Hit::Link;
                hit.url = anchor;
                return hit;
            }
        }
    }
    if (!l.system && (l.avatarRect.contains(p) || l.nameRect.contains(p)) && !message.author.id.isEmpty()) {
        hit.kind = Hit::Author;
        hit.url = message.author.id;
        return hit;
    }
    if (!l.replyRect.isNull() && l.replyRect.contains(p) && !message.referencedDeleted) {
        hit.kind = Hit::Reply;
        hit.url = message.referencedMessageId;
        return hit;
    }
    if (l.content) {
        const QString anchor = l.content->documentLayout()->anchorAt(p - l.contentPos);
        if (anchor == u"spoiler:") {
            hit.kind = Hit::Spoiler;
        } else if (!anchor.isEmpty()) {
            hit.kind = Hit::Link;
            hit.url = anchor;
        }
    }
    return hit;
}

MessageDelegate::TextPoint MessageDelegate::textPointAt(const QModelIndex& index, const QRect& itemRect,
                                                        const QPoint& position) const
{
    if (!index.isValid())
        return {};
    const Layout& l = layout(index, m_viewWidth);
    if (!l.content)
        return {};
    const QPoint p = position - itemRect.topLeft() - l.contentPos;
    TextPoint point;
    point.messageId = m_model->message(index.row()).id;
    const int end = l.content->characterCount() - 1;
    if (p.y() < 0)
        point.position = 0;
    else if (p.y() >= l.content->size().height())
        point.position = end;
    else
        point.position = std::clamp(l.content->documentLayout()->hitTest(p, Qt::FuzzyHit), 0, end);
    return point;
}

bool MessageDelegate::isOverText(const QModelIndex& index, const QRect& itemRect, const QPoint& position) const
{
    if (!index.isValid())
        return false;
    const Layout& l = layout(index, m_viewWidth);
    return l.content
           && l.content->documentLayout()->hitTest(position - itemRect.topLeft() - l.contentPos, Qt::ExactHit) >= 0;
}

std::pair<MessageDelegate::TextPoint, MessageDelegate::TextPoint> MessageDelegate::wordAt(const QModelIndex& index,
                                                                                          const TextPoint& point) const
{
    const Layout& l = layout(index, m_viewWidth);
    if (!l.content || !point.isValid())
        return {};
    QTextCursor cursor(l.content.get());
    cursor.setPosition(point.position);
    cursor.select(QTextCursor::WordUnderCursor);
    return {{point.messageId, cursor.selectionStart()}, {point.messageId, cursor.selectionEnd()}};
}

void MessageDelegate::setSelection(const TextPoint& anchor, const TextPoint& focus)
{
    m_selectionAnchor = anchor;
    m_selectionFocus = focus;
}

bool MessageDelegate::hasSelection() const
{
    if (!m_selectionAnchor.isValid() || !m_selectionFocus.isValid() || m_selectionAnchor == m_selectionFocus)
        return false;
    return m_model->rowOf(m_selectionAnchor.messageId) >= 0 && m_model->rowOf(m_selectionFocus.messageId) >= 0;
}

std::pair<int, int> MessageDelegate::selectionRange(int row, int length) const
{
    if (!m_selectionAnchor.isValid() || !m_selectionFocus.isValid())
        return {-1, -1};
    const int anchorRow = m_model->rowOf(m_selectionAnchor.messageId);
    const int focusRow = m_model->rowOf(m_selectionFocus.messageId);
    if (anchorRow < 0 || focusRow < 0)
        return {-1, -1};
    const bool forward = anchorRow < focusRow
                         || (anchorRow == focusRow && m_selectionAnchor.position <= m_selectionFocus.position);
    const TextPoint& first = forward ? m_selectionAnchor : m_selectionFocus;
    const TextPoint& last = forward ? m_selectionFocus : m_selectionAnchor;
    const int firstRow = std::min(anchorRow, focusRow);
    const int lastRow = std::max(anchorRow, focusRow);
    if (row < firstRow || row > lastRow)
        return {-1, -1};
    const int from = row == firstRow ? std::min(first.position, length) : 0;
    const int to = row == lastRow ? std::min(last.position, length) : length;
    return {from, to};
}

QString MessageDelegate::selectedText() const
{
    if (!hasSelection())
        return {};
    const int anchorRow = m_model->rowOf(m_selectionAnchor.messageId);
    const int focusRow = m_model->rowOf(m_selectionFocus.messageId);
    QStringList parts;
    for (int row = std::min(anchorRow, focusRow); row <= std::max(anchorRow, focusRow); ++row) {
        const Layout& l = layout(m_model->index(row), m_viewWidth);
        if (!l.content)
            continue;
        const auto [from, to] = selectionRange(row, l.content->characterCount() - 1);
        if (from >= to)
            continue;
        QTextCursor cursor(l.content.get());
        cursor.setPosition(from);
        cursor.setPosition(to, QTextCursor::KeepAnchor);
        // Custom emoji are inline images: drop their placeholder characters.
        parts.append(QTextDocumentFragment(cursor).toPlainText().remove(QChar::ObjectReplacementCharacter));
    }
    return parts.join(u'\n');
}

void MessageDelegate::paintInvite(QPainter* painter, const QFont& base, const QRect& box, const QRect& button,
                                  const QString& code) const
{
    const auto& colors = themeColors();
    const InviteInfo* invite = m_session->cachedInvite(code);
    const bool failed = !invite && m_session->inviteFailed(code);
    if (!invite && !failed && !m_requestedInvites.contains(code)) {
        // Looked up once; Session::inviteLoaded repaints the list when it arrives.
        m_requestedInvites.insert(code);
        m_session->fetchInvite(code, false, {});
    }

    painter->setPen(Qt::NoPen);
    painter->setBrush(colors.bg1);
    painter->drawRoundedRect(box, 8, 8);

    const bool groupInvite = invite && invite->type == 1;
    painter->setFont(messageFont(base, 12, QFont::Bold));
    painter->setPen(colors.textMuted);
    const QRect header(box.left() + 16, box.top() + 16, box.width() - 32, 16);
    painter->drawText(header, Qt::AlignVCenter,
                      (groupInvite ? tr("You've been invited to join a group DM") : tr("You've been invited to join a server")).toUpper());

    const QRect icon(box.left() + 16, box.top() + 44, 50, 50);
    const int textLeft = icon.right() + 16;
    const int textWidth = (invite ? button.left() - 12 : box.right() - 16) - textLeft;
    if (!invite) {
        painter->setBrush(colors.bg3);
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(icon, 16, 16);
        painter->setFont(messageFont(base, 16, QFont::DemiBold));
        painter->setPen(failed ? colors.danger : colors.textMuted);
        painter->drawText(QRect(textLeft, icon.top() + 2, textWidth, 24), Qt::AlignVCenter,
                          failed ? tr("Invalid Invite") : tr("Resolving invite…"));
        if (failed) {
            painter->setFont(messageFont(base, 13));
            painter->setPen(colors.textMuted);
            painter->drawText(QRect(textLeft, icon.top() + 26, textWidth, 20), Qt::AlignVCenter,
                              painter->fontMetrics().elidedText(tr("This invite may be expired, or you might not have permission to join."),
                                                                Qt::ElideRight, textWidth));
        }
        return;
    }

    const QString name = invite->guildName.isEmpty() ? invite->channelName : invite->guildName;
    const QImage picture = invite->guildIcon.isEmpty() ? QImage()
                                                       : m_images->image(ImageCache::guildIconUrl(invite->guildId, invite->guildIcon));
    QPainterPath shape;
    shape.addRoundedRect(icon, 16, 16);
    if (!picture.isNull()) {
        painter->save();
        painter->setClipPath(shape);
        painter->drawImage(icon, picture);
        painter->restore();
    } else {
        painter->setPen(Qt::NoPen);
        painter->setBrush(colors.bg3);
        painter->drawPath(shape);
        painter->setFont(messageFont(base, 16, QFont::DemiBold));
        painter->setPen(colors.text);
        QString initials;
        for (const QString& word : name.split(u' ', Qt::SkipEmptyParts)) {
            if (initials.size() < 3)
                initials += word.front();
        }
        painter->drawText(icon, Qt::AlignCenter, initials);
    }

    painter->setFont(messageFont(base, 16, QFont::DemiBold));
    painter->setPen(colors.textBright);
    painter->drawText(QRect(textLeft, icon.top() + 2, textWidth, 24), Qt::AlignVCenter,
                      painter->fontMetrics().elidedText(name, Qt::ElideRight, textWidth));

    // Online / member counts with Discord's dots.
    int x = textLeft;
    const int countsY = icon.top() + 28;
    painter->setFont(messageFont(base, 13));
    auto drawCount = [&](const QColor& dot, const QString& text) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(dot);
        painter->drawEllipse(QRectF(x, countsY + 6, 8, 8));
        x += 12;
        painter->setPen(colors.textMuted);
        painter->drawText(QRect(x, countsY, textWidth, 20), Qt::AlignVCenter, text);
        x += painter->fontMetrics().horizontalAdvance(text) + 12;
    };
    if (invite->onlineCount >= 0)
        drawCount(colors.success, tr("%n Online", nullptr, invite->onlineCount));
    if (invite->memberCount >= 0)
        drawCount(colors.textMuted, tr("%n Members", nullptr, invite->memberCount));

    const bool member = invite->guildId.isEmpty() ? m_session->privateChannel(invite->channelId) != nullptr
                                                  : m_session->guild(invite->guildId) != nullptr;
    painter->setPen(Qt::NoPen);
    painter->setBrush(member ? colors.button : colors.success);
    painter->drawRoundedRect(button, 4, 4);
    painter->setFont(messageFont(base, 14, QFont::DemiBold));
    painter->setPen(member ? colors.textBright : colors.onAccent);
    painter->drawText(button, Qt::AlignCenter, member ? tr("Joined") : tr("Join"));
}

// --- MessageListView --------------------------------------------------------------------------------

MessageListView::MessageListView(MessageDelegate* delegate, QWidget* parent)
    : QListView(parent)
    , m_delegate(delegate)
{
    setObjectName(QStringLiteral("messageList"));
    setItemDelegate(delegate);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    verticalScrollBar()->setSingleStep(24);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSelectionMode(QAbstractItemView::NoSelection);
    setFocusPolicy(Qt::NoFocus);
    setUniformItemSizes(false);
    setResizeMode(QListView::Adjust);
    setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_Hover);
    setFrameShape(QFrame::NoFrame);
    connect(&Theme::instance(), &Theme::changed, this, [this] {
        m_delegate->invalidateAll();
        viewport()->update();
    });
}

void MessageListView::copySelection() const
{
    const QString text = m_delegate->selectedText();
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
}

MessageDelegate::TextPoint MessageListView::textPointAt(const QPoint& position) const
{
    if (!model() || model()->rowCount() == 0)
        return {};
    // Dragging past the top or bottom of the list keeps selecting from the first or last visible row.
    const QPoint inside(position.x(), std::clamp(position.y(), 0, viewport()->height() - 1));
    QModelIndex index = indexAt(inside);
    if (!index.isValid())
        index = model()->index(model()->rowCount() - 1, 0); // the empty space below the newest message
    return m_delegate->textPointAt(index, visualRect(index), position);
}

void MessageListView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (m_delegate->hasSelection()) {
            m_delegate->clearSelection();
            viewport()->update();
        }
        m_selecting = false;
        m_pressPosition = event->position().toPoint();
        const auto kind = hitAt(m_pressPosition).kind;
        // Text (links included) starts a selection when dragged; pictures, buttons and names don't.
        const bool onText = kind == MessageDelegate::Hit::None || kind == MessageDelegate::Hit::Link
                            || kind == MessageDelegate::Hit::Spoiler;
        m_pressPoint = onText ? textPointAt(m_pressPosition) : MessageDelegate::TextPoint{};
    }
    QListView::mousePressEvent(event);
}

void MessageListView::mouseDoubleClickEvent(QMouseEvent* event)
{
    const QPoint position = event->position().toPoint();
    const QModelIndex index = indexAt(position);
    if (event->button() != Qt::LeftButton || !m_delegate->isOverText(index, visualRect(index), position)) {
        QListView::mouseDoubleClickEvent(event);
        return;
    }
    // A double click selects the word; dragging on extends the selection from there.
    const auto [start, end] = m_delegate->wordAt(index, m_delegate->textPointAt(index, visualRect(index), position));
    m_delegate->setSelection(start, end);
    m_pressPoint = start;
    m_selecting = true;
    emit selectionStarted();
    viewport()->update();
}

bool MessageListView::isAtBottom() const
{
    return verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 8;
}

MessageDelegate::Hit MessageListView::hitAt(const QPoint& position) const
{
    const QModelIndex index = indexAt(position);
    if (!index.isValid())
        return {};
    return m_delegate->hitTest(index, visualRect(index), position);
}

void MessageListView::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint position = event->position().toPoint();
    if ((event->buttons() & Qt::LeftButton) && m_pressPoint.isValid()) {
        if (!m_selecting && (position - m_pressPosition).manhattanLength() >= QApplication::startDragDistance()) {
            m_selecting = true;
            emit selectionStarted();
        }
        if (m_selecting) {
            // Scrolls along when the drag leaves the list at the top or bottom.
            if (position.y() < 0)
                verticalScrollBar()->setValue(verticalScrollBar()->value() + position.y());
            else if (position.y() >= viewport()->height())
                verticalScrollBar()->setValue(verticalScrollBar()->value() + position.y() - viewport()->height() + 1);
            const auto focus = textPointAt(position);
            if (focus.isValid()) {
                m_delegate->setSelection(m_pressPoint, focus);
                viewport()->update();
            }
            viewport()->setCursor(Qt::IBeamCursor);
            return;
        }
    }

    const auto hit = hitAt(position);
    if (hit.kind != MessageDelegate::Hit::None) {
        viewport()->setCursor(Qt::PointingHandCursor);
    } else {
        const QModelIndex index = indexAt(position);
        viewport()->setCursor(m_delegate->isOverText(index, visualRect(index), position) ? Qt::IBeamCursor
                                                                                         : Qt::ArrowCursor);
    }
    QListView::mouseMoveEvent(event);
}

void MessageListView::mouseReleaseEvent(QMouseEvent* event)
{
    QListView::mouseReleaseEvent(event);
    if (event->button() != Qt::LeftButton)
        return;
    m_pressPoint = {};
    if (m_selecting) {
        // The end of a selection drag, not a click on what's under the mouse.
        m_selecting = false;
        return;
    }
    const auto hit = hitAt(event->position().toPoint());
    switch (hit.kind) {
    case MessageDelegate::Hit::Link:
        // Mentions link to "user:<id>".
        if (hit.url.startsWith(u"user:"))
            emit userClicked(hit.url.mid(5), event->globalPosition().toPoint());
        else
            emit linkActivated(hit.url);
        break;
    case MessageDelegate::Hit::Image:
        emit imageActivated(hit.url, hit.video, hit.web);
        break;
    case MessageDelegate::Hit::File:
        emit linkActivated(hit.url);
        break;
    case MessageDelegate::Hit::Reaction:
        emit reactionClicked(hit.messageId, hit.reactionIndex);
        break;
    case MessageDelegate::Hit::Reply:
        emit replyClicked(hit.url);
        break;
    case MessageDelegate::Hit::Invite:
        emit inviteClicked(hit.url, hit.messageId);
        break;
    case MessageDelegate::Hit::Author:
        emit userClicked(hit.url, event->globalPosition().toPoint());
        break;
    case MessageDelegate::Hit::Spoiler:
        m_delegate->revealSpoilers(hit.messageId);
        doItemsLayout();
        break;
    case MessageDelegate::Hit::None:
        break;
    }
}

void MessageListView::contextMenuEvent(QContextMenuEvent* event)
{
    const QModelIndex index = indexAt(event->pos());
    if (index.isValid())
        emit messageContextMenuRequested(index.data(Qt::UserRole).toString(), event->globalPos());
}

void MessageListView::resizeEvent(QResizeEvent* event)
{
    const bool atBottom = isAtBottom();
    m_delegate->setViewWidth(viewport()->width());
    QListView::resizeEvent(event);
    if (atBottom)
        QTimer::singleShot(0, this, [this] { scrollToBottom(); });
}

void MessageListView::scrollContentsBy(int dx, int dy)
{
    QListView::scrollContentsBy(dx, dy);
    if (verticalScrollBar()->value() <= verticalScrollBar()->minimum() + 200)
        emit topReached();
}

void MessageListView::rowsInserted(const QModelIndex& parent, int start, int end)
{
    // New messages at the end keep the view pinned to the bottom if it was there.
    const bool appended = end == model()->rowCount() - 1;
    const bool atBottom = isAtBottom();
    QListView::rowsInserted(parent, start, end);
    if (appended && atBottom)
        QTimer::singleShot(0, this, [this] { scrollToBottom(); });
}
