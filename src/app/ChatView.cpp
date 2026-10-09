#include "ChatView.h"

#include "AttachmentTray.h"
#include "EmojiPicker.h"
#include "ImageCache.h"
#include "DiscordVideo.h"
#include "ImageViewer.h"
#include "MemberListView.h"
#include "Motion.h"
#include "MentionPopup.h"
#include "MessageView.h"
#include "Theme.h"
#include "VoiceController.h"
#include "core/Markdown.h"
#include "core/MessageStore.h"
#include "core/Permissions.h"
#include "core/Session.h"
#include "core/UploadLimits.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QPointer>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSettings>
#include <QTextBlock>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr qint64 TypingDurationMs = 10000;
constexpr auto MemberListKey = "ui/memberList";
const char* const QuickReactions[] = {"👍", "❤️", "😂", "😮", "😢", "🙏", "🔥", "🎉"};
constexpr int MaxUserSuggestions = 10;
constexpr int MaxRoleSuggestions = 5;
constexpr int MaxChannelSuggestions = 10;
// Longest "@name" or "#name" being completed.
constexpr int MaxMentionQuery = 32;

QString formatSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 0, QLocale::DataSizeTraditionalFormat);
}

// How well `name` matches what was typed: 0 starts with it, 1 contains it, 2 no match.
int matchRank(const QString& name, const QString& needle)
{
    if (name.isEmpty())
        return 2;
    if (needle.isEmpty() || name.startsWith(needle, Qt::CaseInsensitive))
        return 0;
    return name.contains(needle, Qt::CaseInsensitive) ? 1 : 2;
}

QStringList localFiles(const QMimeData* data)
{
    QStringList files;
    for (const QUrl& url : data->urls()) {
        if (url.isLocalFile())
            files.append(url.toLocalFile());
    }
    return files;
}

} // namespace

// --- Composer ---------------------------------------------------------------------------------------

Composer::Composer(QWidget* parent)
    : QPlainTextEdit(parent)
{
    setObjectName(QStringLiteral("composer"));
    setTabChangesFocus(true);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    document()->setDocumentMargin(10);
    connect(this, &QPlainTextEdit::textChanged, this, &Composer::adjustHeight);
    adjustHeight();
}

void Composer::adjustHeight()
{
    // Grow with the text up to about ten lines, then scroll.
    const int lines = std::clamp(static_cast<int>(document()->size().height()), 1, 10);
    setFixedHeight(lines * fontMetrics().lineSpacing() + 22);
}

void Composer::keyPressEvent(QKeyEvent* event)
{
    if (m_popup && m_popup->handleKey(event))
        return;
    if (event->matches(QKeySequence::Copy) && !textCursor().hasSelection() && m_copyFallback && m_copyFallback())
        return;
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !(event->modifiers() & Qt::ShiftModifier)) {
        // May be empty: a message can be only attachments; the chat view decides.
        emit submitted(toPlainText().trimmed());
        return;
    }
    if (event->key() == Qt::Key_Up && toPlainText().isEmpty()) {
        emit editLastRequested();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        emit cancelRequested();
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

bool Composer::canInsertFromMimeData(const QMimeData* source) const
{
    return !localFiles(source).isEmpty() || source->hasImage() || QPlainTextEdit::canInsertFromMimeData(source);
}

void Composer::insertFromMimeData(const QMimeData* source)
{
    const QStringList files = localFiles(source);
    if (!files.isEmpty()) {
        emit filesPasted(files);
        return;
    }
    // Spreadsheets copy a picture of the cells along with their text; the text is what people mean then.
    if (source->hasImage() && !source->hasText()) {
        const QImage image = qvariant_cast<QImage>(source->imageData());
        if (!image.isNull()) {
            emit imagePasted(image);
            return;
        }
    }
    QPlainTextEdit::insertFromMimeData(source);
}

// --- ChatView ---------------------------------------------------------------------------------------

void ChatView::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const Theme::Settings& appearance = Theme::instance().settings();
    const Theme::Palette& colors = Theme::instance().palette();
    painter.fillRect(rect(), colors.bg2);

    if (appearance.gradientEnabled) {
        QLinearGradient gradient(0, 0, 0, height());
        gradient.setColorAt(0, appearance.gradientTop);
        gradient.setColorAt(1, appearance.gradientBottom);
        painter.fillRect(rect(), gradient);
    }
}

