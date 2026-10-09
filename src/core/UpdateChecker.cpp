#include "UpdateChecker.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <chrono>

namespace {

constexpr qint64 CheckIntervalMs = 24LL * 60 * 60 * 1000;
constexpr int StartupDelayMs = 15 * 1000;
constexpr int TimeoutMs = 20 * 1000;

const QString AutomaticKey = QStringLiteral("updates/automatic");
const QString PrereleasesKey = QStringLiteral("updates/prereleases");
const QString SkippedKey = QStringLiteral("updates/skippedVersion");
const QString LastCheckKey = QStringLiteral("updates/lastCheck");

QString stripPrefix(QString version)
{
    version = version.trimmed();
    if (version.startsWith(u'v') || version.startsWith(u'V'))
        version.remove(0, 1);
    return version;
}

// The release package that fits this system, by file extension. Returns an empty URL when nothing fits
// (a Linux install that is neither Flatpak nor AppImage), so the caller falls back to the release page.
QUrl platformAsset(const QJsonArray& assets)
{
    QStringList wanted;
#if defined(Q_OS_WIN)
    wanted = {QStringLiteral(".exe"), QStringLiteral(".zip")};
#elif defined(Q_OS_MACOS)
    wanted = {QStringLiteral(".dmg")};
#else
    if (qEnvironmentVariableIsSet("FLATPAK_ID"))
        wanted = {QStringLiteral(".flatpak")};
    else if (qEnvironmentVariableIsSet("APPIMAGE"))
        wanted = {QStringLiteral(".AppImage")};
#endif
    for (const QString& suffix : wanted) {
        for (const QJsonValue& value : assets) {
            const QJsonObject asset = value.toObject();
            if (asset.value(u"name").toString().endsWith(suffix, Qt::CaseInsensitive))
                return QUrl(asset.value(u"browser_download_url").toString());
        }
    }
    return {};
}

QNetworkRequest apiRequest(const QString& path)
{
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com") + path));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Snapcord/%1").arg(UpdateChecker::currentVersion()));
    request.setTransferTimeout(TimeoutMs);
    return request;
}

QString describeError(QNetworkReply* reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 403 || status == 429)
        return QCoreApplication::translate("UpdateChecker", "GitHub is limiting requests right now. Try again later.");
    if (reply->error() != QNetworkReply::NoError && status == 0)
        return QCoreApplication::translate("UpdateChecker", "Could not reach GitHub. Check your internet connection.");
    return QCoreApplication::translate("UpdateChecker", "GitHub returned an unexpected answer (HTTP %1).").arg(status);
}

} // namespace

UpdateChecker& UpdateChecker::instance()
{
    static auto* checker = new UpdateChecker(QCoreApplication::instance());
    return *checker;
}

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this] { check(false); });
}

QUrl UpdateChecker::repositoryUrl()
{
    return QUrl(QStringLiteral("https://github.com/") + repository());
}

QString UpdateChecker::currentVersion()
{
    return QCoreApplication::applicationVersion();
}

bool UpdateChecker::automaticChecks()
{
    return QSettings().value(AutomaticKey, true).toBool();
}

void UpdateChecker::setAutomaticChecks(bool enabled)
{
    QSettings().setValue(AutomaticKey, enabled);
    UpdateChecker& checker = instance();
    if (!checker.m_automatic)
        return;
    if (enabled)
        checker.scheduleNext();
    else
        checker.m_timer->stop();
}

bool UpdateChecker::includePrereleases()
{
    return QSettings().value(PrereleasesKey, false).toBool();
}

void UpdateChecker::setIncludePrereleases(bool enabled)
{
    QSettings().setValue(PrereleasesKey, enabled);
}

QString UpdateChecker::skippedVersion()
{
    return QSettings().value(SkippedKey).toString();
}

void UpdateChecker::setSkippedVersion(const QString& version)
{
    if (version.isEmpty())
        QSettings().remove(SkippedKey);
    else
        QSettings().setValue(SkippedKey, version);
}

QDateTime UpdateChecker::lastCheck()
{
    return QSettings().value(LastCheckKey).toDateTime();
}

int UpdateChecker::compareVersions(const QString& a, const QString& b)
{
    // "1.2.3-beta.1+build" -> numbers [1, 2, 3], pre-release "beta.1" (build metadata is ignored).
    auto split = [](const QString& version) {
        QString core = stripPrefix(version).section(u'+', 0, 0);
        const qsizetype dash = core.indexOf(u'-');
        const QString pre = dash >= 0 ? core.mid(dash + 1) : QString();
        if (dash >= 0)
            core.truncate(dash);
        QVector<int> numbers;
        for (const QString& part : core.split(u'.'))
            numbers.append(part.toInt());
        return std::pair{numbers, pre};
    };
    const auto [numbersA, preA] = split(a);
    const auto [numbersB, preB] = split(b);
    for (qsizetype i = 0; i < std::max(numbersA.size(), numbersB.size()); ++i) {
        const int x = numbersA.value(i);
        const int y = numbersB.value(i);
        if (x != y)
            return x < y ? -1 : 1;
    }
    if (preA.isEmpty() || preB.isEmpty())
        return preA.isEmpty() == preB.isEmpty() ? 0 : (preA.isEmpty() ? 1 : -1);

    // Pre-release identifiers compare one by one: numbers numerically, text alphabetically, numbers first.
    const QStringList partsA = preA.split(u'.');
    const QStringList partsB = preB.split(u'.');
    for (qsizetype i = 0; i < std::min(partsA.size(), partsB.size()); ++i) {
        bool numericA = false;
        bool numericB = false;
        const int x = partsA[i].toInt(&numericA);
        const int y = partsB[i].toInt(&numericB);
        if (numericA && numericB) {
            if (x != y)
                return x < y ? -1 : 1;
        } else if (numericA != numericB) {
            return numericA ? -1 : 1;
        } else if (const int order = partsA[i].compare(partsB[i]); order != 0) {
            return order < 0 ? -1 : 1;
        }
    }
    if (partsA.size() != partsB.size())
        return partsA.size() < partsB.size() ? -1 : 1;
    return 0;
}

