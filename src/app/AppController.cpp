#include "AppController.h"

#include "LoginWindow.h"
#include "MainWindow.h"
#include "RichPresence.h"
#include "UpdateDialog.h"
#include "VoiceController.h"
#include "core/ClientProperties.h"
#include "core/RestClient.h"
#include "core/Session.h"
#include "core/UpdateChecker.h"
#include "platform/CredentialStore.h"

#include <QGuiApplication>
#include <QJsonObject>
#include <QMessageBox>

AppController::AppController(QObject* parent)
    : QObject(parent)
{
}

AppController::~AppController()
{
    closeSession();
    delete m_login;
}

void AppController::start()
{
    // The window comes up with the saved theme immediately. Discord's build number is fetched in the
    // background; only the gateway waits for it, and only when nothing is cached yet.
    // New releases on GitHub are announced with a notice; the first check runs a little after start-up.
    connect(&UpdateChecker::instance(), &UpdateChecker::updateAvailableNotice, this, &UpdateDialog::showNotice);
    UpdateChecker::instance().startAutomaticChecks();

    const QString token = CredentialStore::loadToken();
    if (token.isEmpty())
        showLogin();
    else
        showMain(token);
}

void AppController::showLogin()
{
    m_login = new LoginWindow;
    m_login->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_login, &LoginWindow::loggedIn, this, [this](const QString& token) {
        CredentialStore::saveToken(token);
        m_login->hide();
        m_login->deleteLater();
        showMain(token);
    });
    m_login->show();
}

void AppController::showMain(const QString& token)
{
    m_session = new Session(this);
    // Lives and dies with the session.
    if constexpr (RichPresence::Enabled)
        new RichPresence(m_session, m_session);
    m_voice = new VoiceController(m_session, this);
    m_main = new MainWindow(m_session, m_voice);
    m_main->setAttribute(Qt::WA_DeleteOnClose);

    connect(m_main, &MainWindow::logoutRequested, this, &AppController::logout);
    connect(m_session, &Session::authenticationFailed, this, [this] {
        // The token was revoked (password change, logout from another device...): back to the login screen.
        CredentialStore::clearToken();
        closeSession();
        showLogin();
        // Non-blocking on purpose: a nested event loop here would delete the session while this very
        // signal is still being delivered.
        auto* box = new QMessageBox(QMessageBox::Information, QStringLiteral("Snapcord"),
                                    tr("Your session has expired. Please log in again."), QMessageBox::Ok, m_login);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->show();
    });

    auto reportActivity = [this] {
        if (!m_session || !m_voice)
            return;
        m_session->setActiveState(QGuiApplication::applicationState() == Qt::ApplicationActive,
                                  m_voice->state() == VoiceConnection::State::Connected);
    };
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, m_session, reportActivity);
    connect(m_voice, &VoiceController::stateChanged, m_session, reportActivity);
    reportActivity();

    m_main->show();
    Session* session = m_session;
    ClientProperties::ensureBuildNumber([this, session, token] {
        if (m_session == session)
            m_session->start(token);
    });
}

void AppController::logout()
{
    // Invalidate the token on Discord's side too, then forget it locally.
    if (m_session) {
        auto* rest = new RestClient;
        rest->setToken(m_session->token());
        rest->post(QStringLiteral("/auth/logout"),
                   QJsonDocument(QJsonObject{{QStringLiteral("provider"), QJsonValue()},
                                             {QStringLiteral("voip_provider"), QJsonValue()}}),
                   [rest](const RestClient::Response&) { rest->deleteLater(); });
    }
    CredentialStore::clearToken();
    closeSession();
    showLogin();
}

void AppController::closeSession()
{
    // Everything is deleted later, in creation order, because this can run from inside a signal
    // emitted by the main window itself.
    if (m_voice)
        m_voice->leave();
    if (m_main) {
        m_main->hide();
        m_main->deleteLater();
    }
    if (m_voice) {
        m_voice->deleteLater();
        m_voice = nullptr;
    }
    if (m_session) {
        m_session->stop();
        m_session->deleteLater();
        m_session = nullptr;
    }
}