ChatView::ChatView(Session* session, ImageCache* images, VoiceController* voice, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
    , m_images(images)
    , m_voice(voice)
    , m_model(new MessageModel(session->messages(), this))
    , m_delegate(new MessageDelegate(session, images, m_model, this))
    , m_list(new MessageListView(m_delegate))
    , m_icon(new QLabel)
    , m_title(new QLabel)
    , m_topic(new QLabel)
    , m_callButton(new QPushButton)
    , m_membersButton(new QToolButton)
    , m_memberList(new MemberListView(session, images))
    , m_modeBar(new QWidget)
    , m_modeLabel(new QLabel)
    , m_composer(new Composer)
    , m_emojiButton(new QToolButton)
    , m_attachButton(new QToolButton)
    , m_tray(new AttachmentTray)
    , m_mentionPopup(new MentionPopup(this))
    , m_statusLabel(new QLabel)
{
    setObjectName(QStringLiteral("chatArea"));
    setAttribute(Qt::WA_StyledBackground, false);
    setAcceptDrops(true);
    m_list->setModel(m_model);
    connect(&Theme::instance(), &Theme::changed, this, QOverload<>::of(&QWidget::update));

    // Header.
    auto* header = new QWidget;
    header->setObjectName(QStringLiteral("chatHeader"));
    header->setAttribute(Qt::WA_StyledBackground);
    header->setFixedHeight(48);
    m_icon->setFixedSize(24, 24);
    m_title->setObjectName(QStringLiteral("chatTitle"));
    m_topic->setObjectName(QStringLiteral("chatTopic"));
    m_callButton->setObjectName(QStringLiteral("headerCallButton"));
    m_callButton->setIcon(QIcon(QStringLiteral(":/icons/call.svg")));
    m_callButton->setCursor(Qt::PointingHandCursor);
    connect(m_callButton, &QPushButton::clicked, this, [this] { m_voice->startCall(m_channelId); });
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 0, 16, 0);
    headerLayout->setSpacing(8);
    headerLayout->addWidget(m_icon);
    headerLayout->addWidget(m_title);
    headerLayout->addWidget(m_topic, 1);
    headerLayout->addWidget(m_callButton);
    headerLayout->addWidget(m_membersButton);

    m_membersButton->setObjectName(QStringLiteral("headerIconButton"));
    m_membersButton->setIcon(QIcon(QStringLiteral(":/icons/members.svg")));
    m_membersButton->setIconSize(QSize(22, 22));
    m_membersButton->setCursor(Qt::PointingHandCursor);
    m_membersButton->setCheckable(true);
    m_membersButton->setChecked(QSettings().value(QLatin1String(MemberListKey), true).toBool());
    connect(m_membersButton, &QToolButton::toggled, this, [this](bool shown) {
        QSettings().setValue(QLatin1String(MemberListKey), shown);
        Motion::slidePanel(this, m_memberList, [this] { updateMemberList(); });
    });
    connect(m_memberList, &MemberListView::memberClicked, this, [this](const QString& userId, const QPoint& position) {
        emit memberProfileRequested(userId, m_guildId, position);
    });
    connect(m_memberList, &MemberListView::memberContextMenuRequested, this, &ChatView::memberContextMenuRequested);

    // Reply / edit bar above the composer.
    m_modeBar->setObjectName(QStringLiteral("composerModeBar"));
    m_modeBar->setAttribute(Qt::WA_StyledBackground);
    m_modeLabel->setObjectName(QStringLiteral("composerModeLabel"));
    auto* cancel = new QToolButton;
    cancel->setObjectName(QStringLiteral("composerModeCancel"));
    cancel->setText(QStringLiteral("✕"));
    cancel->setCursor(Qt::PointingHandCursor);
    connect(cancel, &QToolButton::clicked, this, &ChatView::cancelMode);
    auto* modeLayout = new QHBoxLayout(m_modeBar);
    modeLayout->setContentsMargins(16, 6, 12, 6);
    modeLayout->addWidget(m_modeLabel, 1);
    modeLayout->addWidget(cancel);
    m_modeBar->hide();

    // Composer row.
    m_emojiButton->setObjectName(QStringLiteral("emojiButton"));
    m_emojiButton->setText(QStringLiteral("🙂"));
    m_emojiButton->setCursor(Qt::PointingHandCursor);
    m_emojiButton->setToolTip(tr("Select emoji"));
    connect(m_emojiButton, &QToolButton::clicked, this, [this] {
        auto* picker = new EmojiPicker(m_session, m_images, m_guildId, this);
        connect(picker, &EmojiPicker::picked, this, [this](const QString& text) {
            m_composer->insertPlainText(text);
            m_composer->setFocus();
        });
        picker->popupAt(m_emojiButton->mapToGlobal(QPoint(m_emojiButton->width(), 0)));
    });
    m_attachButton->setObjectName(QStringLiteral("attachButton"));
    m_attachButton->setIcon(QIcon(QStringLiteral(":/icons/plus.svg")));
    m_attachButton->setIconSize(QSize(20, 20));
    m_attachButton->setCursor(Qt::PointingHandCursor);
    m_attachButton->setToolTip(tr("Upload a File"));
    connect(m_attachButton, &QToolButton::clicked, this, &ChatView::chooseFiles);
    connect(m_tray, &AttachmentTray::removeRequested, this, [this](int index) {
        if (index >= 0 && index < m_files.size()) {
            m_files.removeAt(index);
            updateAttachments();
        }
        m_composer->setFocus();
    });

    m_inputBox = new QWidget;
    m_inputBox->setObjectName(QStringLiteral("composerBox"));
    m_inputBox->setAttribute(Qt::WA_StyledBackground);
    auto* inputRow = new QHBoxLayout;
    inputRow->setContentsMargins(8, 0, 8, 0);
    inputRow->setSpacing(0);
    inputRow->addWidget(m_attachButton, 0, Qt::AlignBottom);
    inputRow->addWidget(m_composer, 1);
    inputRow->addWidget(m_emojiButton, 0, Qt::AlignBottom);
    auto* inputLayout = new QVBoxLayout(m_inputBox);
    inputLayout->setContentsMargins(0, 0, 0, 0);
    inputLayout->setSpacing(0);
    inputLayout->addWidget(m_tray);
    inputLayout->addLayout(inputRow);

    m_statusLabel->setObjectName(QStringLiteral("typingLabel"));
    m_statusLabel->setFixedHeight(22);

    auto* bottom = new QVBoxLayout;
    bottom->setContentsMargins(16, 0, 16, 0);
    bottom->setSpacing(0);
    bottom->addWidget(m_modeBar);
    bottom->addWidget(m_inputBox);
    bottom->addWidget(m_statusLabel);

    // The member list sits next to the messages, under the header.
    auto* messages = new QVBoxLayout;
    messages->setContentsMargins(0, 0, 0, 0);
    messages->setSpacing(0);
    messages->addWidget(m_list, 1);
    messages->addLayout(bottom);
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addLayout(messages, 1);
    body->addWidget(m_memberList);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addLayout(body, 1);

    // Messages.
    connect(m_composer, &Composer::submitted, this, &ChatView::submit);
    connect(m_composer, &Composer::cancelRequested, this, &ChatView::cancelMode);
    connect(m_composer, &Composer::editLastRequested, this, &ChatView::editLastMessage);
    connect(m_composer, &Composer::filesPasted, this, &ChatView::addFiles);
    connect(m_composer, &Composer::imagePasted, this, &ChatView::addPastedImage);

    // Mentions: typing "@name" or "#channel" opens a list of matches above the composer.
    m_composer->setMentionPopup(m_mentionPopup);
    connect(m_mentionPopup, &MentionPopup::picked, this, &ChatView::insertMention);
    connect(m_composer, &QPlainTextEdit::textChanged, this, &ChatView::updateMentionPopup);
    connect(m_composer, &QPlainTextEdit::cursorPositionChanged, this, &ChatView::updateMentionPopup);
    m_memberSearchTimer.setSingleShot(true);
    m_memberSearchTimer.setInterval(300);
    connect(&m_memberSearchTimer, &QTimer::timeout, this, [this] { m_session->searchGuildMembers(m_guildId, m_memberQuery); });
    connect(m_session, &Session::usersChanged, this, [this] {
        if (m_mentionPopup->isVisible())
            updateMentionPopup();
        m_list->viewport()->update();
    });
    auto resolveAuthors = [this](const QString& channelId) {
        if (channelId == m_channelId)
            m_session->ensureAuthorRoles(m_guildId, channelId);
    };
    connect(m_session->messages(), &MessageStore::reset, this, resolveAuthors);
    connect(m_session->messages(), &MessageStore::olderLoaded, this, [resolveAuthors](const QString& channelId, int) {
        resolveAuthors(channelId);
    });
    connect(m_session->messages(), &MessageStore::inserted, this, [resolveAuthors](const QString& channelId, int) {
        resolveAuthors(channelId);
    });
    connect(m_session, &Session::memberListChanged, m_list->viewport(), qOverload<>(&QWidget::update));
    connect(m_session, &Session::guildChanged, m_list->viewport(), qOverload<>(&QWidget::update));
    connect(m_composer, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_restoringDraft && m_editing.isEmpty() && !m_composer->toPlainText().isEmpty())
            m_session->sendTyping(m_channelId);
    });
    connect(m_list, &MessageListView::linkActivated, this, &ChatView::openLink);
    connect(m_list, &MessageListView::imageActivated, this, &ChatView::openImage);
    connect(m_list, &MessageListView::reactionClicked, this, &ChatView::toggleReaction);
    connect(m_list, &MessageListView::replyClicked, this, &ChatView::jumpTo);
    connect(m_list, &MessageListView::inviteClicked, this, &ChatView::joinInvite);
    // Invite cards change when their invite loads and when the user joins its server.
    connect(m_session, &Session::inviteLoaded, m_list->viewport(), qOverload<>(&QWidget::update));
    connect(m_session, &Session::guildListChanged, m_list->viewport(), qOverload<>(&QWidget::update));
    connect(m_list, &MessageListView::userClicked, this, [this](const QString& userId, const QPoint& position) {
        emit profileRequested(userId, m_guildId, position);
    });
    connect(m_list, &MessageListView::messageContextMenuRequested, this, &ChatView::showMessageMenu);
    connect(m_list, &MessageListView::selectionStarted, this, [this] {
        QTextCursor cursor = m_composer->textCursor();
        cursor.clearSelection();
        m_composer->setTextCursor(cursor);
    });
    m_composer->setCopyFallback([this] {
        if (!m_list->hasSelection())
            return false;
        m_list->copySelection();
        return true;
    });
    connect(m_list, &MessageListView::topReached, this, [this] {
        if (!m_channelId.isEmpty() && m_session->messages()->hasOlder(m_channelId))
            m_session->messages()->loadOlder(m_channelId);
    });

    // Loading older messages must not move what the user is looking at.
    connect(m_model, &MessageModel::aboutToPrepend, this, [this] {
        m_scrollBeforePrepend = m_list->verticalScrollBar()->value();
        m_maximumBeforePrepend = m_list->verticalScrollBar()->maximum();
    });
    connect(m_model, &MessageModel::prepended, this, [this] {
        m_list->doItemsLayout();
        QScrollBar* bar = m_list->verticalScrollBar();
        bar->setValue(m_scrollBeforePrepend + bar->maximum() - m_maximumBeforePrepend);
    });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] {
        QTimer::singleShot(0, m_list, [this] { m_list->scrollToBottom(); });
        m_readTimer.start();
    });
    connect(m_model, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex&, int first) {
        // A new message from someone ends their typing indicator.
        if (first < m_model->rowCount() && m_typing.remove(m_model->message(first).author.id))
            updateTyping();
        m_readTimer.start();
    });
    connect(m_session->messages(), &MessageStore::sendFailed, this, [this](const QString& channelId, const QString& reason) {
        if (channelId == m_channelId)
            showError(tr("Your message could not be sent: %1").arg(reason));
    });

    connect(m_images, &ImageCache::imageLoaded, this, [this] {
        // Pictures change row heights only for emoji placeholders, which keep their size; a repaint is enough.
        m_delegate->invalidateAll();
        m_list->viewport()->update();
        if (m_mentionPopup->isVisible())
            updateMentionPopup(); // avatars
    });

    connect(m_session, &Session::typingStarted, this, [this](const QString& channelId, const QString& userId) {
        if (channelId != m_channelId)
            return;
        m_typing.insert(userId, QDateTime::currentMSecsSinceEpoch() + TypingDurationMs);
        updateTyping();
    });
    m_typingTimer.setInterval(1000);
    connect(&m_typingTimer, &QTimer::timeout, this, &ChatView::updateTyping);

    m_readTimer.setSingleShot(true);
    m_readTimer.setInterval(400);
    connect(&m_readTimer, &QTimer::timeout, this, &ChatView::markReadIfVisible);
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (m_list->isAtBottom())
            m_readTimer.start();
    });
}