QNetworkAccessManager* UpdateChecker::network()
{
    // Created on first use, so a run that never checks pays nothing for it.
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    return m_network;
}

void UpdateChecker::startAutomaticChecks()
{
    m_automatic = true;
    if (automaticChecks())
        scheduleNext();
}

void UpdateChecker::scheduleNext()
{
    const QDateTime last = lastCheck();
    qint64 delay = StartupDelayMs;
    if (last.isValid())
        delay = std::max<qint64>(delay, CheckIntervalMs - last.msecsTo(QDateTime::currentDateTimeUtc()));
    m_timer->start(std::chrono::milliseconds(std::min(delay, CheckIntervalMs)));
}

void UpdateChecker::checkNow()
{
    check(true);
}

void UpdateChecker::check(bool manual)
{
    if (m_reply) {
        // A check is already running; a manual request just waits for its answer.
        return;
    }
    // Pre-releases are only visible in the full list; /latest skips them (and drafts).
    const QString path = includePrereleases()
                              ? QStringLiteral("/repos/%1/releases?per_page=10").arg(repository())
                              : QStringLiteral("/repos/%1/releases/latest").arg(repository());
    m_reply = network()->get(apiRequest(path));
    QNetworkReply* reply = m_reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, manual] { finishCheck(reply, manual); });
}

UpdateChecker::Release UpdateChecker::parseRelease(const QJsonObject& object) const
{
    Release release;
    release.version = stripPrefix(object.value(u"tag_name").toString());
    release.name = object.value(u"name").toString();
    release.notes = object.value(u"body").toString();
    release.pageUrl = QUrl(object.value(u"html_url").toString());
    release.prerelease = object.value(u"prerelease").toBool();
    release.publishedAt = QDateTime::fromString(object.value(u"published_at").toString(), Qt::ISODate);
    release.downloadUrl = platformAsset(object.value(u"assets").toArray());
    if (release.downloadUrl.isEmpty())
        release.downloadUrl = release.pageUrl;
    return release;
}

void UpdateChecker::finishCheck(QNetworkReply* reply, bool manual)
{
    reply->deleteLater();
    if (m_reply == reply)
        m_reply = nullptr;

    QSettings().setValue(LastCheckKey, QDateTime::currentDateTimeUtc());
    if (m_automatic && automaticChecks())
        scheduleNext();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString error;
    Release newest;
    if (status == 404) {
        // No published release yet: nothing to update to.
    } else if (reply->error() != QNetworkReply::NoError || status != 200) {
        error = describeError(reply);
    } else {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        if (document.isArray()) {
            for (const QJsonValue& value : document.array()) {
                const QJsonObject object = value.toObject();
                if (object.value(u"draft").toBool())
                    continue;
                const Release release = parseRelease(object);
                if (newest.version.isEmpty() || compareVersions(release.version, newest.version) > 0)
                    newest = release;
            }
        } else if (document.isObject()) {
            newest = parseRelease(document.object());
        } else {
            error = QCoreApplication::translate("UpdateChecker", "GitHub returned an unexpected answer.");
        }
    }

    if (error.isEmpty())
        m_latest = newest;
    qInfo().noquote() << "update check:" << (error.isEmpty() ? QStringLiteral("latest is %1, running %2")
                                                                    .arg(newest.version.isEmpty() ? QStringLiteral("(none)") : newest.version,
                                                                         currentVersion())
                                                              : QStringLiteral("failed (HTTP %1)").arg(status));

    emit checkFinished(error);
    // Automatic checks speak up once per version and run, and never for a version the user skipped.
    if (!manual && error.isEmpty() && updateAvailable() && m_latest.version != skippedVersion()
        && m_latest.version != m_notifiedVersion) {
        m_notifiedVersion = m_latest.version;
        emit updateAvailableNotice(m_latest);
    }
}

bool UpdateChecker::updateAvailable() const
{
    return !m_latest.version.isEmpty() && compareVersions(m_latest.version, currentVersion()) > 0;
}

void UpdateChecker::fetchContributors()
{
    if (m_contributorsLoaded || m_contributorsReply)
        return;
    m_contributorsReply = network()->get(apiRequest(QStringLiteral("/repos/%1/contributors?per_page=100").arg(repository())));
    QNetworkReply* reply = m_contributorsReply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_contributorsReply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        if (reply->error() != QNetworkReply::NoError || status != 200 || !document.isArray()) {
            emit contributorsLoaded(false);
            return;
        }
        m_contributors.clear();
        for (const QJsonValue& value : document.array()) {
            const QJsonObject object = value.toObject();
            // Bots (dependabot, github-actions...) are not people to credit.
            if (object.value(u"type").toString() != u"User")
                continue;
            m_contributors.append({object.value(u"login").toString(), QUrl(object.value(u"html_url").toString()),
                                   object.value(u"contributions").toInt()});
        }
        m_contributorsLoaded = true;
        emit contributorsLoaded(true);
    });
}
