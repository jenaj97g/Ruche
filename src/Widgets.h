#pragma once

#include <QLabel>
#include <QPixmap>
#include <QSlider>
#include <QWidget>

class QToolButton;
class QListWidget;
class QListWidgetItem;

class SeekSlider : public QSlider {
    Q_OBJECT
public:
    explicit SeekSlider(QWidget *parent = nullptr);

Q_SIGNALS:
    // Mouse wheel over the bar: one notch is 5 s, 30 s with Shift, 1 s with Ctrl.
    void wheelSeek(int deltaMs);

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    int valueAt(int x) const;
    int m_wheelAccum = 0;
};

class ElidedLabel : public QLabel {
    Q_OBJECT
public:
    explicit ElidedLabel(QWidget *parent = nullptr);
    void setFullText(const QString &t);
    QString fullText() const { return m_text; }
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void refresh();
    QString m_text;
};

class CoverView : public QWidget {
    Q_OBJECT
public:
    explicit CoverView(int radius = 6, QWidget *parent = nullptr);
    void setImage(const QImage &img);
    void setLogo(const QImage &img); // fitted on a backdrop instead of cropped like album art
    // A small picture in the corner over the cover (the station's logo on a radio song's cover).
    void setBadge(const QImage &img);
    void setPlaceholder(int icon) { m_placeholder = icon; update(); }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w; }
    QSize sizeHint() const override { return {260, 260}; }

Q_SIGNALS:
    void clicked();

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override { Q_EMIT clicked(); }

private:
    QPixmap m_pix;
    QPixmap m_badge;
    QColor m_badgeBackdrop;
    int m_radius;
    int m_placeholder;
    bool m_contain = false;
    QColor m_backdrop;
};

class PlayerBar : public QWidget {
    Q_OBJECT
public:
    explicit PlayerBar(QWidget *parent = nullptr);
    void setTrack(const QString &title, const QString &subtitle);
    void setCover(const QImage &img, int placeholderIcon);
    void setLogo(const QImage &img);
    void setPlaying(bool active, bool paused);
    void setPosition(double pos, double dur, bool seekable, bool live);
    void setShuffle(bool on);
    void setRepeat(int mode); // 0 off, 1 all, 2 one
    void setVolume(int v);
    void setQueueControlsEnabled(bool on); // off for live radio: nothing to skip to
    void refreshTheme() { setPlaying(m_active, m_paused); } // the play glyph is drawn in the theme's colours
    void seekBy(int deltaMs); // relative seek within the current track (wheel, Shift+arrows)

Q_SIGNALS:
    void playPause();
    void next();
    void previous();
    void shuffleToggled(bool on);
    void repeatClicked();
    void seekRequested(double seconds);
    void lookupRequested(bool artist, const QPoint &globalPos); // the title or artist was clicked
    void volumeChanged(int v);
    void coverClicked();

private:
    CoverView *m_cover;
    ElidedLabel *m_title, *m_artist;
    QToolButton *m_play, *m_next, *m_prev, *m_shuffle, *m_repeat, *m_mute;
    SeekSlider *m_seek;
    QSlider *m_volume;
    QLabel *m_pos, *m_dur;
    bool m_dragging = false;
    qint64 m_wheelTarget = -1;   // where a run of wheel seeks is heading
    qint64 m_wheelTime = 0;      // when the last one happened (ms since epoch)
    int m_lastVolume = 80;
    bool m_active = false, m_paused = false;
};

class NowPlayingPanel : public QWidget {
    Q_OBJECT
public:
    explicit NowPlayingPanel(QWidget *parent = nullptr);
    void setInfo(const QString &title, const QString &artist, const QString &album, const QString &tech);
    void setCover(const QImage &img, int placeholderIcon);
    void setLogo(const QImage &img);
    void setCoverWithBadge(const QImage &cover, const QImage &badge); // radio: the song's cover, the station's logo
    void setListTitle(const QString &t);
    QListWidget *list() const { return m_list; }

Q_SIGNALS:
    void itemActivated(int row);
    void lookupRequested(bool artist, const QPoint &globalPos);

private:
    CoverView *m_cover;
    QLabel *m_title, *m_artist, *m_album, *m_tech, *m_listTitle;
    QListWidget *m_list;
};