ChatView::~ChatView()
{
    // The composer still signals while the children are being destroyed, when the mention popup may be gone.
    m_composer->disconnect(this);
}

void ChatView::showChannel(const QString& guildId, const QString& channelId)
{
    if (channelId == m_channelId && guildId == m_guildId) {
        refreshHeader();
        updateMemberList();
        return;
    }
    saveDraft();
    // Restoring a draft is not user typing: do not send typing events or open a mention search.
    const QScopedValueRollback<bool> restoringDraft(m_restoringDraft, true);
    m_memberSearchTimer.stop();
    m_memberQuery.clear();
    m_mentionStart = -1;
    m_guildId = guildId;
    m_channelId = channelId;
    updateMemberList();
    cancelMode();
    m_composer->clear();
    m_mentionTokens.clear();
    m_mentionPopup->hide();
    m_files.clear();
    updateAttachments();
    m_typing.clear();
    m_statusLabel->clear();
    m_delegate->invalidateAll();
    m_delegate->setGuildId(guildId);
    m_session->messages()->open(channelId);
    m_session->ensureAuthorRoles(guildId, channelId);
    m_model->setChannel(channelId);
    refreshHeader();
    restoreDraft();
    m_composer->setFocus();
}

void ChatView::saveDraft()
{
    if (m_channelId.isEmpty())
        return;
    const QString text = m_composer->toPlainText();
    if (text.isEmpty() && m_files.isEmpty() && m_replyTo.isEmpty() && m_editing.isEmpty()) {
        m_drafts.remove(m_channelId);
        return;
    }
    const QTextCursor cursor = m_composer->textCursor();
    m_drafts.insert(m_channelId, Draft{text, m_mentionTokens, m_files, m_replyTo, m_editing,
                                       m_modeLabel->text(), cursor.position(), cursor.anchor()});
}

