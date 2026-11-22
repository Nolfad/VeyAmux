#pragma once

#include <QDBusServiceWatcher>
#include <QString>
#include <optional>

// Hold a session-scoped screen-idle inhibitor while a video source is being watched.
class DisplayIdleInhibitor : public QObject {
public:
    DisplayIdleInhibitor();
    ~DisplayIdleInhibitor() override;
    void setActive(bool active);

private:
    void acquire();
    void release();
    QDBusServiceWatcher service_;
    QString owner_;
    QString latestOwner_;
    std::optional<uint> cookie_;
    bool active_ = false;
    bool pending_ = false;
    unsigned ownerGeneration_ = 0;
};
