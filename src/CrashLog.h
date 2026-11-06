// Crash logs: if Ruche crashes, a short text file with what it was doing (a stack trace
// and the last log messages) is left in dataDir()/crashes for the next start to point at.
// Nothing is written while Ruche runs normally, and old logs are pruned so they can
// never pile up.
#pragma once

#include <QString>
#include <QStringList>

namespace crashlog {

// Catches fatal signals (Linux) / unhandled exceptions (Windows). Call once, early.
void install();
// Every Qt log message also goes into a small in-memory ring that a crash log includes.
void remember(const QString &line);

QString dir();
QStringList logs();      // newest first, full paths
qint64 totalSize();
void clear();            // deletes every crash log
// Keeps the newest 20 logs and at most 5 MB of them.
void prune();
// Crash logs written since the last call (newest first); remembered in the settings.
QStringList unseen();

} // namespace crashlog
