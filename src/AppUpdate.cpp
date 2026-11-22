// VeyAmux for Linux: updates from inside the app (see AppUpdate.h).
#include "AppUpdate.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

#include <curl/curl.h>
#include <cstdio>
#include <unistd.h>

namespace update {
namespace {

constexpr const char* kApi = "https://api.github.com/repos/Nolfad/VeyAmux/releases/latest";
constexpr const char* kAsset = "VeyAmux-x86_64.AppImage";
constexpr qint64 kMaxAppImage = 1024ll * 1024 * 1024;

struct Sink {
    QFile* file = nullptr;      // or memory
    QByteArray* mem = nullptr;
    qint64 bytes = 0, max = 0;
    const std::function<void(int)>* progress = nullptr;
    int last = -1;
};

size_t onData(char* p, size_t size, size_t n, void* user) {
    auto* s = static_cast<Sink*>(user);
    const qint64 len = qint64(size * n);
    s->bytes += len;
    if (s->bytes > s->max) return 0;
    if (s->mem) { s->mem->append(p, int(len)); return size_t(len); }
    return s->file->write(p, len) == len ? size_t(len) : 0;
}

int onProgress(void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    auto* s = static_cast<Sink*>(user);
    if (total > 0 && s->progress && *s->progress) {
        const int pc = int(now * 100 / total);
        if (pc != s->last) { s->last = pc; (*s->progress)(pc); }
    }
    return 0;
}

bool get(const QString& url, Sink& sink, QString& error, bool api) {
    CURL* c = curl_easy_init();
    if (!c) { error = "curl could not start"; return false; }
    char err[CURL_ERROR_SIZE] = {};
    const QByteArray u = url.toUtf8();
    curl_slist* headers = nullptr;
    if (api) headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    curl_easy_setopt(c, CURLOPT_URL, u.constData());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);   // release files redirect to GitHub's file servers
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
    curl_easy_setopt(c, CURLOPT_USERAGENT, "VeyAmux");
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onData);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &sink);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, sink.progress ? 0L : 1L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
    // the AppImage's libcurl looks for the CA file where the build system keeps it; use the system's
    for (const char* ca : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                           "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem"})
        if (access(ca, R_OK) == 0) { curl_easy_setopt(c, CURLOPT_CAINFO, ca); break; }
    const CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    curl_slist_free_all(headers);
    if (rc != CURLE_OK) {
        error = sink.bytes > sink.max ? QStringLiteral("the download is larger than expected")
                                      : QString::fromUtf8(err[0] ? err : curl_easy_strerror(rc));
        return false;
    }
    return true;
}

QString fileHash(const QString& path, QCryptographicHash::Algorithm a) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(a);
    h.addData(&f);
    return QString::fromLatin1(h.result().toHex());
}

} // namespace

bool newer(const QString& a, const QString& b) {
    const QStringList x = a.split('.'), y = b.split('.');
    for (int i = 0; i < std::max(x.size(), y.size()); ++i) {
        const int p = i < x.size() ? x[i].toInt() : 0, q = i < y.size() ? y[i].toInt() : 0;
        if (p != q) return p > q;
    }
    return false;
}

bool latest(Release& out, QString& error) {
    QByteArray body;
    Sink s;
    s.mem = &body;
    s.max = 4 * 1024 * 1024;
    if (!get(kApi, s, error, true)) return false;
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    out.tag = o.value("tag_name").toString();
    // tags are "VeyAmux-V.<version>"
    const auto m = QRegularExpression(QStringLiteral("(\\d+(?:\\.\\d+)*)\\s*$")).match(out.tag);
    out.version = m.hasMatch() ? m.captured(1) : QString();
    out.notes = o.value("body").toString();
    out.page = o.value("html_url").toString();
    for (const QJsonValue& v : o.value("assets").toArray()) {
        const QJsonObject a = v.toObject();
        const QString name = a.value("name").toString();
        if (name == kAsset) {
            out.appImageUrl = a.value("browser_download_url").toString();
            out.size = qint64(a.value("size").toDouble());
            const QString d = a.value("digest").toString();
            if (d.startsWith("sha256:")) out.sha256 = d.mid(7).toLower();
        } else if (name == QString(kAsset) + ".zsync") {
            out.zsyncUrl = a.value("browser_download_url").toString();
        }
    }
    if (out.version.isEmpty()) { error = "GitHub's answer has no release version"; return false; }
    return true;
}

QString appImagePath(QString* why) {
    const QString p = qEnvironmentVariable("APPIMAGE");
    if (p.isEmpty() || !QFileInfo(p).isFile()) {
        if (why) *why = QStringLiteral("not an AppImage");
        return {};
    }
    if (!QFileInfo(QFileInfo(p).absolutePath()).isWritable()) {
        if (why) *why = QStringLiteral("the folder of the AppImage is read-only");
        return {};
    }
    return p;
}

bool install(const Release& r, const std::function<void(int)>& progress, QString& message) {
    QString why;
    const QString target = appImagePath(&why);
    if (target.isEmpty()) { message = "Cannot update here: " + why; return false; }
    if (r.appImageUrl.isEmpty()) { message = "The release has no AppImage"; return false; }
    // what the new file must hash to: GitHub's SHA-256 of the release file, else the SHA-1 in the .zsync file
    QString sha1;
    if (r.sha256.isEmpty()) {
        if (r.zsyncUrl.isEmpty()) { message = "The release has no checksum to check the download against"; return false; }
        QByteArray z;
        Sink s;
        s.mem = &z;
        s.max = 64 * 1024 * 1024;
        if (!get(r.zsyncUrl, s, message, false)) { message = "Could not get the checksum: " + message; return false; }
        const int end = z.indexOf("\n\n");
        for (const QByteArray& line : z.left(end < 0 ? 4096 : end).split('\n'))
            if (line.startsWith("SHA-1: ")) sha1 = QString::fromLatin1(line.mid(7).trimmed()).toLower();
        if (sha1.size() != 40) { message = "The .zsync file has no SHA-1"; return false; }
    }
    const QString part = target + ".download";
    QFile f(part);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { message = "Cannot write " + part; return false; }
    Sink s;
    s.file = &f;
    s.max = kMaxAppImage;
    s.progress = &progress;
    const bool ok = get(r.appImageUrl, s, message, false);
    f.close();
    if (!ok) { QFile::remove(part); message = "Download failed: " + message; return false; }
    const bool match = !r.sha256.isEmpty() ? fileHash(part, QCryptographicHash::Sha256) == r.sha256
                                           : fileHash(part, QCryptographicHash::Sha1) == sha1;
    if (!match) { QFile::remove(part); message = "The download does not match the release's checksum, so it was not installed"; return false; }
    QFile::setPermissions(part, QFile::permissions(target) | QFileDevice::ExeOwner | QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    // rename() replaces the file in one step; the running AppImage keeps its (now unnamed) old file until it exits
    if (::rename(part.toLocal8Bit().constData(), target.toLocal8Bit().constData()) != 0) {
        QFile::remove(part);
        message = "Could not replace " + target;
        return false;
    }
    message = "VeyAmux " + r.version + " is installed";
    return true;
}

} // namespace update
