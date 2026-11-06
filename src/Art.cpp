#include "Art.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>

#include "TagPath.h"

#include <apefile.h>
#include <apeitem.h>
#include <apetag.h>
#include <attachedpictureframe.h>
#include <flacfile.h>
#include <id3v2tag.h>
#include <mp4file.h>
#include <mpegfile.h>
#include <opusfile.h>
#include <vorbisfile.h>
#include <wavpackfile.h>
#include <xiphcomment.h>

static QByteArray fromTL(const TagLib::ByteVector &v) { return QByteArray(v.data(), int(v.size())); }

static QByteArray pickXiph(const TagLib::List<TagLib::FLAC::Picture *> &pics)
{
    QByteArray any;
    for (auto *p : pics) {
        if (p->type() == TagLib::FLAC::Picture::FrontCover)
            return fromTL(p->data());
        if (any.isEmpty())
            any = fromTL(p->data());
    }
    return any;
}

static QByteArray apeCover(TagLib::APE::Tag *tag)
{
    if (!tag)
        return {};
    const auto &items = tag->itemListMap();
    for (const char *key : {"COVER ART (FRONT)", "COVER ART (OTHER)"}) {
        auto it = items.find(key);
        if (it != items.end() && it->second.type() == TagLib::APE::Item::Binary) {
            const QByteArray raw = fromTL(it->second.binaryData());
            const int nul = raw.indexOf('\0');
            return nul >= 0 ? raw.mid(nul + 1) : raw;
        }
    }
    return {};
}

static QByteArray embeddedCover(const QString &file)
{
    const TagPath enc(file);
    const QString ext = QFileInfo(file).suffix().toLower();
    if (ext == QLatin1String("mp3") || ext == QLatin1String("aac")) {
        TagLib::MPEG::File f(enc, false);
        if (f.isValid() && f.ID3v2Tag()) {
            QByteArray any;
            for (auto *fr : f.ID3v2Tag()->frameList("APIC")) {
                auto *pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame *>(fr);
                if (!pic)
                    continue;
                if (pic->type() == TagLib::ID3v2::AttachedPictureFrame::FrontCover)
                    return fromTL(pic->picture());
                if (any.isEmpty())
                    any = fromTL(pic->picture());
            }
            return any;
        }
    } else if (ext == QLatin1String("flac")) {
        TagLib::FLAC::File f(enc, false);
        if (f.isValid()) {
            QByteArray b = pickXiph(f.pictureList());
            if (b.isEmpty() && f.xiphComment())
                b = pickXiph(f.xiphComment()->pictureList());
            return b;
        }
    } else if (ext == QLatin1String("m4a") || ext == QLatin1String("m4b") || ext == QLatin1String("mp4") ||
               ext == QLatin1String("alac")) {
        TagLib::MP4::File f(enc, false);
        if (f.isValid() && f.tag() && f.tag()->contains("covr")) {
            const auto list = f.tag()->item("covr").toCoverArtList();
            if (!list.isEmpty())
                return fromTL(list.front().data());
        }
    } else if (ext == QLatin1String("ogg") || ext == QLatin1String("oga")) {
        TagLib::Ogg::Vorbis::File f(enc, false);
        if (f.isValid() && f.tag())
            return pickXiph(f.tag()->pictureList());
    } else if (ext == QLatin1String("opus")) {
        TagLib::Ogg::Opus::File f(enc, false);
        if (f.isValid() && f.tag())
            return pickXiph(f.tag()->pictureList());
    } else if (ext == QLatin1String("wv")) {
        TagLib::WavPack::File f(enc, false);
        if (f.isValid())
            return apeCover(f.APETag());
    } else if (ext == QLatin1String("ape")) {
        TagLib::APE::File f(enc, false);
        if (f.isValid())
            return apeCover(f.APETag());
    }
    return {};
}

static QString folderCover(const QString &file)
{
    const QDir dir = QFileInfo(file).absoluteDir();
    static const QStringList names = {QStringLiteral("cover"), QStringLiteral("folder"), QStringLiteral("front"),
                                      QStringLiteral("album"), QStringLiteral("albumart")};
    const QStringList images = dir.entryList({QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png")},
                                             QDir::Files | QDir::Readable);
    for (const QString &want : names)
        for (const QString &img : images)
            if (QFileInfo(img).completeBaseName().compare(want, Qt::CaseInsensitive) == 0)
                return dir.filePath(img);
    if (images.size() == 1)
        return dir.filePath(images.first());
    return {};
}

static QImage decodeScaled(QIODevice *dev, int maxSize)
{
    QImageReader reader(dev);
    reader.setAutoTransform(true);
    QSize s = reader.size();
    if (s.isValid() && (s.width() > maxSize || s.height() > maxSize)) {
        s.scale(maxSize, maxSize, Qt::KeepAspectRatio);
        reader.setScaledSize(s);
    }
    QImage img = reader.read();
    if (!img.isNull() && (img.width() > maxSize || img.height() > maxSize))
        img = img.scaled(maxSize, maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img;
}

QImage loadCover(const QString &audioFile, int maxSize)
{
    QByteArray data = embeddedCover(audioFile);
    if (!data.isEmpty()) {
        QBuffer buf(&data);
        buf.open(QIODevice::ReadOnly);
        QImage img = decodeScaled(&buf, maxSize);
        if (!img.isNull())
            return img;
    }
    const QString cover = folderCover(audioFile);
    if (!cover.isEmpty()) {
        QFile f(cover);
        if (f.open(QIODevice::ReadOnly))
            return decodeScaled(&f, maxSize);
    }
    return {};
}

ArtLoader::ArtLoader(QObject *parent) : QObject(parent)
{
    m_pool.setMaxThreadCount(1);
    m_pool.setExpiryTimeout(3000);
}

ArtLoader::~ArtLoader() { m_pool.waitForDone(); }

int ArtLoader::request(const QString &audioFile, int maxSize)
{
    const int token = ++m_token;
    QPointer<ArtLoader> self(this);
    m_pool.start(QRunnable::create([self, audioFile, maxSize, token, this] {
        if (m_token.load() != token)
            return; // superseded before we started
        const QImage img = loadCover(audioFile, maxSize);
        if (self)
            QMetaObject::invokeMethod(self, [self, token, img] {
                if (self)
                    Q_EMIT self->ready(token, img);
            }, Qt::QueuedConnection);
    }));
    return token;
}
