#include "UpdateDialog.h"

#include <QApplication>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {

QPushButton* button(const QString& text, const char* objectName)
{
    auto* result = new QPushButton(text);
    result->setObjectName(QLatin1String(objectName));
    result->setCursor(Qt::PointingHandCursor);
    return result;
}

} // namespace

UpdateDialog::UpdateDialog(const UpdateChecker::Release& release, QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("profileEditor"));
    setAttribute(Qt::WA_StyledBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Update Available"));
    resize(480, 420);

    auto* title = new QLabel(tr("Snapcord %1 is available").arg(release.version));
    title->setObjectName(QStringLiteral("dialogTitle"));
    title->setWordWrap(true);
    auto* subtitle = new QLabel(tr("You are using version %1. Download the new version and install it over this one; "
                                   "your login and settings are kept.")
                                    .arg(UpdateChecker::currentVersion()));
    subtitle->setObjectName(QStringLiteral("settingsHint"));
    subtitle->setWordWrap(true);

    auto* notes = new QTextBrowser;
    notes->setOpenExternalLinks(true);
    if (release.notes.trimmed().isEmpty())
        notes->setPlainText(tr("This release has no notes."));
    else
        notes->setMarkdown(release.notes);

    auto* skip = button(tr("Skip This Version"), "linkButton");
    auto* later = button(tr("Later"), "secondaryButton");
    auto* download = button(tr("Download"), "brandButton");
    download->setDefault(true);
    connect(skip, &QPushButton::clicked, this, [this, version = release.version] {
        UpdateChecker::setSkippedVersion(version);
        reject();
    });
    connect(later, &QPushButton::clicked, this, &QDialog::reject);
    connect(download, &QPushButton::clicked, this, [this, url = release.downloadUrl] {
        QDesktopServices::openUrl(url);
        accept();
    });

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(skip);
    buttons->addStretch();
    buttons->addWidget(later);
    buttons->addWidget(download);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(10);
    layout->addWidget(title);
    layout->addWidget(subtitle);
    layout->addWidget(notes, 1);
    layout->addLayout(buttons);
}

void UpdateDialog::showNotice(const UpdateChecker::Release& release)
{
    // One notice at a time, even if a later check finds yet another version.
    static QPointer<UpdateDialog> open;
    if (open)
        open->close();
    QWidget* parent = QApplication::activeWindow();
    if (!parent) {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->isVisible() && widget->isWindow() && !qobject_cast<QDialog*>(widget)) {
                parent = widget;
                break;
            }
        }
    }
    open = new UpdateDialog(release, parent);
    open->show();
}
