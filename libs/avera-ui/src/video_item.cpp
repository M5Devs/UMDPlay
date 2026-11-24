// SPDX-License-Identifier: GPL-2.0-only

#include "avera_ui/video_item.h"

#include <QDateTime>
#include <QDir>
#include <algorithm>
#include <cmath>
#include <QMutexLocker>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QStandardPaths>

namespace avera {

namespace {

inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/// Picture tweaks in fixed point so the neutral setting is bit-exact: with
/// contrast/saturation at 256 (1.0x) and brightness at 0 the arithmetic below
/// reduces to the plain BT.601 conversion.
struct Effects {
    int contrast = 256;    ///< Q8, 1.0 = 256
    int brightness = 0;    ///< luma levels, -128..128
    int saturation = 256;  ///< Q8, 1.0 = 256

    bool neutral() const { return contrast == 256 && brightness == 0 && saturation == 256; }
};

/// BT.601 limited-range YUV -> packed RGB, one pass per plane row, with the
/// optional brightness/contrast/saturation tweaks folded in.
void yuv_to_rgb32(const uint8_t* y, int ys, const uint8_t* u, int us,
                  const uint8_t* v, int vs, int w, int h, QImage& out,
                  const Effects& fx) {
    const int cq = fx.contrast;
    const int bq = fx.brightness;
    const int sq = fx.saturation;
    for (int row = 0; row < h; ++row) {
        const uint8_t* yr = y + static_cast<size_t>(row) * ys;
        const uint8_t* ur = u + static_cast<size_t>(row / 2) * us;
        const uint8_t* vr = v + static_cast<size_t>(row / 2) * vs;
        auto* dst = reinterpret_cast<QRgb*>(out.scanLine(row));
        for (int col = 0; col < w; ++col) {
            // Luma: scale around mid-grey, then offset.
            int c = ((int(yr[col]) - 16) * cq + 128) >> 8;
            c += bq;
            // Chroma: scale around neutral 128.
            const int d = ((int(ur[col / 2]) - 128) * sq + 128) >> 8;
            const int e = ((int(vr[col / 2]) - 128) * sq + 128) >> 8;
            const int r = clamp8((298 * c + 409 * e + 128) >> 8);
            const int g = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
            const int b = clamp8((298 * c + 516 * d + 128) >> 8);
            dst[col] = qRgb(r, g, b);
        }
    }
}

Effects effects_of(qreal brightness, qreal contrast, qreal saturation) {
    Effects fx;
    fx.contrast = int(std::lround(std::clamp<qreal>(contrast, 0.0, 2.0) * 256.0));
    fx.brightness = int(std::lround(std::clamp<qreal>(brightness, -1.0, 1.0) * 80.0));
    fx.saturation = int(std::lround(std::clamp<qreal>(saturation, 0.0, 2.0) * 256.0));
    return fx;
}

}  // namespace

AveraVideoItem::AveraVideoItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setSmooth(true);
    setAntialiasing(false);
}

AveraVideoItem::~AveraVideoItem() = default;

void AveraVideoItem::setFrame(const uint8_t* y, int yStride, const uint8_t* u,
                              int uStride, const uint8_t* v, int vStride, int width,
                              int height, bool interlaced, qreal pixelAspect) {
    if (!y || !u || !v || width <= 0 || height <= 0) return;

    bool sizeChanged = false;
    {
        QMutexLocker lock(&m_mutex);
        if (m_image.width() != width || m_image.height() != height) {
            m_image = QImage(width, height, QImage::Format_RGB32);
            sizeChanged = true;
        }
        yuv_to_rgb32(y, yStride, u, uStride, v, vStride, width, height, m_image,
                     effects_of(m_brightness, m_contrast, m_saturation));
        m_interlaced = interlaced;
        m_par = pixelAspect > 0 ? pixelAspect : 1.0;
        m_w = width;
        m_h = height;
        m_dirty = true;
    }

    if (sizeChanged) emit sourceSizeChanged();
    emit hasFrameChanged();
    QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
}

void AveraVideoItem::clear() {
    {
        QMutexLocker lock(&m_mutex);
        if (m_w == 0 && m_h == 0 && m_image.isNull()) return;
        m_image = QImage();
        m_w = m_h = 0;
        m_dirty = true;
    }
    emit hasFrameChanged();
    emit sourceSizeChanged();
    QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
}

