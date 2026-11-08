#include "Widgets.h"
#include "Icons.h"
#include "Logo.h"
#include "Models.h"
#include "Theme.h"

#include <QBoxLayout>
#include <functional>
#include <QDateTime>
#include <QToolTip>
#include <QWheelEvent>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QToolButton>

// ---------------------------------------------------------------------------

SeekSlider::SeekSlider(QWidget *parent) : QSlider(Qt::Horizontal, parent)
{
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true); // for the time tooltip under the cursor
}

int SeekSlider::valueAt(int x) const
{
    QStyleOptionSlider opt;
    initStyleOption(&opt);
    const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
    return QStyle::sliderValueFromPosition(minimum(), maximum(), x - handle.width() / 2, width() - handle.width());
}

void SeekSlider::mouseMoveEvent(QMouseEvent *e)
{
    QSlider::mouseMoveEvent(e);
    if (!isEnabled() || maximum() <= 0)
        return;
    const int v = isSliderDown() ? sliderPosition() : valueAt(int(e->position().x()));
    // Show the time where a click would land, just above the bar
    const QPoint at = mapToGlobal(QPoint(int(e->position().x()), 0)) - QPoint(0, 28);
    QToolTip::showText(at, formatDuration(v), this, {}, 2000);
}

void SeekSlider::leaveEvent(QEvent *e)
{
    QToolTip::hideText();
    QSlider::leaveEvent(e);
}

void SeekSlider::wheelEvent(QWheelEvent *e)
{
    e->accept(); // never let the wheel page the slider or scroll a parent
    if (!isEnabled() || maximum() <= 0)
        return;
    // Touchpads send many small deltas; act on every full notch (120)
    const QPoint d = e->angleDelta();
    m_wheelAccum += qAbs(d.y()) >= qAbs(d.x()) ? d.y() : -d.x();
    const int notches = m_wheelAccum / 120;
    if (notches == 0)
        return;
    m_wheelAccum -= notches * 120;
    int step = 5000;
    if (e->modifiers() & Qt::ShiftModifier)
        step = 30000;
    else if (e->modifiers() & Qt::ControlModifier)
        step = 1000;
    Q_EMIT wheelSeek(notches * step);
}

void SeekSlider::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && isEnabled()) {
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
        if (!handle.contains(e->position().toPoint())) {
            // jump straight to the clicked position instead of paging
            const int v = QStyle::sliderValueFromPosition(minimum(), maximum(), int(e->position().x()) - handle.width() / 2,
                                                          width() - handle.width());
            setSliderPosition(v);
            Q_EMIT sliderMoved(v);
            Q_EMIT sliderReleased();
        }
    }
    QSlider::mousePressEvent(e);
}

// ---------------------------------------------------------------------------

ElidedLabel::ElidedLabel(QWidget *parent) : QLabel(parent)
{
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setTextFormat(Qt::PlainText);
}

void ElidedLabel::setFullText(const QString &t)
{
    m_text = t;
    setToolTip(t);
    refresh();
}

QSize ElidedLabel::minimumSizeHint() const { return {40, QLabel::minimumSizeHint().height()}; }

void ElidedLabel::resizeEvent(QResizeEvent *e)
{
    QLabel::resizeEvent(e);
    refresh();
}

void ElidedLabel::refresh() { QLabel::setText(fontMetrics().elidedText(m_text, Qt::ElideRight, qMax(0, width() - 2))); }

// ---------------------------------------------------------------------------

CoverView::CoverView(int radius, QWidget *parent) : QWidget(parent), m_radius(radius), m_placeholder(Icons::Music)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setCursor(Qt::PointingHandCursor);
}

void CoverView::setImage(const QImage &img)
{
    m_contain = false;
    m_pix = img.isNull() ? QPixmap() : QPixmap::fromImage(img);
    m_badge = QPixmap();
    update();
}

void CoverView::setBadge(const QImage &img)
{
    m_badge = img.isNull() ? QPixmap() : QPixmap::fromImage(img);
    m_badgeBackdrop = Logo::backdropFor(img);
    update();
}

