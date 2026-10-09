#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVector>

class QJsonObject;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// Looks for new Snapcord releases on GitHub. It never downloads or installs anything: it only tells the
// interface that a newer version exists and where to get it. Automatic checks run once a day at most.
class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    struct Release
    {
        QString version;   // "0.3.0" (without the leading "v")
        QString name;      // Release title
        QString notes;     // Release notes (Markdown)
        QUrl pageUrl;      // The release page on GitHub
        QUrl downloadUrl;  // Package for this platform, or the release page when none matches
        bool prerelease = false;
        QDateTime publishedAt;
    };

    struct Contributor
    {
        QString login;
        QUrl profileUrl;
        int contributions = 0;
    };

    static UpdateChecker& instance();

    static QString repository() { return QStringLiteral("pedrordgsr/snapcord"); }
    static QUrl repositoryUrl();
    static QString currentVersion();

    // Settings (QSettings "updates/...").
    static bool automaticChecks();
    static void setAutomaticChecks(bool enabled);
    static bool includePrereleases();
    static void setIncludePrereleases(bool enabled);
    static QString skippedVersion();
    static void setSkippedVersion(const QString& version);
    static QDateTime lastCheck();

    // Compares "1.2.3", "v1.2.3" and "1.2.3-beta.1" style versions: <0, 0 or >0. A pre-release sorts
    // before the release with the same number.
    static int compareVersions(const QString& a, const QString& b);

    // Starts the daily automatic checks (when enabled). The first one runs shortly after start-up.
    void startAutomaticChecks();
    // Manual check: always reports through checkFinished, ignoring a skipped version.
    void checkNow();
    bool isChecking() const { return m_reply != nullptr; }
    const Release& latest() const { return m_latest; }
    bool updateAvailable() const;

    // Project contributors from GitHub, fetched once per run.
    void fetchContributors();
    const QVector<Contributor>& contributors() const { return m_contributors; }

signals:
    // Automatic check found a version that is newer and was not skipped.
    void updateAvailableNotice(const UpdateChecker::Release& release);
    // Any check finished. `error` is empty on success; `latest()` then holds the newest release (if any).
    void checkFinished(const QString& error);
    void contributorsLoaded(bool ok);

private:
    explicit UpdateChecker(QObject* parent = nullptr);

    QNetworkAccessManager* network();
    void check(bool manual);
    void finishCheck(QNetworkReply* reply, bool manual);
    void scheduleNext();
    Release parseRelease(const QJsonObject& object) const;

    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_reply = nullptr;
    QNetworkReply* m_contributorsReply = nullptr;
    QTimer* m_timer;
    Release m_latest;
    QVector<Contributor> m_contributors;
    bool m_contributorsLoaded = false;
    bool m_automatic = false;
    QString m_notifiedVersion;
};
