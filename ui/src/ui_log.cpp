#include "ui_log.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QProcessEnvironment>

namespace libs::ui {
namespace {

QMutex g_mutex;
QString g_path;
bool g_resolved = false;

QString resolve_path_locked() {
    const QString from_env = QProcessEnvironment::systemEnvironment().value("LIBS_UI_LOG");
    if (!from_env.isEmpty()) return from_env;
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("ui.log"));
}

}  // namespace

QString ui_log_path() {
    QMutexLocker lock(&g_mutex);
    if (!g_resolved) {
        g_path = resolve_path_locked();
        g_resolved = true;
    }
    return g_path;
}

void ui_log(const char* level, const QString& message) {
    QMutexLocker lock(&g_mutex);
    if (!g_resolved) {
        g_path = resolve_path_locked();
        g_resolved = true;
    }

    QFile file(g_path);
    // 逐行打开/追加/关闭：宁可慢一点，也要保证崩在下一行之前时上一行已经在磁盘上。
    if (!file.open(QIODevice::Append | QIODevice::WriteOnly)) return;

    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QString line = stamp + QLatin1String(" [") + QLatin1String(level) +
                         QLatin1String("] ") + message + QLatin1Char('\n');
    file.write(line.toUtf8());
    file.close();
}

}  // namespace libs::ui
