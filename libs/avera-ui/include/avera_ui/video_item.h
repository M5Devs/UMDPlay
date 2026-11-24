#pragma once

// SPDX-License-Identifier: GPL-2.0-only

#include <QImage>
#include <QMutex>
#include <QQuickItem>
#include <QString>

namespace avera {

/// A video surface for QML.
///
/// The host hands over YUV420P planes (the layout every hardware decoder
/// produces); the item converts to RGB once and uploads a texture. `setFrame`
/// is safe to call from a decode thread — the conversion happens under the
/// item's lock and the repaint is queued onto the GUI thread.
class AveraVideoItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY hasFrameChanged)
    Q_PROPERTY(int sourceWidth READ sourceWidth NOTIFY sourceSizeChanged)
    Q_PROPERTY(int sourceHeight READ sourceHeight NOTIFY sourceSizeChanged)
    Q_PROPERTY(qreal pixelAspect READ pixelAspect NOTIFY sourceSizeChanged)
    Q_PROPERTY(bool interlaced READ interlaced NOTIFY hasFrameChanged)

public:
    explicit AveraVideoItem(QQuickItem* parent = nullptr);
    ~AveraVideoItem() override;

    QSGNode* updatePaintNode(QSGNode* node, UpdatePaintNodeData* data) override;

    /// Copy and convert one YUV420P picture. Thread-safe.
    void setFrame(const uint8_t* y, int yStride, const uint8_t* u, int uStride,
                  const uint8_t* v, int vStride, int width, int height,
                  bool interlaced = false, qreal pixelAspect = 1.0);

    /// Drop the current picture (e.g. when playback stops). Thread-safe.
    void clear();

    bool hasFrame() const;
    int sourceWidth() const;
    int sourceHeight() const;
    qreal pixelAspect() const;
    bool interlaced() const;

signals:
    void hasFrameChanged();
    void sourceSizeChanged();

private:
    mutable QMutex m_mutex;
    QImage   m_image;          ///< RGB32, GUARDED_BY(m_mutex)
    QImage   m_pending;        ///< converted, waiting for the render thread
    bool     m_dirty = false;
    bool     m_interlaced = false;
    qreal    m_par = 1.0;
    int      m_w = 0;
    int      m_h = 0;
};

}  // namespace avera
