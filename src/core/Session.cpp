#include "core/Session.h"

#include "core/ClientProperties.h"
#include "core/Gateway.h"
#include "core/Log.h"
#include "core/OrderedJson.h"
#include "core/Permissions.h"
#include "core/RestClient.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>

#include <algorithm>

Session::Session(QObject* parent)
    : QObject(parent)
    , m_gateway(new Gateway(this))
    , m_rest(new RestClient(this))
    , m_messages(new MessageStore(m_rest, this))
{
    connect(m_gateway, &Gateway::dispatch, this, &Session::onDispatch);
    connect(m_gateway, &Gateway::authenticationFailed, this, &Session::authenticationFailed);
    connect(m_gateway, &Gateway::connectionStateChanged, this, [this](bool connected) {
        // Presence updates before READY would get the connection closed.
        m_gatewayReady = connected;
        emit connectionStateChanged(connected);
    });

    // A song change and the end of the previous one, or several quick skips, become a single update.
    m_presenceTimer.setSingleShot(true);
    m_presenceTimer.setInterval(1000);
    connect(&m_presenceTimer, &QTimer::timeout, this, &Session::sendOwnPresence);

    // Unknown users in voice channels are requested in batches instead of one request per event.
    m_missingUsersTimer.setSingleShot(true);
    m_missingUsersTimer.setInterval(250);
    connect(&m_missingUsersTimer, &QTimer::timeout, this, &Session::requestMissingUsers);
    m_missingRolesTimer.setSingleShot(true);
    m_missingRolesTimer.setInterval(250);
    connect(&m_missingRolesTimer, &QTimer::timeout, this, &Session::requestMissingRoles);
}

void Session::start(const QString& token)
{
    m_token = token;
    m_rest->setToken(token);
    m_gateway->start(token);
}

void Session::startOffline(const QList<std::pair<QString, QJsonObject>>& events)
{
    m_rest->setOffline(true);
    for (const auto& [event, data] : events)
        onDispatch(event, data);
}

void Session::stop()
{
    m_gateway->stop();
}

void Session::setActiveState(bool focused, bool rtcConnected)
{
    m_gateway->setActiveState(focused, rtcConnected);
}

QString Session::sessionId() const
{
    return m_gateway->sessionId();
}

const Guild* Session::guild(const QString& id) const
{
    const auto it = m_guilds.constFind(id);
    return it == m_guilds.cend() ? nullptr : &it.value();
}

const Channel* Session::channel(const QString& guildId, const QString& channelId) const
{
    const Guild* g = guild(guildId);
    if (!g)
        return nullptr;
    const auto it = g->channels.constFind(channelId);
    return it == g->channels.cend() ? nullptr : &it.value();
}

QList<Channel> Session::visibleChannels(const QString& guildId) const
{
    const Guild* g = guild(guildId);
    if (!g)
        return {};

    auto byPosition = [](const Channel& a, const Channel& b) {
        // Text-like channels come before voice channels inside the same category, like in Discord.
        if (a.isVoice() != b.isVoice())
            return !a.isVoice();
        if (a.position != b.position)
            return a.position < b.position;
        return snowflakeLess(a.id, b.id);
    };

    QList<Channel> categories;
    QHash<QString, QList<Channel>> children; // by parent ID ("" = no category)
    for (const Channel& channel : g->channels) {
        if (channel.type == ChannelType::GuildCategory) {
            categories.append(channel);
            continue;
        }
        if (!(Permissions::compute(*g, channel, m_self.id) & Permissions::ViewChannel))
            continue;
        children[channel.parentId].append(channel);
    }
    std::sort(categories.begin(), categories.end(), byPosition);

    QList<Channel> result;
    QList<Channel> uncategorized = children.value(QString());
    std::sort(uncategorized.begin(), uncategorized.end(), byPosition);
    result.append(uncategorized);
    for (const Channel& category : categories) {
        QList<Channel> items = children.value(category.id);
        if (items.isEmpty())
            continue; // Discord hides categories with no visible channels
        std::sort(items.begin(), items.end(), byPosition);
        result.append(category);
        result.append(items);
    }
    return result;
}

QList<VoiceState> Session::voiceStatesInChannel(const QString& guildId, const QString& channelId) const
{
    QList<VoiceState> states;
    if (guildId.isEmpty()) {
        if (const Call* c = call(channelId))
            states = c->voiceStates.values();
    } else if (const Guild* g = guild(guildId)) {
        for (const VoiceState& state : g->voiceStates) {
            if (state.channelId == channelId)
                states.append(state);
        }
    }
    std::sort(states.begin(), states.end(), [this](const VoiceState& a, const VoiceState& b) {
        return user(a.userId).displayName().compare(user(b.userId).displayName(), Qt::CaseInsensitive) < 0;
    });
    return states;
}

bool Session::canConnect(const QString& guildId, const QString& channelId) const
{
    if (guildId.isEmpty())
        return m_privateChannels.contains(channelId);
    const Guild* g = guild(guildId);
    const Channel* c = channel(guildId, channelId);
    if (!g || !c)
        return false;
    const quint64 permissions = Permissions::compute(*g, *c, m_self.id);
    return (permissions & Permissions::ViewChannel) && (permissions & Permissions::Connect);
}

User Session::user(const QString& id) const
{
    if (id == m_self.id)
        return m_self;
    User result = m_users.value(id);
    if (result.id.isEmpty())
        result.id = id;
    return result;
}

void Session::updateVoiceState(const QString& guildId, const QString& channelId, bool selfMute, bool selfDeaf)
{
    m_gateway->updateVoiceState(guildId, channelId, selfMute, selfDeaf);
}

QList<PrivateChannel> Session::privateChannels() const
{
    QList<PrivateChannel> channels = m_privateChannels.values();
    std::sort(channels.begin(), channels.end(), [](const PrivateChannel& a, const PrivateChannel& b) {
        // Channels without messages sort by creation time (their ID).
        const QString& left = a.lastMessageId.isEmpty() ? a.id : a.lastMessageId;
        const QString& right = b.lastMessageId.isEmpty() ? b.id : b.lastMessageId;
        return snowflakeLess(right, left);
    });
    return channels;
}

const PrivateChannel* Session::privateChannel(const QString& id) const
{
    const auto it = m_privateChannels.constFind(id);
    return it == m_privateChannels.cend() ? nullptr : &it.value();
}

QString Session::privateChannelName(const PrivateChannel& channel) const
{
    if (!channel.name.isEmpty())
        return channel.name;
    QStringList names;
    for (const QString& id : channel.recipientIds)
        names.append(user(id).displayName());
    names.removeAll(QString());
    return names.join(QStringLiteral(", "));
}

const Call* Session::call(const QString& channelId) const
{
    const auto it = m_calls.constFind(channelId);
    return it == m_calls.cend() ? nullptr : &it.value();
}

bool Session::isRingingSelf(const QString& channelId) const
{
    const Call* c = call(channelId);
    return c && c->ringing.contains(m_self.id);
}

void Session::ringCall(const QString& channelId)
{
    // A null recipient list rings everyone in the channel.
    m_rest->post(QStringLiteral("/channels/%1/call/ring").arg(channelId),
                 QJsonDocument(QJsonObject{{QStringLiteral("recipients"), QJsonValue()}}), nullptr);
}

void Session::declineCall(const QString& channelId)
{
    // Without recipients, this stops ringing the current user only.
    m_rest->post(QStringLiteral("/channels/%1/call/stop-ringing").arg(channelId),
                 QJsonDocument(QJsonObject{{QStringLiteral("recipients"), QJsonValue()}}), nullptr);
}

