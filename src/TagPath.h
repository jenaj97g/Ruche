// File names handed to TagLib. On Windows TagLib needs UTF-16 to open names outside the
// ANSI code page (Cyrillic, CJK, accents on an English system); elsewhere it takes bytes.
#pragma once

#include <QFile>
#include <QString>
#include <tfile.h>
#include <string>

#ifdef _WIN32
class TagPath {
public:
    explicit TagPath(const QString &path) : m_w(path.toStdWString()) {}
    operator TagLib::FileName() const { return TagLib::FileName(m_w.c_str()); }

private:
    std::wstring m_w;
};
#else
class TagPath {
public:
    explicit TagPath(const QString &path) : m_b(QFile::encodeName(path)) {}
    operator TagLib::FileName() const { return m_b.constData(); }

private:
    QByteArray m_b;
};
#endif