bool AveraVideoItem::hasFrame() const {
    QMutexLocker lock(&m_mutex);
    return m_w > 0 && m_h > 0;
}

int AveraVideoItem::sourceWidth() const {
    QMutexLocker lock(&m_mutex);
    return m_w;
}

int AveraVideoItem::sourceHeight() const {
    QMutexLocker lock(&m_mutex);
    return m_h;
}

qreal AveraVideoItem::pixelAspect() const {
    QMutexLocker lock(&m_mutex);
    return m_par;
}

void AveraVideoItem::setBrightness(qreal v) {
    v = std::clamp<qreal>(v, -1.0, 1.0);
    if (qFuzzyCompare(v + 1.0, m_brightness + 1.0)) return;
    m_brightness = v;
    emit effectsChanged();
    QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
}

void AveraVideoItem::setContrast(qreal v) {
    v = std::clamp<qreal>(v, 0.0, 2.0);
    if (qFuzzyCompare(v + 1.0, m_contrast + 1.0)) return;
    m_contrast = v;
    emit effectsChanged();
    QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
}

void AveraVideoItem::setSaturation(qreal v) {
    v = std::clamp<qreal>(v, 0.0, 2.0);
    if (qFuzzyCompare(v + 1.0, m_saturation + 1.0)) return;
    m_saturation = v;
    emit effectsChanged();
    QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
}

void AveraVideoItem::resetEffects() {
    const bool changed = m_brightness != 0.0 || m_contrast != 1.0 || m_saturation != 1.0;
    m_brightness = 0.0;
    m_contrast = 1.0;
    m_saturation = 1.0;
    if (changed) emit effectsChanged();
}

bool AveraVideoItem::effectsActive() const {
    return m_brightness != 0.0 || m_contrast != 1.0 || m_saturation != 1.0;
}

QString AveraVideoItem::takeSnapshot(const QString& directory) {
    QImage frame;
    {
        QMutexLocker lock(&m_mutex);
        if (m_image.isNull()) return QString();
        frame = m_image;
        // Save square pixels at the picture's true shape: a 720x480 UMD frame
        // is authored 4:3 (PAR 8/9), and a raw grab would look 33% too wide in
        // any viewer. Scaling the width keeps the full vertical detail.
        const int display_w = int(std::lround(double(frame.width()) * double(m_par)));
        if (display_w > 0 && display_w != frame.width()) {
            // Scale the width only: the stored pixels become square, so the
            // file *is* 4:3 in every viewer instead of looking 3:2.
            frame = frame.scaled(display_w, frame.height(), Qt::IgnoreAspectRatio,
                                 Qt::SmoothTransformation);
        }
    }

    QString dir = directory;
    if (dir.isEmpty()) {
        const QString pictures =
            QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        dir = (pictures.isEmpty() ? QDir::homePath() : pictures) + QStringLiteral("/snapshots");
    }
    if (!QDir().mkpath(dir)) return QString();

    const QString name = QStringLiteral("umdplay-%1.png")
                             .arg(QDateTime::currentDateTime().toString(
                                 QStringLiteral("yyyyMMdd-HHmmss-zzz")));
    const QString path = dir + QLatin1Char('/') + name;
    return frame.save(path, "PNG") ? path : QString();
}

bool AveraVideoItem::interlaced() const {
    QMutexLocker lock(&m_mutex);
    return m_interlaced;
}

QSGNode* AveraVideoItem::updatePaintNode(QSGNode* node, UpdatePaintNodeData*) {
    auto* tn = static_cast<QSGSimpleTextureNode*>(node);
    if (!tn) {
        tn = new QSGSimpleTextureNode();
        tn->setFiltering(QSGTexture::Linear);
        tn->setOwnsTexture(true);
    }

    bool haveNewFrame = false;
    {
        QMutexLocker lock(&m_mutex);
        if (m_dirty) {
            m_pending = m_image;
            m_dirty = false;
            haveNewFrame = true;
        }
    }

    if (haveNewFrame) {
        if (m_pending.isNull()) {
            tn->setTexture(nullptr);
        } else if (window()) {
            tn->setTexture(window()->createTextureFromImage(m_pending));
        }
    }
    tn->setRect(boundingRect());
    return tn;
}

}  // namespace avera
