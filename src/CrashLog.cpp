#include "CrashLog.h"
#include "Library.h" // paths

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSysInfo>

#include <atomic>
#include <cstring>
#include <ctime>
#include <mutex>

#ifdef Q_OS_WIN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <csignal>
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#define RUCHE_HAVE_BACKTRACE 1
#endif
#endif

namespace crashlog {
namespace {

// Everything the handler needs is prepared up front: at crash time, allocating or taking
// locks can deadlock, so it only formats into fixed buffers and calls write().
char g_dir[1024];
char g_header[1024];
constexpr size_t kRing = 16 * 1024;
char g_ring[kRing];
std::atomic<size_t> g_ringPos{0}; // total bytes ever written; position = % kRing
std::mutex g_ringLock;
std::atomic<bool> g_crashing{false};

void put(int fd, const char *s) { (void)!::write(fd, s, unsigned(strlen(s))); }

void putNum(int fd, unsigned long long v, int base = 10, int width = 0)
{
    char buf[32];
    int n = 0;
    do {
        const int d = int(v % unsigned(base));
        buf[n++] = char(d < 10 ? '0' + d : 'a' + d - 10);
        v /= unsigned(base);
    } while (v && n < 31);
    while (n < width && n < 31)
        buf[n++] = '0';
    char out[32];
    for (int i = 0; i < n; ++i)
        out[i] = buf[n - 1 - i];
    out[n] = 0;
    put(fd, out);
}

// crash-YYYYMMDD-HHMMSS.txt, from the UTC clock
int openLog()
{
    const time_t now = time(nullptr);
    struct tm t;
#ifdef Q_OS_WIN
    gmtime_s(&t, &now);
#else
    gmtime_r(&now, &t);
#endif
    char path[1200];
    char *p = path;
    auto add = [&p, &path](const char *s) {
        while (*s && p < path + sizeof(path) - 1)
            *p++ = *s++;
    };
    auto addNum = [&add](int v, int width) {
        char b[8];
        for (int i = width - 1; i >= 0; --i) {
            b[i] = char('0' + v % 10);
            v /= 10;
        }
        b[width] = 0;
        add(b);
    };
    add(g_dir);
    add("/crash-");
    addNum(t.tm_year + 1900, 4);
    addNum(t.tm_mon + 1, 2);
    addNum(t.tm_mday, 2);
    add("-");
    addNum(t.tm_hour, 2);
    addNum(t.tm_min, 2);
    addNum(t.tm_sec, 2);
    add(".txt");
    *p = 0;
#ifdef Q_OS_WIN
    return _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
#else
    return open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
#endif
}

void writeRing(int fd)
{
    put(fd, "\n---- last messages ----\n");
    const size_t total = g_ringPos.load();
    if (total == 0) {
        put(fd, "(none)\n");
        return;
    }
    if (total > kRing) { // wrapped: oldest part starts at the write position
        const size_t at = total % kRing;
        (void)!::write(fd, g_ring + at, unsigned(kRing - at));
        (void)!::write(fd, g_ring, unsigned(at));
    } else {
        (void)!::write(fd, g_ring, unsigned(total));
    }
    put(fd, "\n");
}

#ifndef Q_OS_WIN
const char *signalName(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (invalid memory access)";
    case SIGBUS: return "SIGBUS (bus error)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGFPE: return "SIGFPE (arithmetic error)";
    case SIGABRT: return "SIGABRT (aborted)";
    }
    return "signal";
}

void onSignal(int sig, siginfo_t *info, void *)
{
    if (!g_crashing.exchange(true)) {
        const int fd = openLog();
        if (fd >= 0) {
            put(fd, g_header);
            put(fd, "Crash: ");
            put(fd, signalName(sig));
            put(fd, " at address 0x");
            putNum(fd, (unsigned long long)(uintptr_t)(info ? info->si_addr : nullptr), 16);
            put(fd, "\n\n---- stack ----\n");
#ifdef RUCHE_HAVE_BACKTRACE
            void *frames[64];
            const int n = backtrace(frames, 64);
            backtrace_symbols_fd(frames, n, fd);
#else
            put(fd, "(not available on this system)\n");
#endif
            writeRing(fd);
            close(fd);
        }
    }
    // Let the system finish the crash as usual (core dump, crash reporter…)
    signal(sig, SIG_DFL);
    raise(sig);
}
#else
void writeWinStack(int fd, void *const *frames, int n)
{
    for (int i = 0; i < n; ++i) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &mod) &&
            mod) {
            GetModuleFileNameA(mod, name, MAX_PATH);
            const char *base = strrchr(name, '\\');
            put(fd, base ? base + 1 : name);
            put(fd, "+0x");
            putNum(fd, (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)mod), 16);
        } else {
            put(fd, "0x");
            putNum(fd, (unsigned long long)(uintptr_t)frames[i], 16);
        }
        put(fd, "\n");
    }
}