void ChatView::restoreDraft()
{
    // The active composer owns the draft, avoiding a second retained copy after sending or clearing it.
    const Draft draft = m_drafts.take(m_channelId);
    m_mentionTokens = draft.mentions;
    m_files = draft.files;
    m_replyTo = draft.replyTo;
    m_editing = draft.editing;
    m_modeLabel->setText(draft.modeLabel);
    m_modeBar->setVisible(!m_replyTo.isEmpty() || !m_editing.isEmpty());
    m_composer->setPlainText(draft.text);
    QTextCursor cursor = m_composer->textCursor();
    cursor.setPosition(qBound(0, draft.anchor, static_cast<int>(draft.text.size())));
    cursor.setPosition(qBound(0, draft.cursor, static_cast<int>(draft.text.size())), QTextCursor::KeepAnchor);
    m_composer->setTextCursor(cursor);
    updateAttachments();
}

void ChatView::updateMemberList()
{
    // Direct messages have no member list.
    const bool guild = !m_guildId.isEmpty();
    m_membersButton->setVisible(guild);
    m_membersButton->setToolTip(m_membersButton->isChecked() ? tr("Hide Member List") : tr("Show Member List"));
    if (guild)
        m_memberList->setChannel(m_guildId, m_channelId);
    m_memberList->setVisible(guild && m_membersButton->isChecked());
}

void ChatView::refreshHeader()
{
    bool canSend = true;
    m_canAttach = true;
    if (m_guildId.isEmpty()) {
        const PrivateChannel* channel = m_session->privateChannel(m_channelId);
        const QString name = channel ? m_session->privateChannelName(*channel) : QString();
        m_icon->setPixmap(QIcon(QStringLiteral(":/icons/at.svg")).pixmap(QSize(24, 24), devicePixelRatioF()));
        m_title->setText(name);
        m_topic->clear();
        m_composer->setPlaceholderText(tr("Message @%1").arg(name));
        const Call* call = m_session->call(m_channelId);
        const bool callRunning = call && !call->voiceStates.isEmpty();
        m_callButton->setText(callRunning ? tr("Join Call") : tr("Start Call"));
        m_callButton->setVisible(m_voice->channelId() != m_channelId);
    } else {
        const Channel* channel = m_session->channel(m_guildId, m_channelId);
        const QString name = channel ? channel->name : QString();
        m_icon->setPixmap(QIcon(QStringLiteral(":/icons/hash.svg")).pixmap(QSize(24, 24), devicePixelRatioF()));
        m_title->setText(name);
        m_topic->setText(channel ? m_topic->fontMetrics().elidedText(channel->topic.simplified(), Qt::ElideRight, 500) : QString());
        m_topic->setToolTip(channel ? channel->topic : QString());
        m_composer->setPlaceholderText(tr("Message #%1").arg(name));
        m_callButton->hide();
        const Guild* guild = m_session->guild(m_guildId);
        if (guild && channel) {
            const quint64 permissions = Permissions::compute(*guild, *channel, m_session->self().id);
            canSend = permissions & Permissions::SendMessages;
            m_canAttach = permissions & Permissions::AttachFiles;
        }
    }
    m_composer->setReadOnly(!canSend);
    m_emojiButton->setEnabled(canSend);
    m_attachButton->setVisible(canSend && m_canAttach);
    if (!canSend)
        m_composer->setPlaceholderText(tr("You do not have permission to send messages in this channel."));
}