void Session::onDispatch(const QString& event, const QJsonObject& data)
{
    const QString guildId = data.value(u"guild_id").toString();

    if (event == u"READY") {
        loadReady(data);
    } else if (event == u"READY_SUPPLEMENTAL") {
        const QJsonObject presences = data.value(u"merged_presences").toObject();
        for (const QJsonValue& value : presences.value(u"friends").toArray())
            storePresence(value.toObject());
        for (const QJsonValue& guildPresences : presences.value(u"guilds").toArray()) {
            for (const QJsonValue& value : guildPresences.toArray())
                storePresence(value.toObject());
        }
        for (const QJsonValue& value : data.value(u"guilds").toArray()) {
            const QJsonObject object = value.toObject();
            const QString id = object.value(u"id").toString();
            for (const QJsonValue& state : object.value(u"voice_states").toArray())
                applyVoiceState(VoiceState::fromJson(state.toObject(), id));
            emit voiceStatesChanged(id);
        }
    } else if (event == u"GUILD_CREATE") {
        loadGuild(data);
        const QString id = data.value(u"id").toString();
        if (!m_guildOrder.contains(id)) {
            // A newly joined server goes to the top of the list.
            loadGuildFolders(m_guildFolders);
            emit guildListChanged();
        }
        emit guildChanged(id);
    } else if (event == u"GUILD_UPDATE") {
        auto it = m_guilds.find(data.value(u"id").toString());
        if (it != m_guilds.end()) {
            it->name = data.value(u"name").toString(it->name);
            it->icon = data.value(u"icon").toString();
            it->ownerId = data.value(u"owner_id").toString(it->ownerId);
            it->premiumTier = data.value(u"premium_tier").toInt(it->premiumTier);
            emit guildChanged(it->id);
            emit guildListChanged();
        }
    } else if (event == u"GUILD_DELETE") {
        const QString id = data.value(u"id").toString();
        if (data.value(u"unavailable").toBool()) {
            if (m_guilds.contains(id))
                m_guilds[id].unavailable = true;
        } else {
            m_guilds.remove(id);
            loadGuildFolders(m_guildFolders);
        }
        emit guildListChanged();
    } else if (event == u"GUILD_ROLE_CREATE" || event == u"GUILD_ROLE_UPDATE") {
        auto it = m_guilds.find(guildId);
        if (it != m_guilds.end()) {
            const Role role = Role::fromJson(data.value(u"role").toObject());
            it->roles.insert(role.id, role);
            emit guildChanged(guildId);
        }
    } else if (event == u"GUILD_ROLE_DELETE") {
        auto it = m_guilds.find(guildId);
        if (it != m_guilds.end()) {
            it->roles.remove(data.value(u"role_id").toString());
            emit guildChanged(guildId);
        }
    } else if (event == u"GUILD_MEMBER_UPDATE") {
        storeMember(guildId, data);
        if (data.value(u"user").toObject().value(u"id").toString() == m_self.id)
            emit guildChanged(guildId);
    } else if (event == u"GUILD_MEMBERS_CHUNK") {
        for (const QJsonValue& member : data.value(u"members").toArray())
            storeMember(guildId, member.toObject());
        for (const QJsonValue& presence : data.value(u"presences").toArray())
            storePresence(presence.toObject());
        emit usersChanged();
    } else if (event == u"PRESENCE_UPDATE") {
        storePresence(data);
    } else if (event == u"GUILD_MEMBER_LIST_UPDATE") {
        onMemberListUpdate(data);
    } else if (event == u"USER_CONNECTIONS_UPDATE") {
        emit connectionsChanged();
    } else if (event == u"SESSIONS_REPLACE") {
        loadSessions(data.value(u"sessions").toArray());
    } else if (event == u"USER_SETTINGS_UPDATE") {
        // Only the settings that changed are sent.
        if (data.contains(u"status"))
            m_selfStatus = statusFromString(data.value(u"status").toString());
        if (data.contains(u"custom_status"))
            m_selfCustomStatus = CustomStatus::fromJson(data.value(u"custom_status"));
        if (data.contains(u"status") || data.contains(u"custom_status"))
            emit presenceChanged(m_self.id, true);
        // The server list was rearranged (in another client, or the echo of our own change).
        if (data.contains(u"guild_folders")) {
            const QList<GuildFolder> previous = m_guildFolders;
            loadGuildFolders(GuildFolders::fromJson(data.value(u"guild_folders").toArray()));
            if (m_guildFolders != previous)
                emit guildListChanged();
        }
    } else if ((event == u"CHANNEL_CREATE" || event == u"CHANNEL_UPDATE") && guildId.isEmpty()) {
        for (const QJsonValue& recipient : data.value(u"recipients").toArray())
            storeUser(recipient.toObject());
        const PrivateChannel channel = PrivateChannel::fromJson(data);
        if (channel.type == ChannelType::DirectMessage || channel.type == ChannelType::GroupDirectMessage) {
            m_privateChannels.insert(channel.id, channel);
            emit privateChannelsChanged();
        }
    } else if (event == u"CHANNEL_DELETE" && guildId.isEmpty()) {
        if (m_privateChannels.remove(data.value(u"id").toString()))
            emit privateChannelsChanged();
    } else if (event == u"RELATIONSHIP_ADD" || event == u"RELATIONSHIP_UPDATE") {
        storeRelationship(data);
        emit relationshipsChanged();
    } else if (event == u"RELATIONSHIP_REMOVE") {
        if (m_relationships.remove(data.value(u"id").toString()))
            emit relationshipsChanged();
    } else if (event == u"CHANNEL_RECIPIENT_ADD" || event == u"CHANNEL_RECIPIENT_REMOVE") {
        auto it = m_privateChannels.find(data.value(u"channel_id").toString());
        if (it != m_privateChannels.end()) {
            const QJsonObject userJson = data.value(u"user").toObject();
            storeUser(userJson);
            const QString userId = userJson.value(u"id").toString();
            it->recipientIds.removeAll(userId);
            if (event == u"CHANNEL_RECIPIENT_ADD")
                it->recipientIds.append(userId);
            emit privateChannelsChanged();
        }
    } else if (event == u"MESSAGE_CREATE") {
        onMessageCreate(data);
        m_messages->handleDispatch(event, data);
    } else if (event.startsWith(u"MESSAGE_REACTION") || event == u"MESSAGE_UPDATE" || event == u"MESSAGE_DELETE"
               || event == u"MESSAGE_DELETE_BULK") {
        m_messages->handleDispatch(event, data);
    } else if (event == u"MESSAGE_ACK") {
        const QString channelId = data.value(u"channel_id").toString();
        ReadState& state = m_readStates[channelId];
        state.lastAckedId = data.value(u"message_id").toString();
        if (data.value(u"mention_count").isDouble())
            state.mentionCount = data.value(u"mention_count").toInt();
        else
            state.mentionCount = 0;
        emit readStateChanged(m_privateChannels.contains(channelId) ? QString() : guildId, channelId);
    } else if (event == u"TYPING_START") {
        const QString userId = data.value(u"user_id").toString();
        if (data.contains(u"member"))
            storeMember(guildId, data.value(u"member").toObject());
        if (userId != m_self.id)
            emit typingStarted(data.value(u"channel_id").toString(), userId);
    } else if (event == u"USER_GUILD_SETTINGS_UPDATE") {
        loadGuildSettings(data);
        emit readStateChanged(data.value(u"guild_id").toString(), QString());
    } else if (event == u"GUILD_EMOJIS_UPDATE") {
        auto it = m_guilds.find(guildId);
        if (it != m_guilds.end()) {
            it->emojis.clear();
            for (const QJsonValue& value : data.value(u"emojis").toArray()) {
                const QJsonObject emoji = value.toObject();
                it->emojis.append({emoji.value(u"id").toString(), emoji.value(u"name").toString(),
                                   emoji.value(u"animated").toBool()});
            }
        }
    } else if (event == u"CALL_CREATE") {
        loadCall(data);
    } else if (event == u"CALL_UPDATE") {
        const QString channelId = data.value(u"channel_id").toString();
        Call& call = m_calls[channelId];
        call.channelId = channelId;
        call.ringing.clear();
        for (const QJsonValue& id : data.value(u"ringing").toArray())
            call.ringing.append(id.toString());
        emit callChanged(channelId);
    } else if (event == u"CALL_DELETE") {
        const QString channelId = data.value(u"channel_id").toString();
        m_calls.remove(channelId);
        emit callChanged(channelId);
    } else if (event == u"CHANNEL_CREATE" || event == u"CHANNEL_UPDATE") {
        auto it = m_guilds.find(guildId);
        if (it != m_guilds.end()) {
            const Channel channel = Channel::fromJson(data, guildId);
            it->channels.insert(channel.id, channel);
            emit guildChanged(guildId);
        }
    } else if (event == u"CHANNEL_DELETE") {
        auto it = m_guilds.find(guildId);
        if (it != m_guilds.end()) {
            it->channels.remove(data.value(u"id").toString());
            emit guildChanged(guildId);
        }
    } else if (event == u"VOICE_STATE_UPDATE") {
        if (data.contains(u"member"))
            storeMember(guildId, data.value(u"member").toObject());
        const VoiceState state = VoiceState::fromJson(data, guildId);
        if (guildId.isEmpty()) {
            applyCallVoiceState(state);
        } else {
            applyVoiceState(state);
            emit voiceStatesChanged(guildId);
        }
        if (state.userId == m_self.id && state.sessionId == sessionId())
            emit ownVoiceStateChanged(state);
    } else if (event == u"VOICE_SERVER_UPDATE") {
        // A null endpoint means the voice server is being reallocated; a new update follows.
        const QString endpoint = data.value(u"endpoint").toString();
        if (!endpoint.isEmpty())
            emit voiceServerUpdated(guildId, data.value(u"channel_id").toString(), endpoint,
                                    data.value(u"token").toString());
    } else if (event == u"USER_UPDATE") {
        if (data.value(u"id").toString() == m_self.id) {
            m_self = User::fromJson(data);
            // The cached profile is stale now (bio, banner color and pronouns change here too).
            forgetOwnProfile();
            emit usersChanged();
            emit selfProfileChanged();
        }
    }
}

