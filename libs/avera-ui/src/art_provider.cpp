// SPDX-License-Identifier: GPL-2.0-only

#include "avera_ui/art_provider.h"

#include <QMutexLocker>

namespace avera {

ArtProvider::ArtProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

void ArtProvider::insert(const QString& id, const QByteArray& encoded) {
    QMutexLocker lock(&m_mutex);
    m_blobs.insert(id, encoded);
}

void ArtProvider::clear() {
    QMutexLocker lock(&m_mutex);
    m_blobs.clear();
}

QImage ArtProvider::requestImage(const QString& id, QSize* size, const QSize& requested) {
    QByteArray blob;
    {
        QMutexLocker lock(&m_mutex);
        blob = m_blobs.value(id);
    }
    if (blob.isEmpty()) return {};

    QImage image;
    if (!image.loadFromData(blob)) return {};

    if (size) *size = image.size();
    if (requested.isValid() && !requested.isEmpty())
        return image.scaled(requested, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

}  // namespace avera
