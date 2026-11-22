#include "DisplayIdleInhibitor.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace {
constexpr auto serviceName = "org.freedesktop.ScreenSaver";
constexpr auto objectPath = "/org/freedesktop/ScreenSaver";

QDBusMessage method(const QString& destination, const QString& name) {
    return QDBusMessage::createMethodCall(destination, objectPath, serviceName, name);
}
}

DisplayIdleInhibitor::DisplayIdleInhibitor()
    : service_(serviceName, QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForOwnerChange) {
    connect(&service_, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString&, const QString& oldOwner, const QString& newOwner) {
        ++ownerGeneration_;
        latestOwner_ = newOwner;
        if (!oldOwner.isEmpty() && oldOwner == owner_) {
            cookie_.reset();  // Tokens belong to the service instance that issued them.
            owner_.clear();
        }
        if (active_ && !newOwner.isEmpty()) acquire();
    });
}

DisplayIdleInhibitor::~DisplayIdleInhibitor() { release(); }

void DisplayIdleInhibitor::setActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    if (active) acquire();
    else release();
}

void DisplayIdleInhibitor::acquire() {
    if (cookie_ || pending_) return;
    pending_ = true;
    auto call = method(serviceName, "Inhibit");
    call << QStringLiteral("VeyAmux") << QStringLiteral("Video playback");
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, generation = ownerGeneration_](QDBusPendingCallWatcher* done) {
        const QDBusPendingReply<uint> reply = *done;
        done->deleteLater();
        pending_ = false;
        if (reply.isError()) {
            if (active_ && ownerGeneration_ != generation && !latestOwner_.isEmpty()) acquire();
            return;
        }
        owner_ = reply.reply().service();
        cookie_ = reply.value();
        // The old service can reply just after its well-known name moved to a new process.
        if (ownerGeneration_ != generation && owner_ != latestOwner_) {
            release();
            if (active_ && !latestOwner_.isEmpty()) acquire();
        } else if (!active_) {
            release();  // Playback stopped while the request was pending.
        }
    });
}

void DisplayIdleInhibitor::release() {
    if (!cookie_) return;
    auto call = method(owner_, "UnInhibit");
    call << *cookie_;
    QDBusConnection::sessionBus().send(call);
    cookie_.reset();
    owner_.clear();
}