void Session::loadReady(const QJsonObject& data)
{
    m_self = User::fromJson(data.value(u"user").toObject());
    m_messages->setSelf(m_self);
    // A new session starts without subscriptions.
    m_listGuildId.clear();
    m_listChannelId.clear();
    m_memberLists.clear();
    m_guildMembers.clear();
    m_memberRoles.clear();
    m_relationships.clear();
    m_guilds.clear();
    m_guildOrder.clear();
    m_privateChannels.clear();
    m_calls.clear();
    m_readStates.clear();
    m_guildSettings.clear();
    loadReadStates(data.value(u"read_state"));
    // Without the versioned capability this is a plain array; with it, an object with "entries".
    const QJsonValue guildSettings = data.value(u"user_guild_settings");
    const QJsonArray settingsEntries = guildSettings.isArray() ? guildSettings.toArray()
                                                               : guildSettings.toObject().value(u"entries").toArray();
    for (const QJsonValue& entry : settingsEntries)
        loadGuildSettings(entry.toObject());

    for (const QJsonValue& value : data.value(u"users").toArray())
        storeUser(value.toObject());
    for (const QJsonValue& value : data.value(u"relationships").toArray())
        storeRelationship(value.toObject());

    m_presences.clear();
    m_requestedPresences.clear();
    m_profiles.clear();
    m_selfActivities.clear();
    const QJsonObject userSettings = data.value(u"user_settings").toObject();
    m_selfStatus = statusFromString(userSettings.value(u"status").toString());
    if (m_selfStatus == UserStatus::Unknown)
        m_selfStatus = UserStatus::Online;
    m_selfCustomStatus = CustomStatus::fromJson(userSettings.value(u"custom_status"));
    ClientProperties::setDiscordLocale(userSettings.value(u"locale").toString());
    loadSessions(data.value(u"sessions").toArray());

    for (const QJsonValue& value : data.value(u"private_channels").toArray()) {
        const QJsonObject json = value.toObject();
        for (const QJsonValue& recipient : json.value(u"recipients").toArray())
            storeUser(recipient.toObject());
        const PrivateChannel channel = PrivateChannel::fromJson(json);
        if (channel.type == ChannelType::DirectMessage || channel.type == ChannelType::GroupDirectMessage)
            m_privateChannels.insert(channel.id, channel);
    }

    const QJsonArray guilds = data.value(u"guilds").toArray();
    const QJsonArray mergedMembers = data.value(u"merged_members").toArray();
    for (qsizetype i = 0; i < guilds.size(); ++i) {
        const QJsonObject guildJson = guilds.at(i).toObject();
        loadGuild(guildJson);
        const QString id = guildJson.value(u"id").toString();
        for (const QJsonValue& member : mergedMembers.at(i).toArray())
            storeMember(id, member.toObject());
    }

    // Order guilds like the user arranged them in the official client, with their folders.
    for (const QJsonValue& value : guilds)
        m_guildOrder.append(value.toObject().value(u"id").toString());
    loadGuildFolders(GuildFolders::fromJson(userSettings.value(u"guild_folders").toArray()));

    // Identify sent the "unknown" status; announce the real one, with the custom status, like the official client.
    sendOwnPresence();

    emit ready();
    emit guildListChanged();
    emit privateChannelsChanged();
}

void Session::storePresence(const QJsonObject& json)
{
    // Deduplicated payloads carry only "user_id"; the rest have a partial user object.
    const QString userId = json.contains(u"user_id") ? json.value(u"user_id").toString()
                                                     : json.value(u"user").toObject().value(u"id").toString();
    if (userId.isEmpty() || userId == m_self.id)
        return;
    const Presence presence = Presence::fromJson(json);
    const auto it = m_presences.constFind(userId);
    const bool statusChanged = it == m_presences.cend() || it->status != presence.status;
    m_presences.insert(userId, presence);
    emit presenceChanged(userId, statusChanged);
}

void Session::loadSessions(const QJsonArray& sessions)
{
    // The "all" session merges every connected client of this account (a game on the official client, Spotify...).
    QJsonObject merged;
    for (const QJsonValue& value : sessions) {
        const QJsonObject session = value.toObject();
        if (session.value(u"session_id").toString() == u"all") {
            merged = session;
            break;
        }
    }
    if (merged.isEmpty() && !sessions.isEmpty())
        merged = sessions.first().toObject();
    m_selfActivities.clear();
    for (const QJsonValue& value : merged.value(u"activities").toArray()) {
        const Activity activity = Activity::fromJson(value.toObject());
        if (activity.type != Activity::Custom)
            m_selfActivities.append(activity);
    }
    emit presenceChanged(m_self.id, false);
}

Presence Session::presence(const QString& userId) const
{
    if (userId != m_self.id || userId.isEmpty())
        return m_presences.value(userId);
    Presence own;
    own.status = m_selfStatus;
    if (m_selfCustomStatus.isActive())
        own.activities.append(Activity::customStatus(m_selfCustomStatus.text, m_selfCustomStatus.emojiName,
                                                     m_selfCustomStatus.emojiId));
    own.activities.append(m_selfActivities);
    return own;
}

void Session::searchGuildMembers(const QString& guildId, const QString& query)
{
    if (!guildId.isEmpty() && !query.isEmpty())
        m_gateway->searchGuildMembers(guildId, query, 10);
}

void Session::requestPresence(const QString& guildId, const QString& userId)
{
    if (guildId.isEmpty() || userId == m_self.id || m_presences.contains(userId) || m_requestedPresences.contains(userId))
        return;
    m_requestedPresences.insert(userId);
    m_gateway->requestGuildMembers(guildId, {userId}, true);
}

namespace {

quint32 murmur3(const QByteArray& data)
{
    constexpr quint32 c1 = 0xcc9e2d51;
    constexpr quint32 c2 = 0x1b873593;
    auto rotl = [](quint32 x, int r) { return (x << r) | (x >> (32 - r)); };
    const auto* bytes = reinterpret_cast<const uchar*>(data.constData());
    const qsizetype length = data.size();
    const qsizetype blocks = length / 4;
    quint32 hash = 0;
    for (qsizetype i = 0; i < blocks; ++i) {
        quint32 k = bytes[i * 4] | (bytes[i * 4 + 1] << 8) | (bytes[i * 4 + 2] << 16) | (quint32(bytes[i * 4 + 3]) << 24);
        k = rotl(k * c1, 15) * c2;
        hash = rotl(hash ^ k, 13) * 5 + 0xe6546b64;
    }
    quint32 k = 0;
    const uchar* tail = bytes + blocks * 4;
    switch (length & 3) {
    case 3:
        k ^= tail[2] << 16;
        [[fallthrough]];
    case 2:
        k ^= tail[1] << 8;
        [[fallthrough]];
    case 1:
        k ^= tail[0];
        hash ^= rotl(k * c1, 15) * c2;
    }
    hash ^= quint32(length);
    hash ^= hash >> 16;
    hash *= 0x85ebca6b;
    hash ^= hash >> 13;
    hash *= 0xc2b2ae35;
    hash ^= hash >> 16;
    return hash;
}

// Channels seen by the same people share a member list, named after the overwrites that decide who can see
// the channel. Only a guess, used to recognize the list already on screen; the Gateway has the final word.
QString guessMemberListId(const Channel& channel)
{
    QStringList parts;
    for (const PermissionOverwrite& overwrite : channel.overwrites) {
        if (overwrite.allow & Permissions::ViewChannel)
            parts.append(QStringLiteral("allow:") + overwrite.id);
        else if (overwrite.deny & Permissions::ViewChannel)
            parts.append(QStringLiteral("deny:") + overwrite.id);
    }
    return parts.isEmpty() ? QStringLiteral("everyone") : QString::number(murmur3(parts.join(u',').toUtf8()));
}

} // namespace

QJsonArray Session::memberListRanges(int lastRow)
{
    // Rows come in chunks of 100: always the first one, plus the chunk on screen and the one before it.
    // Not `QJsonArray{QJsonArray{0, 99}}`: braces around a single array copy it instead of nesting it.
    QJsonArray ranges;
    ranges.append(QJsonArray{0, 99});
    const int chunk = std::max(0, lastRow) / 100;
    for (int i = std::max(1, chunk - 1); i <= chunk; ++i)
        ranges.append(QJsonArray{i * 100, i * 100 + 99});
    return ranges;
}

void Session::subscribeMemberList(const QString& guildId, const QString& channelId, int lastRow)
{
    if (guildId.isEmpty() || channelId.isEmpty())
        return;
    const QJsonArray ranges = memberListRanges(lastRow);
    if (guildId == m_listGuildId && channelId == m_listChannelId && ranges == m_listRanges)
        return;

    if (guildId != m_listGuildId || channelId != m_listChannelId) {
        GuildMemberLists& guild = m_memberLists[guildId];
        if (!guild.channelLists.contains(channelId)) {
            // A channel not opened before may share a list already here; otherwise the Gateway sends it.
            const Channel* channel = this->channel(guildId, channelId);
            const QString guess = channel ? guessMemberListId(*channel) : QString();
            if (guild.lists.contains(guess))
                guild.channelLists.insert(channelId, guess);
        }
        m_listGuildId = guildId;
        m_listChannelId = channelId;
        emit memberListChanged(guildId);
    }
    m_listRanges = ranges;
    if (m_rest->isOffline())
        return;
    m_gateway->updateGuildSubscriptions(guildId, QJsonObject{
                                                     {QStringLiteral("typing"), true},
                                                     {QStringLiteral("activities"), true},
                                                     {QStringLiteral("threads"), true},
                                                     {QStringLiteral("channels"), QJsonObject{{channelId, ranges}}},
                                                 });
}