void writeWinLog(const char *what, unsigned long code, void *address, CONTEXT *ctx)
{
    if (g_crashing.exchange(true))
        return;
    const int fd = openLog();
    if (fd < 0)
        return;
    put(fd, g_header);
    put(fd, "Crash: ");
    put(fd, what);
    if (code) {
        put(fd, " 0x");
        putNum(fd, code, 16, 8);
    }
    put(fd, " at address 0x");
    putNum(fd, (unsigned long long)(uintptr_t)address, 16);
    put(fd, "\n\n---- stack (module+offset) ----\n");
    void *frames[64];
    int n = 0;
    if (ctx) {
        // Walk from the faulting frame, not from inside this handler
#if defined(_M_X64) || defined(__x86_64__)
        frames[n++] = reinterpret_cast<void *>(ctx->Rip);
#endif
    }
    n += CaptureStackBackTrace(0, 62, frames + n, nullptr);
    writeWinStack(fd, frames, n);
    writeRing(fd);
    _close(fd);
}

LONG WINAPI onException(EXCEPTION_POINTERS *e)
{
    const EXCEPTION_RECORD *r = e ? e->ExceptionRecord : nullptr;
    const char *what = "exception";
    if (r)
        switch (r->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION: what = "access violation"; break;
        case EXCEPTION_STACK_OVERFLOW: what = "stack overflow"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: what = "illegal instruction"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: what = "division by zero"; break;
        }
    writeWinLog(what, r ? r->ExceptionCode : 0, r ? r->ExceptionAddress : nullptr, e ? e->ContextRecord : nullptr);
    return EXCEPTION_CONTINUE_SEARCH; // Windows' own error reporting still runs
}

void onAbort(int)
{
    writeWinLog("abort", 0, nullptr, nullptr);
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}
#endif

void copyTo(char *dst, size_t size, const QByteArray &src)
{
    const size_t n = qMin(size - 1, size_t(src.size()));
    memcpy(dst, src.constData(), n);
    dst[n] = 0;
}

} // namespace

QString dir() { return paths::dataDir() + QStringLiteral("/crashes"); }

void remember(const QString &line)
{
    const QByteArray b = line.toUtf8().left(int(kRing / 4)) + '\n';
    std::lock_guard<std::mutex> lock(g_ringLock);
    size_t pos = g_ringPos.load();
    for (char c : b) {
        g_ring[pos % kRing] = c;
        ++pos;
    }
    g_ringPos.store(pos);
}

void install()
{
    QDir().mkpath(dir());
    copyTo(g_dir, sizeof(g_dir), QFile::encodeName(QDir::toNativeSeparators(dir())).replace('\\', '/'));
    const QByteArray header = QStringLiteral("Ruche %1 crash log\nStarted: %2\nSystem: %3 (%4), Qt %5\n\n")
                                  .arg(QStringLiteral(RUCHE_VERSION),
                                       QDateTime::currentDateTime().toString(Qt::ISODate),
                                       QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(),
                                       QString::fromLatin1(qVersion()))
                                  .toUtf8();
    copyTo(g_header, sizeof(g_header), header);
#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(onException);
    signal(SIGABRT, onAbort); // abort(), std::terminate, failed asserts
#else
#ifdef RUCHE_HAVE_BACKTRACE
    { // load libgcc's unwinder now; doing it inside the handler can allocate
        void *dummy[2];
        backtrace(dummy, 2);
    }
#endif
    // a stack of its own, so a stack overflow can still be reported
    static char altStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = onSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        sigaction(sig, &sa, nullptr);
#endif
}

QStringList logs()
{
    QStringList out;
    const auto list = QDir(dir()).entryInfoList({QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Name | QDir::Reversed);
    for (const QFileInfo &fi : list)
        out << fi.absoluteFilePath();
    return out;
}

qint64 totalSize()
{
    qint64 n = 0;
    for (const QString &f : logs())
        n += QFileInfo(f).size();
    return n;
}

void clear()
{
    for (const QString &f : logs())
        QFile::remove(f);
}

void prune()
{
    const QStringList all = logs(); // newest first
    qint64 kept = 0;
    for (int i = 0; i < all.size(); ++i) {
        const qint64 size = QFileInfo(all[i]).size();
        if (i >= 20 || kept + size > 5 * 1024 * 1024)
            QFile::remove(all[i]);
        else
            kept += size;
    }
}

QStringList unseen()
{
    QSettings s(paths::configFile(), QSettings::IniFormat);
    const QString last = s.value(QStringLiteral("crash/lastSeen")).toString();
    QStringList out;
    for (const QString &f : logs()) {
        const QString name = QFileInfo(f).fileName();
        if (name <= last)
            break;
        out << f;
    }
    if (!out.isEmpty())
        s.setValue(QStringLiteral("crash/lastSeen"), QFileInfo(out.first()).fileName());
    return out;
}

} // namespace crashlog
