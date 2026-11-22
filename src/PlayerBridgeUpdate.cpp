// VeyAmux for Linux: updates from inside the app (the PlayerBridge part; the download itself is AppUpdate.cpp).
// Checked by itself at most twice a day when "Check for updates" is on; the user decides when to install, and the
// new version starts at the next restart (a button restarts at once).
#include <QScreen>
#include "PlayerBridge.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QSettings>
#include <QUrl>

#include <atomic>
#include <memory>
#include <thread>

QString PlayerBridge::appVersion() const { return QCoreApplication::applicationVersion(); }

bool PlayerBridge::updateSelf() const { return !update::appImagePath().isEmpty(); }

void PlayerBridge::checkForUpdate(bool manual) {
    if (!inAppUpdatesEnabled()) return;
    if (updState_ == "checking" || updState_ == "downloading") return;
    if (!manual) {   // automatic: at most every 12 hours
        const qint64 last = QSettings().value("update/lastCheck", 0).toLongLong();
        if (QDateTime::currentSecsSinceEpoch() - last < 12 * 3600) {
            // a version found earlier is still offered without asking GitHub again
            const QString known = QSettings().value("update/known").toString();
            if (!known.isEmpty() && update::newer(known, appVersion())) {
                updRelease_.version = known;
                updState_ = "available";
                updText_ = L("VeyAmux %1 is available", "VeyAmux %1 er klar").arg(known);
                emit updateChanged();
            }
            return;
        }
    }
    updState_ = "checking";
    updText_ = L("Looking for a new version…", "Leder efter en ny version…");
    emit updateChanged();
    std::thread([this, manual, quit = psQuit_] {
        update::Release r;
        QString err;
        const bool ok = update::latest(r, err);
        if (quit->load()) return;
        QMetaObject::invokeMethod(this, [this, ok, r, err, manual] {
            QSettings().setValue("update/lastCheck", QDateTime::currentSecsSinceEpoch());
            if (!ok) {
                updState_ = manual ? "error" : "";
                updText_ = L("Could not check for updates: ", "Kunne ikke søge efter opdateringer: ") + err;
            } else if (update::newer(r.version, appVersion())) {
                updRelease_ = r;
                QSettings().setValue("update/known", r.version);
                updState_ = "available";
                updText_ = L("VeyAmux %1 is available", "VeyAmux %1 er klar").arg(r.version);
            } else {
                QSettings().remove("update/known");
                updState_ = manual ? "uptodate" : "";
                updText_ = L("VeyAmux %1 is the newest version", "VeyAmux %1 er den nyeste version").arg(appVersion());
            }
            emit updateChanged();
        }, Qt::QueuedConnection);
    }).detach();
}

void PlayerBridge::installUpdate() {
    if (!inAppUpdatesEnabled()) return;
    if (updState_ == "downloading" || updState_ == "ready") return;
    if (!updateSelf()) { openReleasePage(); return; }
    updState_ = "downloading";
    updText_ = L("Downloading VeyAmux %1…", "Henter VeyAmux %1…").arg(updRelease_.version);
    emit updateChanged();
    const update::Release known = updRelease_;
    std::thread([this, known, quit = psQuit_] {
        update::Release r = known;
        QString msg;
        // a version remembered from an earlier check: ask GitHub for its files first
        bool ok = !r.appImageUrl.isEmpty() || update::latest(r, msg);
        auto last = std::make_shared<std::atomic<int>>(-1);
        if (ok) ok = update::install(r, [this, quit, last, v = r.version](int pc) {
            if (quit->load() || pc == last->load()) return;
            last->store(pc);
            QMetaObject::invokeMethod(this, [this, pc, v] {
                if (updState_ != "downloading") return;
                updText_ = L("Downloading VeyAmux %1… %2 %", "Henter VeyAmux %1… %2 %").arg(v).arg(pc);
                emit updateChanged();
            }, Qt::QueuedConnection);
        }, msg);
        if (quit->load()) return;
        QMetaObject::invokeMethod(this, [this, ok, msg, r] {
            qInfo("Update: %s", qPrintable(msg));
            if (ok) {
                updRelease_ = r;
                updState_ = "ready";
                updText_ = L("VeyAmux %1 is installed. It starts the next time, or restart now.",
                             "VeyAmux %1 er installeret. Den starter næste gang, eller genstart nu.").arg(r.version);
                QSettings().remove("update/known");
            } else {
                updState_ = "error";
                updText_ = L("The update failed: ", "Opdateringen mislykkedes: ") + msg;
            }
            emit updateChanged();
        }, Qt::QueuedConnection);
    }).detach();
}

void PlayerBridge::openReleasePage() {
    QDesktopServices::openUrl(QUrl(updRelease_.page.isEmpty() ? QStringLiteral("https://github.com/Nolfad/VeyAmux/releases/latest")
                                                               : updRelease_.page));
}