const MemberList* Session::memberList(const QString& guildId, const QString& channelId) const
{
    const auto guild = m_memberLists.constFind(guildId);
    if (guild == m_memberLists.cend())
        return nullptr;
    const auto list = guild->lists.constFind(guild->channelLists.value(channelId));
    return list == guild->lists.cend() ? nullptr : &*list;
}

void Session::onMemberListUpdate(const QJsonObject& data)
{
    const QString guildId = data.value(u"guild_id").toString();
    const auto guild = m_memberLists.find(guildId);
    if (guild == m_memberLists.end())
        return;
    const QString listId = data.value(u"id").toString();
    const QJsonArray ops = data.value(u"ops").toArray();
    auto list = guild->lists.find(listId);
    if (list == guild->lists.end()) {
        // A new list starts with a full copy of its rows.
        const bool sync = std::any_of(ops.begin(), ops.end(), [](const QJsonValue& op) {
            return op.toObject().value(u"op").toString() == u"SYNC";
        });
        if (!sync)
            return;
        // The first one after subscribing tells which list the channel on screen uses.
        if (guildId == m_listGuildId && !guild->channelLists.contains(m_listChannelId))
            guild->channelLists.insert(m_listChannelId, listId);
        list = guild->lists.insert(listId, MemberList{listId, guildId});
        qCInfo(lcGateway) << "member list" << listId << "received," << data.value(u"member_count").toInt() << "members";
    }

    list->memberCount = data.value(u"member_count").toInt();
    list->onlineCount = data.value(u"online_count").toInt();
    QList<MemberListItem>& items = list->items;
    auto reserveRows = [&items](qsizetype count) {
        if (items.size() < count)
            items.resize(count);
    };
    for (const QJsonValue& value : ops) {
        const QJsonObject op = value.toObject();
        const QString type = op.value(u"op").toString();
        const QJsonArray range = op.value(u"range").toArray();
        const int index = op.value(u"index").toInt();
        if (type == u"SYNC") {
            const int first = range.at(0).toInt();
            const QJsonArray rows = op.value(u"items").toArray();
            reserveRows(first + rows.size());
            for (qsizetype i = 0; i < rows.size(); ++i)
                items[first + i] = memberListItem(guildId, rows.at(i).toObject());
        } else if (type == u"INSERT") {
            reserveRows(index);
            items.insert(index, memberListItem(guildId, op.value(u"item").toObject()));
        } else if (type == u"UPDATE") {
            reserveRows(index + 1);
            items[index] = memberListItem(guildId, op.value(u"item").toObject());
        } else if (type == u"DELETE") {
            if (index < items.size())
                items.removeAt(index);
        } else if (type == u"INVALIDATE") {
            const int last = std::min(range.at(1).toInt(), int(items.size()) - 1);
            for (int i = std::max(0, range.at(0).toInt()); i <= last; ++i)
                items[i] = {};
        }
    }
    // Each non-empty group is a header row followed by its members.
    qsizetype rows = 0;
    for (const QJsonValue& value : data.value(u"groups").toArray()) {
        const int count = value.toObject().value(u"count").toInt();
        if (count > 0)
            rows += 1 + count;
    }
    items.resize(rows);
    emit memberListChanged(guildId);
}

MemberListItem Session::memberListItem(const QString& guildId, const QJsonObject& json)
{
    MemberListItem item;
    if (json.contains(u"group")) {
        const QJsonObject group = json.value(u"group").toObject();
        item.groupId = group.value(u"id").toString();
        item.groupCount = group.value(u"count").toInt();
        return item;
    }
    const QJsonObject member = json.value(u"member").toObject();
    storeMember(guildId, member);
    item.userId = member.value(u"user").toObject().value(u"id").toString();
    item.nick = member.value(u"nick").toString();
    for (const QJsonValue& role : member.value(u"roles").toArray())
        item.roleIds.append(role.toString());
    QJsonObject presence = member.value(u"presence").toObject();
    if (!presence.isEmpty()) {
        presence.insert(QStringLiteral("user_id"), item.userId);
        storePresence(presence);
    }
    return item;
}

void Session::setLocalActivity(const QString& source, const QJsonObject& activity)
{
    if (activity.isEmpty() ? !m_localActivities.contains(source) : m_localActivities.value(source) == activity)
        return;
    if (activity.isEmpty())
        m_localActivities.remove(source);
    else
        m_localActivities.insert(source, activity);
    m_presenceTimer.start();
}

void Session::sendOwnPresence()
{
    m_presenceTimer.stop();
    if (!m_gatewayReady)
        return; // READY sends it
    QJsonArray activities;
    if (m_selfCustomStatus.isActive())
        activities.append(Activity::customStatus(m_selfCustomStatus.text, m_selfCustomStatus.emojiName,
                                                 m_selfCustomStatus.emojiId)
                              .toJson());
    for (const QJsonObject& activity : std::as_const(m_localActivities))
        activities.append(activity);
    m_gateway->updatePresence(statusToString(m_selfStatus), activities);
}

namespace {

constexpr qint64 ProfileLifetimeMs = 3 * 60 * 1000;

// Discord errors look like {"message": "Invalid Form Body", "errors": {"bio": {"_errors": [{"message": "..."}]}}};
// the first field error is the most useful part.
QString findFieldError(const QJsonObject& errors)
{
    for (auto it = errors.begin(); it != errors.end(); ++it) {
        const QJsonObject object = it.value().toObject();
        const QJsonArray list = object.value(u"_errors").toArray();
        if (!list.isEmpty())
            return list.first().toObject().value(u"message").toString();
        const QString nested = findFieldError(object);
        if (!nested.isEmpty())
            return nested;
    }
    return {};
}

QString errorText(const RestClient::Response& response)
{
    const QJsonObject body = response.body.object();
    if (body.contains(u"captcha_key"))
        return QCoreApplication::translate("Session", "Discord asked for a captcha. Make this change in the official app.");
    const QString field = findFieldError(body.value(u"errors").toObject());
    if (!field.isEmpty())
        return field;
    if (body.contains(u"message"))
        return body.value(u"message").toString();
    if (!response.networkError.isEmpty())
        return response.networkError;
    return QCoreApplication::translate("Session", "Request failed (HTTP %1).").arg(response.status);
}

} // namespace

void Session::fetchProfile(const QString& userId, const QString& guildId, ProfileCallback callback)
{
    const QString key = userId + u'/' + guildId;
    const auto cached = m_profiles.constFind(key);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (cached != m_profiles.cend() && (cached->fetchedAt == 0 || now - cached->fetchedAt < ProfileLifetimeMs)) {
        callback(&cached->profile, QString());
        return;
    }

    QString path = QStringLiteral("/users/%1/profile?with_mutual_guilds=true&with_mutual_friends_count=false").arg(userId);
    if (!guildId.isEmpty())
        path += QStringLiteral("&guild_id=") + guildId;
    m_rest->get(path, [this, key, guildId, userId, callback](const RestClient::Response& response) {
        if (!response.ok()) {
            callback(nullptr, errorText(response));
            return;
        }
        UserProfile profile = UserProfile::fromJson(response.body.object());
        if (profile.guildId.isEmpty())
            profile.guildId = guildId;
        if (!profile.user.id.isEmpty() && profile.user.id != m_self.id)
            m_users.insert(profile.user.id, profile.user); // the freshest name and picture
        if (!guildId.isEmpty() && (profile.joinedAt.isValid() || !profile.roleIds.isEmpty()))
            m_memberRoles[guildId].insert(profile.user.id.isEmpty() ? userId : profile.user.id, profile.roleIds);
        // Profiles are only kept while a popup might reopen them; this bounds the cache.
        if (m_profiles.size() > 64)
            m_profiles.clear();
        m_profiles.insert(key, {profile, QDateTime::currentMSecsSinceEpoch()});
        callback(&profile, QString());
    });
}

void Session::forgetOwnProfile()
{
    const QString prefix = m_self.id + u'/';
    for (auto it = m_profiles.begin(); it != m_profiles.end();) {
        if (it.key().startsWith(prefix) && it->fetchedAt != 0)
            it = m_profiles.erase(it);
        else
            ++it;
    }
}

void Session::cacheProfile(const UserProfile& profile)
{
    m_profiles.insert(profile.user.id + u'/' + profile.guildId, {profile, 0});
}