void CoverView::setLogo(const QImage &img)
{
    m_contain = true;
    m_badge = QPixmap();
    m_backdrop = Logo::backdropFor(img);
    m_pix = img.isNull() ? QPixmap() : QPixmap::fromImage(img);
    update();
}

void CoverView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const int side = qMin(width(), height());
    const QRectF r((width() - side) / 2.0, (height() - side) / 2.0, side, side);
    QPainterPath clip;
    clip.addRoundedRect(r, m_radius, m_radius);
    p.setClipPath(clip);
    if (m_pix.isNull()) {
        QLinearGradient g(r.topLeft(), r.bottomRight());
        g.setColorAt(0, Theme::mixed(0.08));
        g.setColorAt(1, Theme::mixed(0.03));
        p.fillRect(r, g);
        const qreal s = side * 0.42;
        Icons::paint(&p, QRectF(r.center().x() - s / 2, r.center().y() - s / 2, s, s), Icons::Name(m_placeholder),
                     Theme::mixed(0.19));
        return;
    }
    if (m_contain) {
        p.fillRect(r, m_backdrop);
        const qreal pad = side * 0.1;
        QSizeF ls = m_pix.size() / m_pix.devicePixelRatio();
        ls.scale(r.adjusted(pad, pad, -pad, -pad).size(), Qt::KeepAspectRatio);
        p.drawPixmap(QRectF(r.center().x() - ls.width() / 2, r.center().y() - ls.height() / 2, ls.width(), ls.height()),
                     m_pix, m_pix.rect());
        return;
    }
    // fill the square, cropping a non-square cover from the centre
    QSizeF ps = m_pix.size() / m_pix.devicePixelRatio();
    ps.scale(r.size(), Qt::KeepAspectRatioByExpanding);
    const QRectF target(r.center().x() - ps.width() / 2, r.center().y() - ps.height() / 2, ps.width(), ps.height());
    p.drawPixmap(target, m_pix, m_pix.rect());
    if (!m_badge.isNull() && side >= 120) { // too small to read on the player bar's thumbnail
        const qreal b = side * 0.26, m = side * 0.045;
        const QRectF tile(r.right() - m - b, r.bottom() - m - b, b, b);
        p.setClipping(false);
        QPainterPath round;
        round.addRoundedRect(tile, b * 0.16, b * 0.16);
        p.fillPath(round, m_badgeBackdrop);
        p.setPen(QPen(QColor(0, 0, 0, 70), 1));
        p.drawPath(round);
        const qreal pad = b * 0.12;
        QSizeF ls = m_badge.size() / m_badge.devicePixelRatio();
        ls.scale(tile.adjusted(pad, pad, -pad, -pad).size(), Qt::KeepAspectRatio);
        p.setClipPath(round);
        p.drawPixmap(QRectF(tile.center().x() - ls.width() / 2, tile.center().y() - ls.height() / 2, ls.width(), ls.height()),
                     m_badge, m_badge.rect());
    }
}

// ---------------------------------------------------------------------------

namespace {
// Makes a label act like a link: a left click calls `fn` with where it happened.
class ClickFilter : public QObject {
public:
    ClickFilter(QWidget *w, std::function<void(QPoint)> fn) : QObject(w), m_fn(std::move(fn))
    {
        w->setCursor(Qt::PointingHandCursor);
        w->installEventFilter(this);
    }
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() == Qt::LeftButton && static_cast<QWidget *>(o)->rect().contains(me->position().toPoint())) {
                m_fn(me->globalPosition().toPoint());
                return true;
            }
        }
        return false;
    }

private:
    std::function<void(QPoint)> m_fn;
};
} // namespace