void ChatView::markReadIfVisible()
{
    if (m_channelId.isEmpty() || !isVisible() || !window()->isActiveWindow() || !m_list->isAtBottom())
        return;
    m_session->markRead(m_guildId, m_channelId);
}

void ChatView::submit(const QString& typed)
{
    m_mentionPopup->hide();
    const bool editing = !m_editing.isEmpty();
    const QString text = Mentions::encode(typed, m_mentionTokens);
    if (text.isEmpty() && (editing || m_files.isEmpty()))
        return;
    const int limit = UploadLimits::maxMessageLength(m_session->self().premiumType);
    if (text.size() > limit) {
        showError(tr("Your message is too long (%1 of %2 characters).").arg(text.size()).arg(limit));
        return;
    }
    if (editing) {
        m_session->messages()->edit(m_channelId, m_editing, text);
    } else {
        m_session->messages()->send(m_channelId, m_guildId, text, m_replyTo, m_files);
        m_files.clear();
        updateAttachments();
        QTimer::singleShot(0, m_list, [this] { m_list->scrollToBottom(); });
    }
    m_composer->clear();
    m_mentionTokens.clear();
    cancelMode();
}

void ChatView::dragEnterEvent(QDragEnterEvent* event)
{
    if (!m_channelId.isEmpty() && !localFiles(event->mimeData()).isEmpty())
        event->acceptProposedAction();
}

void ChatView::dropEvent(QDropEvent* event)
{
    const QStringList files = localFiles(event->mimeData());
    if (files.isEmpty())
        return;
    event->acceptProposedAction();
    addFiles(files);
}

void ChatView::chooseFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Upload a File"));
    if (!files.isEmpty())
        addFiles(files);
    m_composer->setFocus();
}

void ChatView::addFiles(const QStringList& paths)
{
    static const QMimeDatabase mimeTypes;
    for (const QString& path : paths) {
        const QFileInfo info(path);
        if (!info.isFile() || !info.isReadable()) {
            showError(tr("%1 is not a file that can be sent.").arg(info.fileName()));
            continue;
        }
        OutgoingFile file;
        file.filename = info.fileName();
        file.path = info.absoluteFilePath();
        file.size = info.size();
        file.contentType = mimeTypes.mimeTypeForFile(info).name();
        if (!addFile(file))
            break;
    }
    updateAttachments();
    m_composer->setFocus();
}

