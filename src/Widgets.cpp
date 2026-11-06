#include "Widgets.h"
#include "Icons.h"
#include "Logo.h"
#include "Models.h"
#include "Theme.h"

#include <QBoxLayout>
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
    update();
}

void CoverView::setLogo(const QImage &img)
{
    m_contain = true;
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
}

// ---------------------------------------------------------------------------

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
    if (m_dragging)
        return;
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

void NowPlayingPanel::setLogo(const QImage &img)
{
    if (img.isNull())
        setCover(img, Icons::Radio);
    else
        m_cover->setLogo(img);
}

void NowPlayingPanel::setListTitle(const QString &t) { m_listTitle->setText(t.toUpper()); }
