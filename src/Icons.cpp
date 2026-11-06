#include "Icons.h"

#include <QFont>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <cmath>

namespace {
QColor g_normal(220, 220, 220), g_active(255, 255, 255), g_disabled(110, 110, 115), g_accent(245, 165, 36);

QPainterPath arrowHead(QPointF tip, QPointF dir, qreal len = 3.2, qreal half = 2.6)
{
    const qreal n = std::hypot(dir.x(), dir.y());
    const QPointF d(dir.x() / n, dir.y() / n), perp(-d.y(), d.x());
    QPainterPath p;
    p.moveTo(tip - d * len + perp * half);
    p.lineTo(tip);
    p.lineTo(tip - d * len - perp * half);
    return p;
}
} // namespace

void Icons::paint(QPainter *p, const QRectF &r, Name n, const QColor &color)
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const qreal s = qMin(r.width(), r.height()) / 24.0;
    p->translate(r.center().x() - 12 * s, r.center().y() - 12 * s);
    p->scale(s, s);
    QPen pen(color, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p->setPen(Qt::NoPen);
    p->setBrush(color);

    auto stroke = [&](const QPainterPath &path, qreal w = 1.9) {
        pen.setWidthF(w);
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        p->drawPath(path);
        p->setPen(Qt::NoPen);
        p->setBrush(color);
    };
    auto line = [&](qreal x1, qreal y1, qreal x2, qreal y2, qreal w = 1.9) {
        QPainterPath path;
        path.moveTo(x1, y1);
        path.lineTo(x2, y2);
        stroke(path, w);
    };

    switch (n) {
    case Play: {
        QPainterPath path;
        path.moveTo(7.5, 4.8);
        path.lineTo(19.5, 12);
        path.lineTo(7.5, 19.2);
        path.closeSubpath();
        p->drawPath(path);
        break;
    }
    case Pause:
        p->drawRoundedRect(QRectF(6, 5, 4.2, 14), 1, 1);
        p->drawRoundedRect(QRectF(13.8, 5, 4.2, 14), 1, 1);
        break;
    case Next:
    case Prev: {
        if (n == Prev) {
            p->translate(24, 0);
            p->scale(-1, 1);
        }
        QPainterPath path;
        path.moveTo(5, 5.5);
        path.lineTo(15, 12);
        path.lineTo(5, 18.5);
        path.closeSubpath();
        p->drawPath(path);
        p->drawRoundedRect(QRectF(16, 5.5, 2.8, 13), 0.8, 0.8);
        break;
    }
    case Stop:
        p->drawRoundedRect(QRectF(6, 6, 12, 12), 1.5, 1.5);
        break;
    case Shuffle: {
        QPainterPath a;
        a.moveTo(3, 7);
        a.lineTo(7, 7);
        a.cubicTo(11.5, 7, 12.5, 17, 17, 17);
        a.lineTo(20.5, 17);
        QPainterPath b;
        b.moveTo(3, 17);
        b.lineTo(7, 17);
        b.cubicTo(11.5, 17, 12.5, 7, 17, 7);
        b.lineTo(20.5, 7);
        stroke(a);
        stroke(b);
        stroke(arrowHead({21, 7}, {1, 0}));
        stroke(arrowHead({21, 17}, {1, 0}));
        break;
    }
    case Repeat:
    case RepeatOne: {
        QPainterPath top;
        top.moveTo(4.5, 12.5);
        top.cubicTo(4.5, 8.5, 6, 7, 9.5, 7);
        top.lineTo(19, 7);
        QPainterPath bottom;
        bottom.moveTo(19.5, 11.5);
        bottom.cubicTo(19.5, 15.5, 18, 17, 14.5, 17);
        bottom.lineTo(5, 17);
        stroke(top);
        stroke(bottom);
        stroke(arrowHead({19.5, 7}, {1, 0}));
        stroke(arrowHead({4.5, 17}, {-1, 0}));
        if (n == RepeatOne) {
            QFont f;
            f.setPixelSize(8);
            f.setBold(true);
            p->setFont(f);
            p->setPen(color);
            p->drawText(QRectF(7, 7.5, 10, 9.5), Qt::AlignCenter, QStringLiteral("1"));
            p->setPen(Qt::NoPen);
        }
        break;
    }
    case Volume:
    case Mute: {
        QPainterPath spk;
        spk.moveTo(3.5, 9);
        spk.lineTo(7.5, 9);
        spk.lineTo(12.5, 4.8);
        spk.lineTo(12.5, 19.2);
        spk.lineTo(7.5, 15);
        spk.lineTo(3.5, 15);
        spk.closeSubpath();
        p->drawPath(spk);
        if (n == Volume) {
            QPainterPath w1;
            w1.arcMoveTo(QRectF(9.5, 8, 8, 8), -50);
            w1.arcTo(QRectF(9.5, 8, 8, 8), -50, 100);
            QPainterPath w2;
            w2.arcMoveTo(QRectF(8, 4.5, 13, 15), -52);
            w2.arcTo(QRectF(8, 4.5, 13, 15), -52, 104);
            stroke(w1);
            stroke(w2);
        } else {
            line(15.5, 9, 21, 15);
            line(21, 9, 15.5, 15);
        }
        break;
    }
    case Search: {
        QPainterPath c;
        c.addEllipse(QPointF(10.5, 10.5), 6, 6);
        stroke(c, 2);
        line(15, 15, 20, 20, 2.4);
        break;
    }
    case Music: {
        line(9.5, 17, 9.5, 6.5, 1.8);
        line(18.5, 15, 18.5, 4.5, 1.8);
        QPainterPath beam;
        beam.moveTo(8.6, 5.5);
        beam.lineTo(19.4, 3.2);
        beam.lineTo(19.4, 6.4);
        beam.lineTo(8.6, 8.7);
        beam.closeSubpath();
        p->drawPath(beam);
        p->drawEllipse(QPointF(7.3, 17.3), 2.9, 2.3);
        p->drawEllipse(QPointF(16.3, 15.3), 2.9, 2.3);
        break;
    }
    case Radio: {
        p->drawEllipse(QPointF(12, 11), 2.2, 2.2);
        for (qreal rad : {5.0, 8.5}) {
            QPainterPath l, r;
            const QRectF box(12 - rad, 11 - rad, 2 * rad, 2 * rad);
            l.arcMoveTo(box, 135);
            l.arcTo(box, 135, 90);
            r.arcMoveTo(box, -45);
            r.arcTo(box, -45, 90);
            stroke(l);
            stroke(r);
        }
        line(12, 13.5, 12, 21, 1.9);
        break;
    }
    case Queue:
        line(4, 6.5, 15, 6.5);
        line(4, 11.5, 15, 11.5);
        line(4, 16.5, 11, 16.5);
        {
            QPainterPath t;
            t.moveTo(14.5, 13.5);
            t.lineTo(20.5, 17);
            t.lineTo(14.5, 20.5);
            t.closeSubpath();
            p->drawPath(t);
        }
        break;
    case Playlist:
        line(4, 6.5, 15, 6.5);
        line(4, 11.5, 15, 11.5);
        line(4, 16.5, 10, 16.5);
        line(18, 9, 18, 18, 1.7);
        line(18, 9, 21, 10, 1.7);
        p->drawEllipse(QPointF(15.8, 18.2), 2.4, 1.9);
        break;
    case Add:
        line(12, 5, 12, 19, 2);
        line(5, 12, 19, 12, 2);
        break;
    case Remove:
        line(5, 12, 19, 12, 2);
        break;
    case Import: {
        QPainterPath tray;
        tray.moveTo(4, 14.5);
        tray.lineTo(4, 19.5);
        tray.lineTo(20, 19.5);
        tray.lineTo(20, 14.5);
        stroke(tray);
        line(12, 3.5, 12, 14);
        stroke(arrowHead({12, 14.5}, {0, 1}, 4, 3.5));
        break;
    }
    case Menu:
        line(4, 6.5, 20, 6.5);
        line(4, 12, 20, 12);
        line(4, 17.5, 20, 17.5);
        break;
    case Settings:
        line(4, 7, 20, 7);
        line(4, 12, 20, 12);
        line(4, 17, 20, 17);
        p->drawEllipse(QPointF(9, 7), 2.4, 2.4);
        p->drawEllipse(QPointF(15.5, 12), 2.4, 2.4);
        p->drawEllipse(QPointF(7, 17), 2.4, 2.4);
        break;
    case Folder: {
        QPainterPath f;
        f.moveTo(3.5, 7);
        f.lineTo(3.5, 18.5);
        f.lineTo(20.5, 18.5);
        f.lineTo(20.5, 8.5);
        f.lineTo(11.5, 8.5);
        f.lineTo(9.5, 6);
        f.lineTo(4.5, 6);
        f.closeSubpath();
        stroke(f, 1.7);
        break;
    }
    case Edit: {
        QPainterPath pen2;
        pen2.moveTo(5, 19);
        pen2.lineTo(6, 15);
        pen2.lineTo(16, 5);
        pen2.lineTo(19, 8);
        pen2.lineTo(9, 18);
        pen2.closeSubpath();
        stroke(pen2, 1.7);
        break;
    }
    case Logo: {
        // honeycomb cell with a wave through it
        QPainterPath hex;
        for (int i = 0; i < 6; ++i) {
            const qreal a = M_PI / 180.0 * (60 * i - 90);
            const QPointF pt(12 + 10 * std::cos(a), 12 + 10 * std::sin(a));
            if (i == 0)
                hex.moveTo(pt);
            else
                hex.lineTo(pt);
        }
        hex.closeSubpath();
        p->drawPath(hex);
        QPainterPath wave;
        wave.moveTo(5.5, 12);
        wave.cubicTo(8, 6.5, 10, 6.5, 12, 12);
        wave.cubicTo(14, 17.5, 16, 17.5, 18.5, 12);
        pen.setColor(QColor(28, 29, 32));
        stroke(wave, 2.2);
        break;
    }
    }
    p->restore();
}

namespace {
class PaintedEngine : public QIconEngine {
public:
    PaintedEngine(Icons::Name n, bool accent) : m_name(n), m_accent(accent) {}
    void paint(QPainter *p, const QRect &r, QIcon::Mode mode, QIcon::State state) override
    {
        QColor c = g_normal;
        if (m_accent || state == QIcon::On)
            c = g_accent;
        if (mode == QIcon::Active && !m_accent && state == QIcon::Off)
            c = g_active;
        if (mode == QIcon::Disabled)
            c = g_disabled;
        Icons::paint(p, r, m_name, c);
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QPixmap pm(size * scale);
        pm.setDevicePixelRatio(scale);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }
    QIconEngine *clone() const override { return new PaintedEngine(m_name, m_accent); }

private:
    Icons::Name m_name;
    bool m_accent;
};
} // namespace

void Icons::setColors(const QColor &normal, const QColor &active, const QColor &disabled, const QColor &accent)
{
    g_normal = normal;
    g_active = active;
    g_disabled = disabled;
    g_accent = accent;
}

QIcon Icons::icon(Name n) { return QIcon(new PaintedEngine(n, false)); }
QIcon Icons::accentIcon(Name n) { return QIcon(new PaintedEngine(n, true)); }
