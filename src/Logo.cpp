#include "Logo.h"
#include "Theme.h"

#include <QBuffer>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>

QImage Logo::decode(const QByteArray &data, int maxSize)
{
    QByteArray copy = data;
    QBuffer buf(&copy);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    const QByteArray fmt = reader.format();
    QImage best;
    if (fmt == "svg" || fmt == "svgz") {
        QSize s = reader.size();
        if (!s.isValid() || s.isEmpty())
            s = QSize(maxSize, maxSize);
        s.scale(maxSize, maxSize, Qt::KeepAspectRatio);
        reader.setScaledSize(s);
        best = reader.read();
    } else {
        // ICO and friends hold several sizes: keep the biggest.
        const int count = qMax(1, reader.imageCount());
        for (int i = 0; i < count && i < 16; ++i) {
            if (i > 0 && !reader.jumpToImage(i))
                break;
            QSize s = reader.size();
            if (s.isValid() && (s.width() > 2 * maxSize || s.height() > 2 * maxSize)) {
                s.scale(2 * maxSize, 2 * maxSize, Qt::KeepAspectRatio);
                reader.setScaledSize(s);
            }
            const QImage img = reader.read();
            if (!img.isNull() && qint64(img.width()) * img.height() > qint64(best.width()) * best.height())
                best = img;
            reader.setScaledSize(QSize());
        }
    }
    return best.isNull() ? best : normalise(best, maxSize);
}

QImage Logo::normalise(const QImage &in, int maxSize)
{
    QImage img = in.convertToFormat(QImage::Format_ARGB32);
    if (img.hasAlphaChannel()) {
        int top = img.height(), bottom = -1, left = img.width(), right = -1;
        for (int y = 0; y < img.height(); ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                if (qAlpha(line[x]) > 8) {
                    top = qMin(top, y);
                    bottom = qMax(bottom, y);
                    left = qMin(left, x);
                    right = qMax(right, x);
                }
            }
        }
        if (bottom >= top && right >= left)
            img = img.copy(left, top, right - left + 1, bottom - top + 1);
    }
    if (img.width() > maxSize || img.height() > maxSize)
        img = img.scaled(maxSize, maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img;
}

// Average brightness of a logo that sits on transparency, or -1 for opaque artwork.
static double transparentLuminance(const QImage &in)
{
    if (in.isNull() || !in.hasAlphaChannel())
        return -1;
    const QImage img = in.width() > 64 || in.height() > 64
                           ? in.scaled(64, 64, Qt::KeepAspectRatio, Qt::FastTransformation).convertToFormat(QImage::Format_ARGB32)
                           : in.convertToFormat(QImage::Format_ARGB32);
    double lum = 0, weight = 0;
    int transparent = 0, total = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x, ++total) {
            const int a = qAlpha(line[x]);
            if (a < 32) {
                ++transparent;
                continue;
            }
            const double w = a / 255.0;
            lum += w * (0.2126 * qRed(line[x]) + 0.7152 * qGreen(line[x]) + 0.0722 * qBlue(line[x])) / 255.0;
            weight += w;
        }
    }
    if (weight <= 0 || transparent < total / 20)
        return -1; // opaque artwork brings its own background
    return lum / weight;
}

bool Logo::wantsLightBackdrop(const QImage &img)
{
    const double l = transparentLuminance(img);
    return l >= 0 && l < 0.38;
}

QColor Logo::backdropFor(const QImage &img)
{
    // Dark artwork on transparency vanishes on a dark theme, white artwork on a light one.
    const double l = transparentLuminance(img);
    if (Theme::isDark())
        return l >= 0 && l < 0.38 ? QColor(0xec, 0xec, 0xef) : Theme::surface();
    return l > 0.7 ? QColor(0x2a, 0x2b, 0x30) : Theme::surface();
}

namespace {
QHash<QString, QPixmap> &tileCache()
{
    static QHash<QString, QPixmap> cache;
    return cache;
}
} // namespace

QPixmap Logo::tile(const QString &file, int size, qreal dpr)
{
    const QString key = QStringLiteral("%1|%2|%3|%4").arg(file).arg(size).arg(dpr).arg(Theme::generation());
    auto &cache = tileCache();
    auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return *it;
    const QPixmap pm = tileFromImage(QImage(file), size, dpr);
    if (cache.size() > 400)
        cache.clear();
    cache.insert(key, pm);
    return pm;
}

QPixmap Logo::tileFromImage(const QImage &logo, int size, qreal dpr)
{
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    if (!logo.isNull()) {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QRectF r(0, 0, size, size);
        QPainterPath clip;
        clip.addRoundedRect(r, size * 0.16, size * 0.16);
        p.fillPath(clip, backdropFor(logo));
        const qreal pad = size * 0.08;
        QSizeF s = logo.size();
        s.scale(r.adjusted(pad, pad, -pad, -pad).size(), Qt::KeepAspectRatio);
        const QRectF target(r.center().x() - s.width() / 2, r.center().y() - s.height() / 2, s.width(), s.height());
        p.setClipPath(clip);
        p.drawImage(target, logo.scaled((s * dpr).toSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    return pm;
}

void Logo::forget(const QString &file)
{
    auto &cache = tileCache();
    for (auto it = cache.begin(); it != cache.end();) {
        if (it.key().startsWith(file + QLatin1Char('|')))
            it = cache.erase(it);
        else
            ++it;
    }
}

bool Logo::isImageFileName(const QString &name)
{
    const QString ext = QFileInfo(name).suffix().toLower();
    return ext == QLatin1String("png") || ext == QLatin1String("jpg") || ext == QLatin1String("jpeg") ||
           ext == QLatin1String("svg") || ext == QLatin1String("svgz") || ext == QLatin1String("webp") ||
           ext == QLatin1String("ico") || ext == QLatin1String("gif") || ext == QLatin1String("bmp");
}