static QToolButton *makeButton(Icons::Name icon, const QString &tip, int size, QWidget *parent)
{
    auto *b = new QToolButton(parent);
    b->setIcon(Icons::icon(icon));
    b->setIconSize(QSize(size, size));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

PlayerBar::PlayerBar(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("playerBar"));
    setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(78);

    m_cover = new CoverView(4, this);
    m_cover->setFixedSize(56, 56);
    connect(m_cover, &CoverView::clicked, this, &PlayerBar::coverClicked);
    m_title = new ElidedLabel(this);
    m_title->setObjectName(QStringLiteral("barTitle"));
    m_artist = new ElidedLabel(this);
    new ClickFilter(m_title, [this](QPoint at) { Q_EMIT lookupRequested(false, at); });
    new ClickFilter(m_artist, [this](QPoint at) { Q_EMIT lookupRequested(true, at); });
    m_artist->setObjectName(QStringLiteral("barArtist"));
    auto *info = new QVBoxLayout;
    info->setSpacing(2);
    info->addStretch();
    info->addWidget(m_title);
    info->addWidget(m_artist);
    info->addStretch();

    auto *left = new QHBoxLayout;
    left->setSpacing(10);
    left->addWidget(m_cover);
    left->addLayout(info, 1);
    auto *leftW = new QWidget(this);
    leftW->setLayout(left);
    leftW->setMinimumWidth(180);
    left->setContentsMargins(0, 0, 0, 0);

    m_shuffle = makeButton(Icons::Shuffle, tr("Shuffle"), 18, this);
    m_shuffle->setCheckable(true);
    m_prev = makeButton(Icons::Prev, tr("Previous (Ctrl+Left)"), 20, this);
    m_play = makeButton(Icons::Play, tr("Play / Pause (Space)"), 20, this);
    m_play->setObjectName(QStringLiteral("playButton"));
    m_play->setFixedSize(38, 38);
    m_next = makeButton(Icons::Next, tr("Next (Ctrl+Right)"), 20, this);
    m_repeat = makeButton(Icons::Repeat, tr("Repeat"), 18, this);
    m_repeat->setCheckable(true);
    connect(m_play, &QToolButton::clicked, this, &PlayerBar::playPause);
    connect(m_next, &QToolButton::clicked, this, &PlayerBar::next);
    connect(m_prev, &QToolButton::clicked, this, &PlayerBar::previous);
    connect(m_shuffle, &QToolButton::toggled, this, &PlayerBar::shuffleToggled);
    connect(m_repeat, &QToolButton::clicked, this, [this] {
        m_repeat->setChecked(!m_repeat->isChecked()); // state comes back through setRepeat()
        Q_EMIT repeatClicked();
    });

    auto *controls = new QHBoxLayout;
    controls->setSpacing(10);
    controls->addStretch();
    controls->addWidget(m_shuffle);
    controls->addWidget(m_prev);
    controls->addWidget(m_play);
    controls->addWidget(m_next);
    controls->addWidget(m_repeat);
    controls->addStretch();

    m_pos = new QLabel(QStringLiteral("0:00"), this);
    m_pos->setObjectName(QStringLiteral("time"));
    m_pos->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_pos->setMinimumWidth(44);
    m_dur = new QLabel(QStringLiteral("0:00"), this);
    m_dur->setObjectName(QStringLiteral("time"));
    m_dur->setMinimumWidth(44);
    m_seek = new SeekSlider(this);
    m_seek->setRange(0, 0);
    connect(m_seek, &QSlider::sliderPressed, this, [this] { m_dragging = true; });
    connect(m_seek, &QSlider::sliderMoved, this, [this](int v) { m_pos->setText(formatDuration(v)); });
    connect(m_seek, &QSlider::sliderReleased, this, [this] {
        m_dragging = false;
        Q_EMIT seekRequested(m_seek->sliderPosition() / 1000.0);
    });
    connect(m_seek, &SeekSlider::wheelSeek, this, &PlayerBar::seekBy);
    auto *seekRow = new QHBoxLayout;
    seekRow->setSpacing(8);
    seekRow->addWidget(m_pos);
    seekRow->addWidget(m_seek, 1);
    seekRow->addWidget(m_dur);

    auto *center = new QVBoxLayout;
    center->setSpacing(2);
    center->addLayout(controls);
    center->addLayout(seekRow);
    auto *centerW = new QWidget(this);
    centerW->setLayout(center);
    center->setContentsMargins(0, 0, 0, 0);
    centerW->setMaximumWidth(640);
    centerW->setMinimumWidth(320);

    m_mute = makeButton(Icons::Volume, tr("Mute"), 18, this);
    m_volume = new QSlider(Qt::Horizontal, this);
    m_volume->setRange(0, 100);
    m_volume->setFixedWidth(110);
    m_volume->setFocusPolicy(Qt::NoFocus);
    connect(m_volume, &QSlider::valueChanged, this, [this](int v) {
        m_mute->setIcon(Icons::icon(v == 0 ? Icons::Mute : Icons::Volume));
        Q_EMIT volumeChanged(v);
    });
    connect(m_mute, &QToolButton::clicked, this, [this] {
        if (m_volume->value() > 0) {
            m_lastVolume = m_volume->value();
            m_volume->setValue(0);
        } else {
            m_volume->setValue(m_lastVolume > 0 ? m_lastVolume : 70);
        }
    });
    auto *right = new QHBoxLayout;
    right->setSpacing(4);
    right->addStretch();
    right->addWidget(m_mute);
    right->addWidget(m_volume);
    auto *rightW = new QWidget(this);
    rightW->setLayout(right);
    right->setContentsMargins(0, 0, 0, 0);
    rightW->setMinimumWidth(160);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(12, 8, 16, 8);
    row->setSpacing(16);
    row->addWidget(leftW, 3);
    row->addWidget(centerW, 4);
    row->addWidget(rightW, 3);

    setTrack(QString(), QString());
    setPlaying(false, false);
}

