#pragma once

#include "core/UpdateChecker.h"

#include <QDialog>

// "A new version is available" notice: release notes, plus Download / Skip This Version / Later.
// Download opens the package (or the release page) in the browser; nothing is installed by the app.
class UpdateDialog : public QDialog
{
    Q_OBJECT

public:
    explicit UpdateDialog(const UpdateChecker::Release& release, QWidget* parent = nullptr);

    // Shows the notice for an automatic check, on top of whichever Snapcord window is open.
    static void showNotice(const UpdateChecker::Release& release);
};
