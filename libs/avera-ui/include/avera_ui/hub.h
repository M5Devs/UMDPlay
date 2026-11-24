#pragma once

// SPDX-License-Identifier: GPL-2.0-only

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

namespace avera {

/// One glossy card in a hub row. `icon` is any image URL QML can load
/// (qrc:/, file://, data:) and may be empty.
struct AveraCard {
    QString     title;
    QString     subtitle;
    QString     icon;
    QString     badge;
    bool        playable = true;
    QVariantMap data;               ///< opaque host payload, echoed on activate
};

/// A vertical category with its horizontal card strip.
struct AveraRow {
    QString          key;           ///< stable id, e.g. "recent"
    QString          title;
    QVector<AveraCard> cards;
};

/// Two-dimensional carousel state for the QML hub.
///
/// Rows run vertically, cards horizontally; the selection is a (row, index)
/// pair that the QML side mirrors with its own animations. The host fills it
/// with `setRows` and reacts to `activated`.
class AveraHub : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(int currentRow READ currentRow NOTIFY selectionChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY selectionChanged)
    Q_PROPERTY(QString currentRowKey READ currentRowKey NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap currentCard READ currentCard NOTIFY selectionChanged)
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY selectionChanged)
    Q_PROPERTY(QString currentSubtitle READ currentSubtitle NOTIFY selectionChanged)
    Q_PROPERTY(bool currentPlayable READ currentPlayable NOTIFY selectionChanged)

public:
    explicit AveraHub(QObject* parent = nullptr);

    void setRows(QVector<AveraRow> rows);
    QVariantList rows() const { return m_rows; }

    int currentRow() const { return m_row; }
    int currentIndex() const { return m_index; }
    QString currentRowKey() const;
    QVariantMap currentCard() const;
    QString currentTitle() const;
    QString currentSubtitle() const;
    bool currentPlayable() const;

    /// Move the selection; returns true when it actually changed.
    Q_INVOKABLE bool move(int dx, int dy);
    Q_INVOKABLE bool setSelection(int row, int index);
    Q_INVOKABLE void activateCurrent();
    Q_INVOKABLE void activate(int row, int index);

signals:
    void rowsChanged();
    void selectionChanged();
    void activated(const QString& rowKey, int index, const QVariantMap& card);

private:
    bool valid() const;
    void clamp();

    QVariantList m_rows;    ///< QVariantMap per row, QVariantList of cards
    int          m_row = 0;
    int          m_index = 0;
};

}  // namespace avera
