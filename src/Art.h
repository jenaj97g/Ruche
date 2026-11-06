// Album art: embedded pictures first, then cover/folder images next to the file.
// Decoded straight to the display size so a 3000px scan never sits in memory at full size.
#pragma once

#include <QImage>
#include <QObject>
#include <QThreadPool>
#include <atomic>

QImage loadCover(const QString &audioFile, int maxSize);

class ArtLoader : public QObject {
    Q_OBJECT
public:
    explicit ArtLoader(QObject *parent = nullptr);
    ~ArtLoader() override;
    // Result arrives through ready(); only the latest token matters to callers.
    int request(const QString &audioFile, int maxSize);

Q_SIGNALS:
    void ready(int token, const QImage &image);

private:
    QThreadPool m_pool;
    std::atomic_int m_token{0};
};