void Session::updateProfile(const ProfileChanges& changes, ResultCallback callback)
{
    QJsonObject account;
    if (changes.globalName)
        account.insert(QStringLiteral("global_name"),
                       changes.globalName->isEmpty() ? QJsonValue() : QJsonValue(*changes.globalName));
    if (changes.avatar)
        account.insert(QStringLiteral("avatar"), changes.avatar->isEmpty() ? QJsonValue() : QJsonValue(*changes.avatar));

    QJsonObject profile;
    if (changes.pronouns)
        profile.insert(QStringLiteral("pronouns"), *changes.pronouns);
    if (changes.bio)
        profile.insert(QStringLiteral("bio"), *changes.bio);
    if (changes.accentColor)
        profile.insert(QStringLiteral("accent_color"), *changes.accentColor < 0 ? QJsonValue() : QJsonValue(*changes.accentColor));

    // The display name and picture belong to the account; the rest to the profile. Two requests, one after the other.
    auto patchProfile = [this, profile, callback] {
        if (profile.isEmpty()) {
            callback(QString());
            return;
        }
        m_rest->patch(QStringLiteral("/users/@me/profile"), QJsonDocument(profile),
                      [this, callback](const RestClient::Response& response) {
                          if (!response.ok()) {
                              callback(errorText(response));
                              return;
                          }
                          forgetOwnProfile();
                          emit selfProfileChanged();
                          callback(QString());
                      });
    };
    if (account.isEmpty()) {
        patchProfile();
        return;
    }
    m_rest->patch(QStringLiteral("/users/@me"), QJsonDocument(account),
                  [this, patchProfile, callback](const RestClient::Response& response) {
                      if (!response.ok()) {
                          callback(errorText(response));
                          return;
                      }
                      const User updated = User::fromJson(response.body.object());
                      if (updated.id == m_self.id) {
                          m_self = updated;
                          emit usersChanged();
                      }
                      patchProfile();
                  });
}

void Session::loadGuildFolders(const QList<GuildFolder>& folders)
{
    // Guilds keep their known order; ones never placed (just joined) come after and end up at the top.
    QStringList known;
    for (const QString& id : std::as_const(m_guildOrder)) {
        if (m_guilds.contains(id))
            known.append(id);
    }
    for (auto it = m_guilds.cbegin(); it != m_guilds.cend(); ++it) {
        if (!known.contains(it.key()))
            known.append(it.key());
    }
    m_guildFolders = GuildFolders::normalized(folders, known);
    m_guildOrder = GuildFolders::flatten(m_guildFolders);
}

void Session::setGuildFolders(const QList<GuildFolder>& folders)
{
    const QList<GuildFolder> previous = m_guildFolders;
    loadGuildFolders(folders);
    if (m_guildFolders == previous)
        return;
    emit guildListChanged();
    patchSettings({{QStringLiteral("guild_folders"), GuildFolders::toJson(m_guildFolders)}});
}

void Session::fetchInvite(const QString& code, bool typed, InviteCallback callback)
{
    // Invites change rarely; a looked up one (or a dead link) is remembered for a while.
    constexpr qint64 InviteCacheMs = 5 * 60 * 1000;
    const auto cached = m_invites.constFind(code);
    if (cached != m_invites.cend()
        && (cached->fetchedAt == 0 || QDateTime::currentMSecsSinceEpoch() - cached->fetchedAt < InviteCacheMs)) {
        if (callback)
            callback(cached->invite ? &*cached->invite : nullptr, cached->error);
        return;
    }
    auto pending = m_pendingInvites.find(code);
    if (pending != m_pendingInvites.end()) {
        pending->append(std::move(callback));
        return;
    }
    m_pendingInvites[code].append(std::move(callback));

    const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(code));
    QString path = QStringLiteral("/invites/") + encoded;
    path += typed ? QStringLiteral("?inputValue=%1&with_counts=true&with_expiration=true").arg(encoded)
                  : QStringLiteral("?with_counts=true&with_expiration=true");
    m_rest->get(path, [this, code](const RestClient::Response& response) {
        CachedInvite entry;
        entry.fetchedAt = QDateTime::currentMSecsSinceEpoch();
        if (response.ok())
            entry.invite = InviteInfo::fromJson(response.body.object());
        else if (response.status == 404)
            entry.error = QCoreApplication::translate("Session", "This invite is invalid or has expired.");
        else
            entry.error = errorText(response);
        // Network failures are not remembered: the next look retries.
        if (response.status != 0)
            m_invites.insert(code, entry);
        const QList<InviteCallback> callbacks = m_pendingInvites.take(code);
        for (const InviteCallback& callback : callbacks) {
            if (callback)
                callback(entry.invite ? &*entry.invite : nullptr, entry.error);
        }
        emit inviteLoaded(code);
    });
}

const InviteInfo* Session::cachedInvite(const QString& code) const
{
    const auto it = m_invites.constFind(code);
    return it != m_invites.cend() && it->invite ? &*it->invite : nullptr;
}

void Session::cacheInvite(const InviteInfo& invite)
{
    m_invites.insert(invite.code, CachedInvite{invite, {}, 0});
}

bool Session::inviteFailed(const QString& code) const
{
    const auto it = m_invites.constFind(code);
    return it != m_invites.cend() && !it->invite;
}

void Session::acceptInvite(const InviteInfo& invite, const InviteSource& source, ResultCallback callback)
{
    // The official client says where the join started: the invite card of a message, or the dialog.
    OrderedJson context;
    if (source.messageId.isEmpty()) {
        context.insert(u"location", QStringLiteral("Join Guild"))
            .insert(u"location_guild_id", invite.guildId)
            .insert(u"location_channel_id", invite.channelId)
            .insert(u"location_channel_type", invite.channelType);
    } else {
        context.insert(u"location", QStringLiteral("Invite Button Embed"))
            .insert(u"location_guild_id", source.guildId.isEmpty() ? QJsonValue() : QJsonValue(source.guildId))
            .insert(u"location_channel_id", source.channelId)
            .insert(u"location_channel_type", source.channelType)
            .insert(u"location_message_id", source.messageId);
    }
    const QJsonObject body{{QStringLiteral("session_id"), sessionId()}};
    m_rest->post(QStringLiteral("/invites/") + QString::fromLatin1(QUrl::toPercentEncoding(invite.code)), QJsonDocument(body),
                 [callback](const RestClient::Response& response) {
                     // The guild itself arrives through GUILD_CREATE.
                     if (callback)
                         callback(response.ok() ? QString() : errorText(response));
                 },
                 &context);
}

QString Session::inviteChannel(const QString& guildId, const QString& preferred) const
{
    const Guild* g = guild(guildId);
    if (!g)
        return {};
    auto allowed = [&](const Channel& c) {
        if (c.type == ChannelType::GuildCategory)
            return false;
        const quint64 permissions = Permissions::compute(*g, c, m_self.id);
        return (permissions & Permissions::ViewChannel) && (permissions & Permissions::CreateInstantInvite);
    };
    if (const Channel* c = channel(guildId, preferred); c && allowed(*c))
        return c->id;
    for (const Channel& c : visibleChannels(guildId)) {
        if (allowed(c))
            return c.id;
    }
    return {};
}

void Session::createInvite(const QString& channelId, InviteCallback callback)
{
    // The defaults of the official "Invite People" dialog: expires after 7 days, no use limit.
    const QJsonObject body{{QStringLiteral("max_age"), 604800},
                           {QStringLiteral("max_uses"), 0},
                           {QStringLiteral("target_type"), QJsonValue()},
                           {QStringLiteral("temporary"), false}};
    m_rest->post(QStringLiteral("/channels/%1/invites").arg(channelId), QJsonDocument(body),
                 [this, callback](const RestClient::Response& response) {
                     if (!response.ok()) {
                         if (callback)
                             callback(nullptr, errorText(response));
                         return;
                     }
                     CachedInvite entry;
                     entry.invite = InviteInfo::fromJson(response.body.object());
                     entry.fetchedAt = QDateTime::currentMSecsSinceEpoch();
                     m_invites.insert(entry.invite->code, entry);
                     if (callback)
                         callback(&*entry.invite, QString());
                 });
}

void Session::leaveGuild(const QString& guildId, ResultCallback callback)
{
    // GUILD_DELETE follows and removes it from the list.
    m_rest->deleteResource(QStringLiteral("/users/@me/guilds/") + guildId,
                           QJsonDocument(QJsonObject{{QStringLiteral("lurking"), false}}),
                           [callback](const RestClient::Response& response) {
                               if (callback)
                                   callback(response.ok() ? QString() : errorText(response));
                           });
}

bool Session::canManageChannels(const QString& guildId, const QString& channelId) const
{
    const Guild* g = guild(guildId);
    if (!g)
        return false;
    if (channelId.isEmpty())
        return Permissions::compute(*g, Channel(), m_self.id) & Permissions::ManageChannels;
    const Channel* c = channel(guildId, channelId);
    if (!c)
        return false;
    const quint64 permissions = Permissions::compute(*g, *c, m_self.id);
    return (permissions & Permissions::ViewChannel) && (permissions & Permissions::ManageChannels);
}