void PlayerBar::setTrack(const QString &title, const QString &subtitle)
{
    m_title->setFullText(title.isEmpty() ? tr("Nothing playing") : title);
    m_artist->setFullText(subtitle);
}

void PlayerBar::setCover(const QImage &img, int placeholderIcon)
{
    m_cover->setPlaceholder(placeholderIcon);
    m_cover->setImage(img);
}

void PlayerBar::setLogo(const QImage &img)
{
    if (img.isNull())
        setCover(img, Icons::Radio);
    else
        m_cover->setLogo(img);
}

void PlayerBar::setPlaying(bool active, bool paused)
{
    m_active = active;
    m_paused = paused;
    const bool showPause = active && !paused;
    m_play->setIcon(QIcon());
    QIcon ic;
    // dark glyph on the honey-coloured button
    QPixmap pm(QSize(20, 20) * devicePixelRatioF());
    pm.setDevicePixelRatio(devicePixelRatioF());
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    Icons::paint(&p, QRectF(0, 0, 20, 20), showPause ? Icons::Pause : Icons::Play, Theme::onAccent());
    p.end();
    ic.addPixmap(pm);
    m_play->setIcon(ic);
}

void PlayerBar::seekBy(int deltaMs)
{
    if (!m_seek->isEnabled() || m_seek->maximum() <= 0)
        return;
    // Quick notches add up from where the last one was heading, not from a position
    // report that hasn't caught up with the seek yet.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 from = (m_wheelTarget >= 0 && now - m_wheelTime < 600) ? m_wheelTarget : m_seek->value();
    const qint64 to = qBound(qint64(0), from + deltaMs, qint64(m_seek->maximum()));
    m_wheelTarget = to;
    m_wheelTime = now;
    m_seek->setValue(int(to));
    m_pos->setText(formatDuration(to));
    Q_EMIT seekRequested(to / 1000.0);
}

void PlayerBar::setPosition(double pos, double dur, bool seekable, bool live)
{
    if (live) {
        m_seek->setEnabled(false);
        m_seek->setRange(0, 1);
        m_seek->setValue(0);
        m_pos->setText(formatDuration(qint64(pos * 1000)));
        m_dur->setText(tr("LIVE"));
        return;
    }
    m_seek->setEnabled(seekable && dur > 0);
    if (m_dragging || QDateTime::currentMSecsSinceEpoch() - m_wheelTime < 600)
        return; // keep showing where the user is seeking to
    m_seek->setRange(0, qMax(0, int(dur * 1000)));
    m_seek->setValue(int(pos * 1000));
    m_pos->setText(dur > 0 || pos > 0 ? formatDuration(qint64(pos * 1000)) : QStringLiteral("0:00"));
    if (pos < 1)
        m_pos->setText(QStringLiteral("0:00"));
    m_dur->setText(dur > 0 ? formatDuration(qint64(dur * 1000)) : QStringLiteral("0:00"));
}

