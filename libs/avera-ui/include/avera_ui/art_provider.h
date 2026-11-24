#pragma once

// SPDX-License-Identifier: GPL-2.0-only

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QString>

namespace avera {

/// Serves cover art straight from memory, so the host never has to write a
/// disc's ICON0.PNG / PIC1.PNG to a temporary file just to show it in QML.
///
///     provider->insert("hero", pngBytes);
///     engine.addImageProvider("discart", provider);
///     // QML: Image { source: "image://discart/hero" }
///
/// Qt takes ownership of the provider; the host keeps the pointer it created.
class ArtProvider : public QQuickImageProvider {
public:
    ArtProvider();

    void insert(const QString& id, const QByteArray& encoded);
    void clear();

    QImage requestImage(const QString& id, QSize* size, const QSize& requested) override;

private:
    mutable QMutex      m_mutex;
    QHash<QString, QByteArray> m_blobs;
};

}  // namespace avera