void ChatView::addPastedImage(const QImage& image)
{
    OutgoingFile file;
    QBuffer buffer(&file.data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    file.filename = QStringLiteral("image.png");
    file.size = file.data.size();
    file.contentType = QStringLiteral("image/png");
    addFile(file);
    updateAttachments();
}

bool ChatView::addFile(const OutgoingFile& file)
{
    if (m_channelId.isEmpty() || m_composer->isReadOnly() || !m_canAttach) {
        showError(tr("You do not have permission to attach files in this channel."));
        return false;
    }
    if (m_files.size() >= UploadLimits::MaxFiles) {
        showError(tr("You can only upload %1 files at a time.").arg(UploadLimits::MaxFiles));
        return false;
    }
    const Guild* guild = m_guildId.isEmpty() ? nullptr : m_session->guild(m_guildId);
    const qint64 maxSize = UploadLimits::maxFileSize(m_session->self().premiumType, guild ? guild->premiumTier : 0);
    if (file.size > maxSize) {
        showError(tr("%1 is too large. The maximum file size here is %2.").arg(file.filename, formatSize(maxSize)));
        return false;
    }
    qint64 total = file.size;
    for (const OutgoingFile& other : std::as_const(m_files))
        total += other.size;
    if (total > UploadLimits::MaxTotalSize) {
        showError(tr("These files are too large together. The maximum per message is %1.").arg(formatSize(UploadLimits::MaxTotalSize)));
        return false;
    }
    m_files.append(file);
    return true;
}

void ChatView::updateAttachments()
{
    m_tray->setFiles(m_files);
}

bool ChatView::canMentionEveryone() const
{
    const Guild* guild = m_session->guild(m_guildId);
    const Channel* channel = m_session->channel(m_guildId, m_channelId);
    return guild && channel && (Permissions::compute(*guild, *channel, m_session->self().id) & Permissions::MentionEveryone);
}

void ChatView::updateMentionPopup()
{
    if (m_restoringDraft)
        return;
    // Find an "@" or "#" that starts a word and leads, without spaces, to the cursor.
    const QTextCursor cursor = m_composer->textCursor();
    m_mentionStart = -1;
    QChar trigger;
    QString query;
    if (!cursor.hasSelection() && !m_composer->isReadOnly()) {
        const QString before = cursor.block().text().left(cursor.positionInBlock());
        for (qsizetype i = before.size() - 1; i >= 0 && before.size() - i <= MaxMentionQuery + 1; --i) {
            const QChar c = before.at(i);
            if (c.isSpace())
                break;
            if (c == u'@' || (c == u'#' && !m_guildId.isEmpty())) {
                if (i == 0 || before.at(i - 1).isSpace()) {
                    m_mentionStart = cursor.block().position() + static_cast<int>(i);
                    trigger = c;
                    query = before.mid(i + 1);
                }
                break;
            }
        }
    }
    if (m_mentionStart < 0) {
        m_mentionPopup->hide();
        return;
    }

    const QList<MentionSuggestion> suggestions = trigger == u'@' ? userSuggestions(query) : channelSuggestions(query);
    if (suggestions.isEmpty()) {
        m_mentionPopup->hide();
    } else {
        const QString title = trigger == u'#' ? tr("Channels") : m_guildId.isEmpty() ? tr("Members") : tr("Members and Roles");
        m_mentionPopup->setSuggestions(title, suggestions);
        m_mentionPopup->placeAbove(QRect(m_inputBox->mapTo(this, QPoint(0, 0)), m_inputBox->size()));
        if (!m_mentionPopup->isVisible()) {
            m_mentionPopup->show();
            Motion::fadeInWidget(m_mentionPopup);
        }
    }
    // Members not seen yet are searched on the server; the list refreshes when they arrive.
    if (trigger == u'@' && !m_guildId.isEmpty() && !query.isEmpty() && query != m_memberQuery) {
        m_memberQuery = query;
        m_memberSearchTimer.start();
    }
}

QList<MentionSuggestion> ChatView::userSuggestions(const QString& query) const
{
    // Recent authors first (the people in the conversation), then other known members.
    QStringList ids;
    QHash<QString, User> authors;
    const auto& messages = m_session->messages()->messages(m_channelId);
    for (qsizetype i = messages.size() - 1; i >= 0; --i) {
        const User& author = messages[i].author;
        if (!author.id.isEmpty() && !authors.contains(author.id)) {
            authors.insert(author.id, author);
            ids.append(author.id);
        }
    }
    QHash<QString, QString> nicks;
    if (m_guildId.isEmpty()) {
        if (const PrivateChannel* channel = m_session->privateChannel(m_channelId)) {
            for (const QString& id : channel->recipientIds) {
                if (!ids.contains(id))
                    ids.append(id);
            }
        }
        if (!ids.contains(m_session->self().id))
            ids.append(m_session->self().id);
    } else {
        nicks = m_session->knownMembers(m_guildId);
        for (auto it = nicks.cbegin(); it != nicks.cend(); ++it) {
            if (!authors.contains(it.key()))
                ids.append(it.key());
        }
    }

    QList<std::pair<int, MentionSuggestion>> ranked;
    for (const QString& id : std::as_const(ids)) {
        User user = m_session->user(id);
        if (user.id.isEmpty())
            user = id == m_session->self().id ? m_session->self() : authors.value(id);
        if (user.id.isEmpty())
            continue;
        const QString nick = nicks.value(id);
        const int rank = std::min({matchRank(nick, query), matchRank(user.globalName, query), matchRank(user.username, query)});
        if (rank > 1)
            continue;
        MentionSuggestion suggestion;
        suggestion.kind = MentionSuggestion::User;
        suggestion.label = nick.isEmpty() ? user.displayName() : nick;
        suggestion.display = u'@' + suggestion.label;
        suggestion.raw = QStringLiteral("<@%1>").arg(id);
        suggestion.detail = user.username;
        suggestion.avatar = m_images->image(ImageCache::avatarUrl(user));
        ranked.append({rank, suggestion});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    QList<MentionSuggestion> result;
    for (const auto& entry : std::as_const(ranked)) {
        if (result.size() >= MaxUserSuggestions)
            break;
        result.append(entry.second);
    }

    const Guild* guild = m_session->guild(m_guildId);
    if (!guild)
        return result;
    const bool everyone = canMentionEveryone();
    QList<Role> roles;
    for (const Role& role : guild->roles) {
        if (role.id != guild->id && (role.mentionable || everyone) && matchRank(role.name, query) <= 1)
            roles.append(role);
    }
    std::sort(roles.begin(), roles.end(), [](const Role& a, const Role& b) { return a.position > b.position; });
    for (const Role& role : std::as_const(roles).first(std::min<qsizetype>(roles.size(), MaxRoleSuggestions))) {
        MentionSuggestion suggestion;
        suggestion.kind = MentionSuggestion::Role;
        suggestion.label = role.name;
        suggestion.display = u'@' + role.name;
        suggestion.raw = QStringLiteral("<@&%1>").arg(role.id);
        suggestion.detail = tr("Role");
        if (role.color != 0)
            suggestion.color = QColor::fromRgb(QRgb(role.color));
        result.append(suggestion);
    }
    if (everyone) {
        const std::pair<QString, QString> special[] = {
            {QStringLiteral("everyone"), tr("Notify everyone who can see this channel.")},
            {QStringLiteral("here"), tr("Notify everyone online who can see this channel.")},
        };
        for (const auto& [name, detail] : special) {
            if (!name.startsWith(query, Qt::CaseInsensitive))
                continue;
            MentionSuggestion suggestion;
            suggestion.kind = MentionSuggestion::Everyone;
            suggestion.label = u'@' + name;
            suggestion.display = suggestion.label;
            suggestion.raw = suggestion.label;
            suggestion.detail = detail;
            result.append(suggestion);
        }
    }
    return result;
}

QList<MentionSuggestion> ChatView::channelSuggestions(const QString& query) const
{
    const Guild* guild = m_session->guild(m_guildId);
    if (!guild)
        return {};
    QList<std::pair<int, MentionSuggestion>> ranked;
    for (const Channel& channel : m_session->visibleChannels(m_guildId)) {
        const int rank = matchRank(channel.name, query);
        if (channel.type == ChannelType::GuildCategory || rank > 1)
            continue;
        MentionSuggestion suggestion;
        suggestion.kind = MentionSuggestion::Channel;
        suggestion.label = channel.name;
        suggestion.display = u'#' + channel.name;
        suggestion.raw = QStringLiteral("<#%1>").arg(channel.id);
        suggestion.icon = channel.isVoice() ? QStringLiteral(":/icons/speaker.svg") : QStringLiteral(":/icons/hash.svg");
        suggestion.detail = guild->channels.value(channel.parentId).name;
        ranked.append({rank, suggestion});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    QList<MentionSuggestion> result;
    for (const auto& entry : std::as_const(ranked)) {
        if (result.size() >= MaxChannelSuggestions)
            break;
        result.append(entry.second);
    }
    return result;
}

void ChatView::insertMention(const MentionSuggestion& suggestion)
{
    if (m_mentionStart < 0)
        return;
    // Two people (or channels) can have the same name: then show something unique for the second one.
    const auto taken = [this, &suggestion](const QString& display) {
        return std::any_of(m_mentionTokens.cbegin(), m_mentionTokens.cend(), [&](const MentionToken& token) {
            return token.display == display && token.raw != suggestion.raw;
        });
    };
    QString display = suggestion.display;
    if (taken(display) && suggestion.kind == MentionSuggestion::User)
        display = u'@' + suggestion.detail;
    if (taken(display))
        display = suggestion.raw;
    const bool known = std::any_of(m_mentionTokens.cbegin(), m_mentionTokens.cend(),
                                   [&](const MentionToken& token) { return token.display == display; });
    if (display != suggestion.raw && !known)
        m_mentionTokens.append({display, suggestion.raw});

    QTextCursor cursor = m_composer->textCursor();
    const int end = cursor.position();
    cursor.setPosition(m_mentionStart);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.insertText(display + u' ');
    m_composer->setTextCursor(cursor);
    m_mentionStart = -1;
    m_composer->setFocus();
}

void ChatView::startReply(const QString& messageId)
{
    const Message* message = m_session->messages()->message(m_channelId, messageId);
    if (!message)
        return;
    m_editing.clear();
    m_replyTo = messageId;
    m_modeLabel->setText(tr("Replying to <b>%1</b>").arg(message->author.displayName().toHtmlEscaped()));
    m_modeBar->show();
    m_composer->setFocus();
}

void ChatView::startEdit(const QString& messageId)
{
    const Message* message = m_session->messages()->message(m_channelId, messageId);
    if (!message)
        return;
    m_replyTo.clear();
    m_editing = messageId;
    m_modeLabel->setText(tr("Editing message — <b>Escape</b> to cancel, <b>Enter</b> to save"));
    m_modeBar->show();
    // Mentions are edited in their readable form, like when they were written.
    m_mentionTokens.clear();
    const QString text = Mentions::decode(message->content, [this](QChar kind, const QString& id) -> QString {
        if (kind == u'@') {
            const User user = m_session->user(id);
            if (user.id.isEmpty())
                return {};
            const QString nick = m_guildId.isEmpty() ? QString() : m_session->knownMembers(m_guildId).value(id);
            return u'@' + (nick.isEmpty() ? user.displayName() : nick);
        }
        const Guild* guild = m_session->guild(m_guildId);
        if (kind == u'&')
            return guild && guild->roles.contains(id) ? u'@' + guild->roles.value(id).name : QString();
        const Channel* channel = m_session->channel(m_guildId, id);
        return channel ? u'#' + channel->name : QString();
    }, &m_mentionTokens);
    m_composer->setPlainText(text);
    m_composer->moveCursor(QTextCursor::End);
    m_composer->setFocus();
}

void ChatView::cancelMode()
{
    if (!m_editing.isEmpty()) {
        m_composer->clear();
        m_mentionTokens.clear();
    }
    m_replyTo.clear();
    m_editing.clear();
    m_modeBar->hide();
}

void ChatView::editLastMessage()
{
    const auto& messages = m_session->messages()->messages(m_channelId);
    for (qsizetype i = messages.size() - 1; i >= 0; --i) {
        if (messages[i].author.id == m_session->self().id && !messages[i].isSystemMessage() && !messages[i].pending) {
            startEdit(messages[i].id);
            return;
        }
    }
}

void ChatView::showMessageMenu(const QString& messageId, const QPoint& globalPosition)
{
    const Message* message = m_session->messages()->message(m_channelId, messageId);
    if (!message || message->pending)
        return;
    const bool own = message->author.id == m_session->self().id;

    QMenu menu(this);
    QMenu* reactions = menu.addMenu(tr("Add Reaction"));
    for (const char* quick : QuickReactions) {
        const QString emoji = QString::fromUtf8(quick);
        reactions->addAction(emoji, this, [this, messageId, emoji] {
            Emoji reaction;
            reaction.name = emoji;
            m_session->messages()->setReaction(m_channelId, messageId, reaction, true);
        });
    }
    reactions->addSeparator();
    reactions->addAction(tr("Other…"), this, [this, messageId, globalPosition] { pickReaction(messageId, globalPosition); });

    if (!m_composer->isReadOnly())
        menu.addAction(tr("Reply"), this, [this, messageId] { startReply(messageId); });
    if (own && !message->isSystemMessage())
        menu.addAction(tr("Edit Message"), this, [this, messageId] { startEdit(messageId); });
    menu.addSeparator();
    if (m_list->hasSelection())
        menu.addAction(tr("Copy"), this, [this] { m_list->copySelection(); });
    if (!message->content.isEmpty())
        menu.addAction(tr("Copy Text"), this, [content = message->content] { QApplication::clipboard()->setText(content); });
    menu.addAction(tr("Copy Message Link"), this, [this, messageId] {
        QApplication::clipboard()->setText(QStringLiteral("https://discord.com/channels/%1/%2/%3")
                                               .arg(m_guildId.isEmpty() ? QStringLiteral("@me") : m_guildId, m_channelId, messageId));
    });
    menu.addAction(tr("Copy Message ID"), this, [messageId] { QApplication::clipboard()->setText(messageId); });
    if (own) {
        menu.addSeparator();
        QAction* remove = menu.addAction(tr("Delete Message"), this, [this, messageId] {
            const auto answer = QMessageBox::question(this, tr("Delete Message"),
                                                      tr("Are you sure you want to delete this message?"));
            if (answer == QMessageBox::Yes)
                m_session->messages()->remove(m_channelId, messageId);
        });
        remove->setObjectName(QStringLiteral("dangerAction"));
    }
    menu.exec(globalPosition);
}

void ChatView::pickReaction(const QString& messageId, const QPoint& globalPosition)
{
    auto* picker = new EmojiPicker(m_session, m_images, m_guildId, this);
    connect(picker, &EmojiPicker::picked, this, [this, messageId](const QString&, const Emoji& emoji) {
        m_session->messages()->setReaction(m_channelId, messageId, emoji, true);
    });
    picker->popupAt(globalPosition + QPoint(picker->width(), picker->height()));
}

void ChatView::toggleReaction(const QString& messageId, int reactionIndex)
{
    const Message* message = m_session->messages()->message(m_channelId, messageId);
    if (!message || reactionIndex < 0 || reactionIndex >= message->reactions.size())
        return;
    const Reaction& reaction = message->reactions[reactionIndex];
    m_session->messages()->setReaction(m_channelId, messageId, reaction.emoji, !reaction.me);
}

void ChatView::openLink(const QString& url)
{
    // Invites open in the app, like in Discord.
    if (const QString code = Invites::codeFromUrl(QUrl(url)); !code.isEmpty()) {
        emit inviteLinkActivated(code);
        return;
    }
    if (url.startsWith(u"http://") || url.startsWith(u"https://"))
        QDesktopServices::openUrl(QUrl(url));
}

void ChatView::joinInvite(const QString& code, const QString& messageId)
{
    const InviteInfo* invite = m_session->cachedInvite(code);
    if (!invite)
        return;
    const QString guildId = invite->guildId;
    const QString channelId = guildId.isEmpty() ? invite->channelId : QString();
    const bool member = guildId.isEmpty() ? m_session->privateChannel(invite->channelId) != nullptr
                                          : m_session->guild(guildId) != nullptr;
    if (member) {
        emit openChannelRequested(guildId, channelId);
        return;
    }
    Session::InviteSource source;
    source.guildId = m_guildId;
    source.channelId = m_channelId;
    source.messageId = messageId;
    if (const Channel* channel = m_session->channel(m_guildId, m_channelId))
        source.channelType = static_cast<int>(channel->type);
    else if (const PrivateChannel* conversation = m_session->privateChannel(m_channelId))
        source.channelType = static_cast<int>(conversation->type);
    QPointer<ChatView> guard(this);
    m_session->acceptInvite(*invite, source, [guard, guildId, channelId](const QString& error) {
        if (!guard)
            return;
        if (!error.isEmpty())
            guard->showError(error);
        else
            emit guard->openChannelRequested(guildId, channelId);
    });
}

void ChatView::openImage(const QString& url, bool video, bool web)
{
    if (!url.startsWith(u"http://") && !url.startsWith(u"https://"))
        return;
    const QUrl parsed(url);
    // YouTube, Twitch and other page embeds always leave the app. Only a direct Discord
    // .mp4/.webm is played here; other video files follow the same browser path.
    if (web || (video && !DiscordVideo::isDiscordFile(parsed))) {
        QDesktopServices::openUrl(parsed);
        return;
    }
    if (video) {
        DiscordVideo::open(parsed, window());
        return;
    }
    ImageViewer::open(parsed, m_images, window());
}

void ChatView::jumpTo(const QString& messageId)
{
    const int row = m_model->rowOf(messageId);
    if (row >= 0)
        m_list->scrollTo(m_model->index(row), QAbstractItemView::PositionAtCenter);
}

void ChatView::updateTyping()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList names;
    for (auto it = m_typing.begin(); it != m_typing.end();) {
        if (it.value() < now) {
            it = m_typing.erase(it);
        } else {
            names.append(m_session->user(it.key()).displayName());
            ++it;
        }
    }
    names.removeAll(QString());
    if (names.isEmpty()) {
        m_typingTimer.stop();
        m_statusLabel->clear();
        return;
    }
    m_typingTimer.start();
    m_statusLabel->setProperty("error", false);
    m_statusLabel->setStyleSheet(QString());
    QString text;
    if (names.size() == 1)
        text = tr("<b>%1</b> is typing…").arg(names[0].toHtmlEscaped());
    else if (names.size() == 2)
        text = tr("<b>%1</b> and <b>%2</b> are typing…").arg(names[0].toHtmlEscaped(), names[1].toHtmlEscaped());
    else
        text = tr("Several people are typing…");
    m_statusLabel->setText(text);
}

void ChatView::showError(const QString& text)
{
    m_statusLabel->setStyleSheet(QStringLiteral("color:#f23f43;"));
    m_statusLabel->setText(text.toHtmlEscaped());
    QTimer::singleShot(6000, this, [this] {
        if (m_typing.isEmpty()) {
            m_statusLabel->setStyleSheet(QString());
            m_statusLabel->clear();
        }
    });
}