void Session::storeChannel(const QJsonObject& json)
{
    // The Gateway sends the same channel again (CHANNEL_CREATE / CHANNEL_UPDATE); storing the reply right
    // away lets the interface show the change without waiting for it.
    const Channel channel = Channel::fromJson(json, QString());
    auto it = m_guilds.find(channel.guildId);
    if (channel.id.isEmpty() || it == m_guilds.end())
        return;
    it->channels.insert(channel.id, channel);
    emit guildChanged(channel.guildId);
}

void Session::createChannel(const QString& guildId, ChannelType type, const QString& name, const QString& parentId,
                            ChannelCallback callback)
{
    // The body of the official "Create Channel" dialog (for a channel that is not private).
    QJsonObject body{{QStringLiteral("type"), static_cast<int>(type)},
                     {QStringLiteral("name"), name},
                     {QStringLiteral("permission_overwrites"), QJsonArray()}};
    if (!parentId.isEmpty())
        body.insert(QStringLiteral("parent_id"), parentId);
    m_rest->post(QStringLiteral("/guilds/%1/channels").arg(guildId), QJsonDocument(body),
                 [this, callback](const RestClient::Response& response) {
                     if (!response.ok()) {
                         if (callback)
                             callback(QString(), errorText(response));
                         return;
                     }
                     storeChannel(response.body.object());
                     if (callback)
                         callback(response.body.object().value(u"id").toString(), QString());
                 });
}

void Session::editChannel(const QString& channelId, const QJsonObject& changes, ResultCallback callback)
{
    m_rest->patch(QStringLiteral("/channels/") + channelId, QJsonDocument(changes),
                  [this, callback](const RestClient::Response& response) {
                      if (response.ok())
                          storeChannel(response.body.object());
                      if (callback)
                          callback(response.ok() ? QString() : errorText(response));
                  });
}

void Session::deleteChannel(const QString& guildId, const QString& channelId, ResultCallback callback)
{
    m_rest->deleteResource(QStringLiteral("/channels/") + channelId,
                           [this, guildId, channelId, callback](const RestClient::Response& response) {
                               if (response.ok()) {
                                   auto it = m_guilds.find(guildId);
                                   if (it != m_guilds.end() && it->channels.remove(channelId))
                                       emit guildChanged(guildId);
                               }
                               if (callback)
                                   callback(response.ok() ? QString() : errorText(response));
                           });
}

void Session::patchSettings(const QJsonObject& changes)
{
    // The legacy JSON settings endpoint still syncs status and custom status to the other clients.
    m_rest->patch(QStringLiteral("/users/@me/settings"), QJsonDocument(changes), nullptr);
}

void Session::setStatus(UserStatus status)
{
    m_selfStatus = status;
    patchSettings({{QStringLiteral("status"), statusToString(status)}});
    sendOwnPresence();
    emit presenceChanged(m_self.id, true);
}

void Session::setCustomStatus(const CustomStatus& status)
{
    m_selfCustomStatus = status;
    QJsonValue value;
    if (!status.isEmpty()) {
        QJsonObject json{{QStringLiteral("text"), status.text.isEmpty() ? QJsonValue() : QJsonValue(status.text)},
                         {QStringLiteral("emoji_name"), status.emojiName.isEmpty() ? QJsonValue() : QJsonValue(status.emojiName)},
                         {QStringLiteral("emoji_id"), status.emojiId.isEmpty() ? QJsonValue() : QJsonValue(status.emojiId)},
                         {QStringLiteral("expires_at"), status.expiresAt.isValid()
                                                            ? QJsonValue(status.expiresAt.toUTC().toString(Qt::ISODateWithMs))
                                                            : QJsonValue()}};
        value = json;
    }
    patchSettings({{QStringLiteral("custom_status"), value}});
    sendOwnPresence();
    emit presenceChanged(m_self.id, true);
}

void Session::loadReadStates(const QJsonValue& value)
{
    // A plain array, or {"entries": [...]} with the versioned read states capability.
    const QJsonArray entries = value.isArray() ? value.toArray() : value.toObject().value(u"entries").toArray();
    for (const QJsonValue& entry : entries) {
        const QJsonObject json = entry.toObject();
        if (json.value(u"read_state_type").toInt(0) != 0)
            continue; // only channel read states matter here
        ReadState state;
        state.lastAckedId = json.value(u"last_message_id").isString()
            ? json.value(u"last_message_id").toString()
            : QString::number(json.value(u"last_message_id").toInteger());
        state.mentionCount = json.value(u"mention_count").toInt();
        m_readStates.insert(json.value(u"id").toString(), state);
    }
}

void Session::loadGuildSettings(const QJsonObject& json)
{
    GuildSettings settings;
    // A temporary mute ("mute_config" with an end time) still counts while it lasts; Snapcord treats it as muted.
    settings.muted = json.value(u"muted").toBool();
    settings.suppressEveryone = json.value(u"suppress_everyone").toBool();
    settings.suppressRoles = json.value(u"suppress_roles").toBool();
    for (const QJsonValue& value : json.value(u"channel_overrides").toArray()) {
        const QJsonObject override = value.toObject();
        if (override.value(u"muted").toBool())
            settings.mutedChannels.insert(override.value(u"channel_id").toString());
    }
    m_guildSettings.insert(json.value(u"guild_id").toString(), settings);
}

QString Session::lastMessageId(const QString& guildId, const QString& channelId) const
{
    if (guildId.isEmpty()) {
        const PrivateChannel* channel = privateChannel(channelId);
        return channel ? channel->lastMessageId : QString();
    }
    const Channel* c = channel(guildId, channelId);
    return c ? c->lastMessageId : QString();
}

bool Session::isUnread(const QString& guildId, const QString& channelId) const
{
    const QString last = lastMessageId(guildId, channelId);
    if (last.isEmpty())
        return false;
    const auto it = m_readStates.constFind(channelId);
    // Channels never opened have no read state; Discord only shows them as unread once a read state exists.
    if (it == m_readStates.cend())
        return false;
    return snowflakeLess(it->lastAckedId, last);
}

int Session::mentionCount(const QString& channelId) const
{
    return m_readStates.value(channelId).mentionCount;
}

bool Session::isMuted(const QString& guildId, const QString& channelId) const
{
    const auto it = m_guildSettings.constFind(guildId);
    if (it == m_guildSettings.cend())
        return false;
    if (it->muted && !guildId.isEmpty())
        return true;
    if (channelId.isEmpty())
        return false;
    if (it->mutedChannels.contains(channelId))
        return true;
    // A muted category mutes the channels inside it.
    const Channel* c = guildId.isEmpty() ? nullptr : channel(guildId, channelId);
    return c && !c->parentId.isEmpty() && it->mutedChannels.contains(c->parentId);
}

bool Session::guildHasUnread(const QString& guildId) const
{
    const Guild* g = guild(guildId);
    if (!g || isMuted(guildId, QString()))
        return false;
    for (const Channel& c : g->channels) {
        if (c.isVoice() || c.type == ChannelType::GuildCategory || isMuted(guildId, c.id))
            continue;
        if (isUnread(guildId, c.id) && (Permissions::compute(*g, c, m_self.id) & Permissions::ViewChannel))
            return true;
    }
    return false;
}

int Session::guildMentionCount(const QString& guildId) const
{
    const Guild* g = guild(guildId);
    if (!g)
        return 0;
    int total = 0;
    for (const Channel& c : g->channels)
        total += m_readStates.value(c.id).mentionCount;
    return total;
}

int Session::privateMentionCount() const
{
    int total = 0;
    for (const PrivateChannel& channel : m_privateChannels)
        total += m_readStates.value(channel.id).mentionCount;
    return total;
}

void Session::markRead(const QString& guildId, const QString& channelId)
{
    const QString last = lastMessageId(guildId, channelId);
    if (last.isEmpty())
        return;
    ReadState& state = m_readStates[channelId];
    if (!snowflakeLess(state.lastAckedId, last) && state.mentionCount == 0)
        return;
    state.lastAckedId = last;
    state.mentionCount = 0;
    m_rest->post(QStringLiteral("/channels/%1/messages/%2/ack").arg(channelId, last),
                 QJsonDocument(QJsonObject{{QStringLiteral("token"), QJsonValue()}}), nullptr);
    emit readStateChanged(guildId, channelId);
}

void Session::sendTyping(const QString& channelId)
{
    // Discord shows the indicator for about 10 seconds, so one request every 8 seconds is enough.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (channelId == m_lastTypingChannel && now - m_lastTypingSent < 8000)
        return;
    m_lastTypingChannel = channelId;
    m_lastTypingSent = now;
    m_rest->post(QStringLiteral("/channels/%1/typing").arg(channelId), QJsonDocument(QJsonObject()), nullptr);
}

QList<CustomEmoji> Session::customEmojis(const QString& guildId) const
{
    const Guild* g = guild(guildId);
    return g ? g->emojis : QList<CustomEmoji>();
}

