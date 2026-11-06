// Station logos: decoding, normalising and the small tiles shown in lists.
#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QString>

namespace Logo {
// Decodes any supported image (PNG, JPEG, SVG, WebP, ICO, GIF, BMP). For multi-image
// files (ICO) the largest picture is used; SVG is rendered at `maxSize`.
QImage decode(const QByteArray &data, int maxSize = 256);
// Trims fully transparent margins and scales down to fit `maxSize`.
QImage normalise(const QImage &img, int maxSize = 256);
// Dark artwork on a transparent background would vanish on the dark theme.
bool wantsLightBackdrop(const QImage &img);
QColor backdropFor(const QImage &img);
// Square tile with the logo fitted inside, for list rows and the player bar.
QPixmap tile(const QString &file, int size, qreal dpr);
QPixmap tileFromImage(const QImage &logo, int size, qreal dpr);
void forget(const QString &file); // drop cached tiles after a logo changes
bool isImageFileName(const QString &name);
} // namespace Logo
