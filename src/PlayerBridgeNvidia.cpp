#include <QScreen>
#include "PlayerBridge.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

QString PlayerBridge::nvidiaQualityName(int level) const {
    static const char* levels[] = {"Low", "Medium", "High"};
    return L(levels[qBound(0, level, 2)]);
}

QString PlayerBridge::nvidiaRegulationStatus() const {
    if (!nvidiaAutoRegulate()) return {};
    if (nvidiaQuality() == 0) return L("Low is already the fastest model.");
    if (!liveVulkanActive()) return {};
    const auto m = liveVk_->nvidiaFrameStats().models;
    if (!m.fastStatus.empty()) return translateMessage(QString::fromStdString(m.fastStatus));
    if (!m.fastReady) return {};
    return L("Primary model: %1. Faster model: %2. Pairs processed: %3 primary / %4 faster.")
            .arg(nvidiaQualityName(m.quality), nvidiaQualityName(m.quality - 1)).arg(m.primaryPairs).arg(m.fastPairs);
}

bool PlayerBridge::nvidiaRuntimeInstalled() const {
    if (!qEnvironmentVariableIsEmpty("VEYAMUX_VFX_DIR")) return true;
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QFileInfo::exists(data + "/veyamux-nvidia/vfx-0.2.0.0/ready-v1");
}

void PlayerBridge::installNvidiaRuntime() {
    if (nvidiaInstaller_ || nvidiaRuntimeInstalled()) return;
    const auto script = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../share/veyamux/install-nvidia-vfg.py");
    const auto python = QStandardPaths::findExecutable("python3");
    if (!QFileInfo::exists(script) || python.isEmpty()) {
        nvidiaInstallStatus_ = QStringLiteral("The NVIDIA installer requires Python 3 and the bundled setup script.");
        emit nvidiaInstallChanged();
        return;
    }
    auto* process = new QProcess(this);
    nvidiaInstaller_ = process;
    nvidiaInstallStatus_ = QStringLiteral("Starting NVIDIA runtime download…");
    auto env = QProcessEnvironment::systemEnvironment();
    env.remove("LD_LIBRARY_PATH");
    process->setProcessEnvironment(env);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        while (process->canReadLine()) {
            auto line = QString::fromUtf8(process->readLine()).trimmed();
            if (!line.isEmpty()) nvidiaInstallStatus_ = line;
        }
        emit nvidiaInstallChanged();
    });
    auto release = [this, process] {
        nvidiaInstaller_ = nullptr;
        process->deleteLater();
        emit nvidiaInstallChanged();
    };
    connect(process, &QProcess::errorOccurred, this, [this, process, release](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        nvidiaInstallStatus_ = process->errorString();
        release();
    });
    connect(process, &QProcess::finished, this, [this, release](int code, QProcess::ExitStatus how) {
        if (how == QProcess::CrashExit) nvidiaInstallStatus_ = QStringLiteral("The NVIDIA installer stopped unexpectedly.");
        else if (code == 0) nvidiaInstallStatus_ = QStringLiteral("NVIDIA VFG is ready. Restart the app to load it.");
        release();
    });
    process->start(python, {"-I", "-u", script});
    emit nvidiaInstallChanged();
}