bool Session::mentionsSelf(const Message& message) const
{
    if (message.mentionedUserIds.contains(m_self.id))
        return true;
    const GuildSettings settings = m_guildSettings.value(message.guildId);
    if (message.mentionsEveryone && !settings.suppressEveryone)
        return true;
    if (!settings.suppressRoles && !message.guildId.isEmpty()) {
        if (const Guild* g = guild(message.guildId)) {
            for (const QString& role : message.mentionedRoleIds) {
                if (g->selfRoleIds.contains(role))
                    return true;
            }
        }
    }
    return false;
}

void Session::onMessageCreate(const QJsonObject& data)
{
    const Message message = Message::fromJson(data);
    if (data.contains(u"member") && !message.guildId.isEmpty()) {
        QJsonObject member = data.value(u"member").toObject();
        const QJsonObject author = data.value(u"author").toObject();
        if (!author.isEmpty())
            member.insert(QStringLiteral("user"), author);
        storeMember(message.guildId, member);
    }
    const QString channelId = message.channelId;
    const bool isPrivate = message.guildId.isEmpty();

    if (isPrivate) {
        auto it = m_privateChannels.find(channelId);
        if (it == m_privateChannels.end())
            return;
        it->lastMessageId = message.id;
        emit privateChannelsChanged();
    } else {
        auto guildIt = m_guilds.find(message.guildId);
        if (guildIt == m_guilds.end())
            return;
        auto channelIt = guildIt->channels.find(channelId);
        if (channelIt != guildIt->channels.end())
            channelIt->lastMessageId = message.id;
    }

    ReadState& state = m_readStates[channelId];
    if (message.author.id == m_self.id) {
        // Sending a message reads the channel up to it.
        state.lastAckedId = message.id;
        state.mentionCount = 0;
    } else if (mentionsSelf(message) || (isPrivate && !isMuted(QString(), channelId))) {
        ++state.mentionCount;
        emit notificationMessage(message);
    }
    emit readStateChanged(message.guildId, channelId);
}

void Session::loadCall(const QJsonObject& data)
{
    Call call;
    call.channelId = data.value(u"channel_id").toString();
    for (const QJsonValue& id : data.value(u"ringing").toArray())
        call.ringing.append(id.toString());
    for (const QJsonValue& value : data.value(u"voice_states").toArray()) {
        const QJsonObject json = value.toObject();
        if (json.contains(u"member"))
            storeUser(json.value(u"member").toObject().value(u"user").toObject());
        const VoiceState state = VoiceState::fromJson(json, QString());
        if (!state.userId.isEmpty())
            call.voiceStates.insert(state.userId, state);
    }
    m_calls.insert(call.channelId, call);
    emit callChanged(call.channelId);
}

void Session::applyCallVoiceState(const VoiceState& state)
{
    // A user is in at most one call: drop them from wherever they were, then add them to the new one.
    for (auto it = m_calls.begin(); it != m_calls.end(); ++it) {
        if (it.key() != state.channelId && it->voiceStates.remove(state.userId))
            emit callChanged(it.key());
    }
    if (state.channelId.isEmpty())
        return;
    Call& call = m_calls[state.channelId];
    call.channelId = state.channelId;
    call.voiceStates.insert(state.userId, state);
    emit callChanged(state.channelId);
}

void Session::loadGuild(const QJsonObject& data)
{
    // With the CLIENT_STATE_V2 capability the guild fields live in "properties"; without it they are merged in.
    const QJsonObject properties = data.contains(u"properties") ? data.value(u"properties").toObject() : data;

    Guild guild;
    guild.id = data.value(u"id").toString();
    guild.name = properties.value(u"name").toString();
    guild.icon = properties.value(u"icon").toString();
    guild.ownerId = properties.value(u"owner_id").toString();
    guild.premiumTier = properties.value(u"premium_tier").toInt();
    guild.unavailable = data.value(u"unavailable").toBool();

    for (const QJsonValue& value : data.value(u"roles").toArray()) {
        const Role role = Role::fromJson(value.toObject());
        guild.roles.insert(role.id, role);
    }
    for (const QJsonValue& value : data.value(u"channels").toArray()) {
        const Channel channel = Channel::fromJson(value.toObject(), guild.id);
        guild.channels.insert(channel.id, channel);
    }
    const QJsonArray emojis = data.contains(u"emojis") ? data.value(u"emojis").toArray()
                                                       : properties.value(u"emojis").toArray();
    for (const QJsonValue& value : emojis) {
        const QJsonObject emoji = value.toObject();
        guild.emojis.append({emoji.value(u"id").toString(), emoji.value(u"name").toString(),
                             emoji.value(u"animated").toBool()});
    }
    m_guilds.insert(guild.id, guild);

    for (const QJsonValue& member : data.value(u"members").toArray())
        storeMember(guild.id, member.toObject());
    for (const QJsonValue& state : data.value(u"voice_states").toArray())
        applyVoiceState(VoiceState::fromJson(state.toObject(), guild.id));
    for (const QJsonValue& presence : data.value(u"presences").toArray())
        storePresence(presence.toObject());
}

void Session::applyVoiceState(const VoiceState& state)
{
    auto it = m_guilds.find(state.guildId);
    if (it == m_guilds.end() || state.userId.isEmpty())
        return;
    if (state.channelId.isEmpty()) {
        it->voiceStates.remove(state.userId);
        return;
    }
    it->voiceStates.insert(state.userId, state);
    if (state.userId != m_self.id && !m_users.contains(state.userId)) {
        m_missingUsers[state.guildId].insert(state.userId);
        m_missingUsersTimer.start();
    }
}

void Session::storeUser(const QJsonObject& json)
{
    const User user = User::fromJson(json);
    if (!user.id.isEmpty())
        m_users.insert(user.id, user);
}

std::optional<QStringList> Session::memberRoleIds(const QString& guildId, const QString& userId) const
{
    if (const Guild* guild = this->guild(guildId); guild && userId == m_self.id && !guild->selfRoleIds.isEmpty())
        return guild->selfRoleIds;
    const auto cached = m_memberRoles.constFind(guildId);
    if (cached != m_memberRoles.cend()) {
        const auto member = cached->constFind(userId);
        if (member != cached->cend())
            return *member;
    }
    // Rows the member sidebar already loaded know the same roles the list paints with.
    const auto lists = m_memberLists.constFind(guildId);
    if (lists == m_memberLists.cend())
        return std::nullopt;
    for (const MemberList& list : lists->lists) {
        for (const MemberListItem& item : list.items) {
            if (item.userId == userId)
                return item.roleIds;
        }
    }
    return std::nullopt;
}

void Session::ensureAuthorRoles(const QString& guildId, const QString& channelId)
{
    if (guildId.isEmpty() || channelId.isEmpty())
        return;
    bool queued = false;
    const auto remember = [this, &guildId, &queued](const QString& userId, const QStringList& roles) {
        if (userId.isEmpty() || userId == m_self.id)
            return;
        if (!roles.isEmpty() && !m_memberRoles.value(guildId).contains(userId))
            m_memberRoles[guildId].insert(userId, roles);
        if (memberRoleIds(guildId, userId))
            return;
        const QString key = guildId + u'/' + userId;
        if (m_rolesRequested.contains(key) || m_missingRoles.value(guildId).contains(userId))
            return;
        m_missingRoles[guildId].insert(userId);
        queued = true;
    };
    for (const Message& message : m_messages->messages(channelId)) {
        remember(message.author.id, message.memberRoleIds);
        remember(message.referencedAuthor.id, message.referencedMemberRoleIds);
    }
    if (queued)
        m_missingRolesTimer.start();
}

void Session::requestMissingRoles()
{
    for (auto it = m_missingRoles.begin(); it != m_missingRoles.end(); ++it) {
        QStringList ids;
        for (const QString& id : it.value()) {
            m_rolesRequested.insert(it.key() + u'/' + id);
            ids.append(id);
        }
        it.value().clear();
        for (qsizetype i = 0; i < ids.size(); i += 100)
            m_gateway->requestGuildMembers(it.key(), ids.mid(i, 100));
    }
}

int Session::memberColor(const QString& guildId, const QString& userId, const QStringList& fallbackRoleIds) const
{
    const Guild* guild = this->guild(guildId);
    if (!guild || userId.isEmpty())
        return 0;
    const QStringList roleIds = memberRoleIds(guildId, userId).value_or(fallbackRoleIds);
    const Role* best = nullptr;
    for (const QString& id : roleIds) {
        const auto it = guild->roles.constFind(id);
        if (it != guild->roles.cend() && it->color != 0 && (!best || it->position > best->position))
            best = &*it;
    }
    return best ? best->color : 0;
}

void Session::finishAction(const RestClient::Response& response, ResultCallback callback)
{
    if (callback)
        callback(response.ok() ? QString() : errorText(response));
}