void PlayerBar::setShuffle(bool on)
{
    QSignalBlocker b(m_shuffle);
    m_shuffle->setChecked(on);
}

void PlayerBar::setRepeat(int mode)
{
    m_repeat->setChecked(mode != 0);
    m_repeat->setIcon(Icons::icon(mode == 2 ? Icons::RepeatOne : Icons::Repeat));
    m_repeat->setToolTip(mode == 0 ? tr("Repeat: off") : mode == 1 ? tr("Repeat: all") : tr("Repeat: one track"));
}

void PlayerBar::setQueueControlsEnabled(bool on)
{
    for (QToolButton *b : {m_prev, m_next, m_shuffle, m_repeat})
        b->setEnabled(on);
}

void PlayerBar::setVolume(int v)
{
    m_volume->setValue(v);
    m_mute->setIcon(Icons::icon(v == 0 ? Icons::Mute : Icons::Volume));
}

// ---------------------------------------------------------------------------

NowPlayingPanel::NowPlayingPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("nowPlaying"));
    setAttribute(Qt::WA_StyledBackground);
    m_cover = new CoverView(8, this);
    m_cover->setMinimumSize(160, 160);
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("npTitle"));
    m_title->setWordWrap(true);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_artist = new QLabel(this);
    new ClickFilter(m_title, [this](QPoint at) { Q_EMIT lookupRequested(false, at); });
    new ClickFilter(m_artist, [this](QPoint at) { Q_EMIT lookupRequested(true, at); });
    m_artist->setObjectName(QStringLiteral("npArtist"));
    m_artist->setWordWrap(true);
    m_album = new QLabel(this);
    m_album->setObjectName(QStringLiteral("npAlbum"));
    m_album->setWordWrap(true);
    m_tech = new QLabel(this);
    m_tech->setObjectName(QStringLiteral("npTech"));
    m_listTitle = new QLabel(this);
    m_listTitle->setObjectName(QStringLiteral("sectionLabel"));
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("upNext"));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setTextElideMode(Qt::ElideRight);
    m_list->setUniformItemSizes(true);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) { Q_EMIT itemActivated(m_list->row(it)); });

    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(14, 14, 14, 10);
    l->setSpacing(4);
    l->addWidget(m_cover);
    l->addSpacing(8);
    l->addWidget(m_title);
    l->addWidget(m_artist);
    l->addWidget(m_album);
    l->addWidget(m_tech);
    l->addSpacing(12);
    l->addWidget(m_listTitle);
    l->addWidget(m_list, 1);
    setInfo(QString(), QString(), QString(), QString());
}

void NowPlayingPanel::setInfo(const QString &title, const QString &artist, const QString &album, const QString &tech)
{
    m_title->setText(title.isEmpty() ? tr("Nothing playing") : title);
    m_artist->setText(artist);
    m_artist->setVisible(!artist.isEmpty());
    m_album->setText(album);
    m_album->setVisible(!album.isEmpty());
    m_tech->setText(tech);
    m_tech->setVisible(!tech.isEmpty());
}

void NowPlayingPanel::setCover(const QImage &img, int placeholderIcon)
{
    m_cover->setPlaceholder(placeholderIcon);
    m_cover->setImage(img);
}

void NowPlayingPanel::setCoverWithBadge(const QImage &cover, const QImage &badge)
{
    m_cover->setPlaceholder(Icons::Radio);
    m_cover->setImage(cover);
    m_cover->setBadge(badge);
}

void NowPlayingPanel::setLogo(const QImage &img)
{
    if (img.isNull())
        setCover(img, Icons::Radio);
    else
        m_cover->setLogo(img);
}

void NowPlayingPanel::setListTitle(const QString &t) { m_listTitle->setText(t.toUpper()); }
