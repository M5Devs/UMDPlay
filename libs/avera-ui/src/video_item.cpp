// SPDX-License-Identifier: GPL-2.0-only

#include "avera_ui/video_item.h"

#include <QMutexLocker>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>

namespace avera {

namespace {

inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/// BT.601 limited-range YUV -> packed RGB, one pass per plane row.
void yuv_to_rgb32(const uint8_t* y, int ys, const uint8_t* u, int us,
                  const uint8_t* v, int vs, int w, int h, QImage& out) {
    for (int row = 0; row < h; ++row) {
        const uint8_t* yr = y + static_cast<size_t>(row) * ys;
        const uint8_t* ur = u + static_cast<size_t>(row / 2) * us;
        const uint8_t* vr = v + static_cast<size_t>(row / 2) * vs;
        auto* dst = reinterpret_cast<QRgb*>(out.scanLine(row));
        for (int col = 0; col < w; ++col) {
            const int c = int(yr[col]) - 16;
            const int d = int(ur[col / 2]) - 128;
            const int e = int(vr[col / 2]) - 128;
            const int r = clamp8((298 * c + 409 * e + 128) >> 8);
            const int g = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
            const int b = clamp8((298 * c + 516 * d + 128) >> 8);
            dst[col] = qRgb(r, g, b);
        }
    }
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
        yuv_to_rgb32(y, yStride, u, uStride, v, vStride, width, height, m_image);
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
