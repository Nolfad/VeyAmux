// VeyAmux for Linux: updates from inside the app (no Gear Lever or AppImageUpdate needed).
//
// The newest release is asked from GitHub's API (api.github.com/repos/Nolfad/VeyAmux/releases/latest). When it is
// newer than this version, VeyAmux downloads its VeyAmux-x86_64.AppImage next to the running one, checks it (the
// SHA-256 GitHub records for the release file, or else the SHA-1 in the release's .zsync file), makes it executable
// and puts it in place of the running AppImage under the same name, so a Steam shortcut, Gear Lever entry or desktop
// file keeps working. The running copy goes on until VeyAmux restarts. Only for the AppImage ($APPIMAGE); a build
// from source is updated where it came from.
#pragma once
#include <QString>
#include <functional>

namespace update {

// Fork releases use the AppImage update channel regardless of their version suffix.
inline constexpr bool inAppUpdatesEnabled = false;

struct Release {
    QString tag;          // "VeyAmux-V.1.6"
    QString version;      // "1.6"
    QString notes;        // the release text (Markdown)
    QString page;         // the release's web page
    QString appImageUrl, zsyncUrl;
    QString sha256;       // GitHub's digest of the AppImage, if it has one (lower-case hex)
    qint64 size = 0;
};

// Blocking (network): run it on a worker thread. False with a message when GitHub cannot be asked.
bool latest(Release& out, QString& error);
// a > b for dotted version numbers ("1.10" > "1.9")
bool newer(const QString& a, const QString& b);
// The running AppImage's path, or empty when VeyAmux does not run as an AppImage (or the file cannot be replaced).
QString appImagePath(QString* why = nullptr);
// Blocking: download, check and put the new AppImage in place. progress: 0..100 while downloading.
bool install(const Release& r, const std::function<void(int)>& progress, QString& message);

} // namespace update