void Session::patchGuildMember(const QString& guildId, const QString& userId, const QString& viewedChannelId,
                               const QJsonObject& body, ResultCallback callback)
{
    const QString previous = m_rest->referer();
    const QString page = viewedChannelId.isEmpty() ? guildId : viewedChannelId;
    m_rest->setReferer(QStringLiteral("https://discord.com/channels/%1/%2").arg(guildId, page));
    m_rest->patch(QStringLiteral("/guilds/%1/members/%2").arg(guildId, userId), QJsonDocument(body),
                  [this, callback](const RestClient::Response& response) { finishAction(response, callback); });
    m_rest->setReferer(previous);
}

void Session::setServerMute(const QString& guildId, const QString& userId, const QString& viewedChannelId, bool mute,
                            ResultCallback callback)
{
    patchGuildMember(guildId, userId, viewedChannelId, QJsonObject{{QStringLiteral("mute"), mute}}, std::move(callback));
}

void Session::setTimeout(const QString& guildId, const QString& userId, const QString& viewedChannelId, int seconds,
                         ResultCallback callback)
{
    QJsonObject body;
    if (seconds <= 0)
        body.insert(QStringLiteral("communication_disabled_until"), QJsonValue());
    else
        body.insert(QStringLiteral("communication_disabled_until"),
                    QDateTime::currentDateTimeUtc().addSecs(seconds).toString(Qt::ISODateWithMs));
    patchGuildMember(guildId, userId, viewedChannelId, body, std::move(callback));
}

void Session::kickMember(const QString& guildId, const QString& userId, const QString& viewedChannelId,
                         ResultCallback callback)
{
    const QString previous = m_rest->referer();
    const QString page = viewedChannelId.isEmpty() ? guildId : viewedChannelId;
    m_rest->setReferer(QStringLiteral("https://discord.com/channels/%1/%2").arg(guildId, page));
    m_rest->deleteResource(QStringLiteral("/guilds/%1/members/%2").arg(guildId, userId),
                           [this, callback](const RestClient::Response& response) { finishAction(response, callback); });
    m_rest->setReferer(previous);
}

void Session::banMember(const QString& guildId, const QString& userId, const QString& viewedChannelId,
                        ResultCallback callback)
{
    const QString previous = m_rest->referer();
    const QString page = viewedChannelId.isEmpty() ? guildId : viewedChannelId;
    m_rest->setReferer(QStringLiteral("https://discord.com/channels/%1/%2").arg(guildId, page));
    m_rest->put(QStringLiteral("/guilds/%1/bans/%2").arg(guildId, userId),
                QJsonDocument(QJsonObject{{QStringLiteral("delete_message_seconds"), 0}}),
                [this, callback](const RestClient::Response& response) { finishAction(response, callback); });
    m_rest->setReferer(previous);
}

void Session::blockUser(const QString& userId, ResultCallback callback)
{
    m_rest->put(QStringLiteral("/users/@me/relationships/%1").arg(userId),
                QJsonDocument(QJsonObject{{QStringLiteral("type"), Relationship::Blocked}}),
                [this, userId, callback](const RestClient::Response& response) {
                    if (response.ok()) {
                        m_relationships.insert(userId, Relationship::Blocked);
                        emit relationshipsChanged();
                    }
                    finishAction(response, callback);
                });
}

QList<Relationship> Session::relationships() const
{
    QList<Relationship> list;
    list.reserve(m_relationships.size());
    for (auto it = m_relationships.cbegin(); it != m_relationships.cend(); ++it)
        list.append({it.key(), it.value()});
    return list;
}

void Session::storeRelationship(const QJsonObject& json)
{
    const QJsonObject user = json.value(u"user").toObject();
    if (!user.isEmpty())
        storeUser(user);
    const QString id = user.isEmpty() ? json.value(u"id").toString() : user.value(u"id").toString();
    if (!id.isEmpty())
        m_relationships.insert(id, json.value(u"type").toInt());
}

void Session::addFriend(const QString& username, ResultCallback callback)
{
    const QString name = username.trimmed();
    if (name.isEmpty()) {
        if (callback)
            callback(tr("Enter a username."));
        return;
    }
    // The official client sends discriminator as null now that usernames are unique.
    const QJsonObject body{{QStringLiteral("username"), name}, {QStringLiteral("discriminator"), QJsonValue()}};
    m_rest->post(QStringLiteral("/users/@me/relationships"), QJsonDocument(body),
                 [this, callback](const RestClient::Response& response) {
                     if (response.ok() && response.body.isObject())
                         storeRelationship(response.body.object());
                     if (response.ok())
                         emit relationshipsChanged();
                     finishAction(response, callback);
                 });
}

void Session::acceptFriend(const QString& userId, ResultCallback callback)
{
    m_rest->put(QStringLiteral("/users/@me/relationships/%1").arg(userId),
                QJsonDocument(QJsonObject{{QStringLiteral("type"), Relationship::Friend}}),
                [this, userId, callback](const RestClient::Response& response) {
                    if (response.ok()) {
                        m_relationships.insert(userId, Relationship::Friend);
                        emit relationshipsChanged();
                    }
                    finishAction(response, callback);
                });
}

void Session::removeRelationship(const QString& userId, ResultCallback callback)
{
    m_rest->deleteResource(QStringLiteral("/users/@me/relationships/%1").arg(userId),
                           [this, userId, callback](const RestClient::Response& response) {
                               if (response.ok()) {
                                   m_relationships.remove(userId);
                                   emit relationshipsChanged();
                               }
                               finishAction(response, callback);
                           });
}

void Session::openDirectMessage(const QString& userId, DirectMessageCallback callback)
{
    for (const PrivateChannel& channel : privateChannels()) {
        if (!channel.isGroup() && channel.recipientIds.size() == 1 && channel.recipientIds.first() == userId) {
            if (callback)
                callback(channel.id, QString());
            return;
        }
    }
    m_rest->post(QStringLiteral("/users/@me/channels"),
                 QJsonDocument(QJsonObject{{QStringLiteral("recipients"), QJsonArray{userId}}}),
                 [this, callback](const RestClient::Response& response) {
                     if (!response.ok()) {
                         if (callback)
                             callback(QString(), errorText(response));
                         return;
                     }
                     const PrivateChannel channel = PrivateChannel::fromJson(response.body.object());
                     if (!channel.id.isEmpty())
                         m_privateChannels.insert(channel.id, channel);
                     emit privateChannelsChanged();
                     if (callback)
                         callback(channel.id, channel.id.isEmpty() ? tr("Could not open the conversation.") : QString());
                 });
}

void Session::setMemberRole(const QString& guildId, const QString& userId, const QString& viewedChannelId,
                            const QString& roleId, bool grant, ResultCallback callback)
{
    const QString previous = m_rest->referer();
    const QString page = viewedChannelId.isEmpty() ? guildId : viewedChannelId;
    m_rest->setReferer(QStringLiteral("https://discord.com/channels/%1/%2").arg(guildId, page));
    const QString path = QStringLiteral("/guilds/%1/members/%2/roles/%3").arg(guildId, userId, roleId);
    const auto done = [this, callback](const RestClient::Response& response) { finishAction(response, callback); };
    if (grant)
        m_rest->put(path, done);
    else
        m_rest->deleteResource(path, done);
    m_rest->setReferer(previous);
}

void Session::moveMember(const QString& guildId, const QString& userId, const QString& viewedChannelId,
                         const QString& channelId, ResultCallback callback)
{
    QJsonObject body;
    body.insert(QStringLiteral("channel_id"), channelId.isEmpty() ? QJsonValue() : QJsonValue(channelId));
    patchGuildMember(guildId, userId, viewedChannelId, body, std::move(callback));
}

void Session::storeMember(const QString& guildId, const QJsonObject& member)
{
    const QJsonObject userJson = member.value(u"user").toObject();
    if (!userJson.isEmpty())
        storeUser(userJson);
    const QString userId = userJson.isEmpty() ? member.value(u"user_id").toString() : userJson.value(u"id").toString();
    if (!guildId.isEmpty() && !userId.isEmpty()) {
        m_guildMembers[guildId].insert(userId, member.value(u"nick").toString());
        if (member.contains(u"roles")) {
            QStringList roles;
            for (const QJsonValue& role : member.value(u"roles").toArray())
                roles.append(role.toString());
            m_memberRoles[guildId].insert(userId, roles);
        }
    }
    if (userId == m_self.id && !m_self.id.isEmpty()) {
        auto it = m_guilds.find(guildId);
        if (it != m_guilds.end() && member.contains(u"roles")) {
            it->selfRoleIds.clear();
            for (const QJsonValue& role : member.value(u"roles").toArray())
                it->selfRoleIds.append(role.toString());
        }
    }
}

void Session::requestMissingUsers()
{
    for (auto it = m_missingUsers.cbegin(); it != m_missingUsers.cend(); ++it) {
        QStringList ids;
        for (const QString& id : it.value()) {
            if (!m_users.contains(id))
                ids.append(id);
        }
        // The Gateway accepts up to 100 user IDs per request.
        for (qsizetype i = 0; i < ids.size(); i += 100)
            m_gateway->requestGuildMembers(it.key(), ids.mid(i, 100));
    }
    m_missingUsers.clear();
}
