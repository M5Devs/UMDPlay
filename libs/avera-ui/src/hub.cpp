// SPDX-License-Identifier: GPL-2.0-only

#include "avera_ui/hub.h"

#include <QVariantMap>

namespace avera {

namespace {

QVariantMap card_to_map(const AveraCard& c, int row, int index) {
    QVariantMap m;
    m.insert(QStringLiteral("title"), c.title);
    m.insert(QStringLiteral("subtitle"), c.subtitle);
    m.insert(QStringLiteral("icon"), c.icon);
    m.insert(QStringLiteral("badge"), c.badge);
    m.insert(QStringLiteral("playable"), c.playable);
    m.insert(QStringLiteral("row"), row);
    m.insert(QStringLiteral("index"), index);
    m.insert(QStringLiteral("data"), c.data);
    return m;
}

}  // namespace

AveraHub::AveraHub(QObject* parent) : QObject(parent) {}

void AveraHub::setRows(QVector<AveraRow> rows) {
    m_rows.clear();
    for (int r = 0; r < rows.size(); ++r) {
        QVariantMap row;
        row.insert(QStringLiteral("key"), rows[r].key);
        row.insert(QStringLiteral("title"), rows[r].title);
        QVariantList cards;
        for (int c = 0; c < rows[r].cards.size(); ++c)
            cards.append(card_to_map(rows[r].cards[c], r, c));
        row.insert(QStringLiteral("cards"), cards);
        m_rows.append(row);
    }
    m_row = 0;
    m_index = 0;
    clamp();
    emit rowsChanged();
    emit selectionChanged();
}

bool AveraHub::valid() const {
    return m_row >= 0 && m_row < m_rows.size();
}

void AveraHub::clamp() {
    if (m_rows.isEmpty()) { m_row = m_index = 0; return; }
    m_row = qBound(0, m_row, int(m_rows.size()) - 1);
    const QVariantMap row = m_rows.at(m_row).toMap();
    const QVariantList cards = row.value(QStringLiteral("cards")).toList();
    m_index = cards.isEmpty() ? 0 : qBound(0, m_index, int(cards.size()) - 1);
}

QString AveraHub::currentRowKey() const {
    if (!valid()) return {};
    return m_rows.at(m_row).toMap().value(QStringLiteral("key")).toString();
}

QVariantMap AveraHub::currentCard() const {
    if (!valid()) return {};
    const QVariantList cards =
        m_rows.at(m_row).toMap().value(QStringLiteral("cards")).toList();
    if (m_index < 0 || m_index >= cards.size()) return {};
    return cards.at(m_index).toMap();
}

QString AveraHub::currentTitle() const {
    return currentCard().value(QStringLiteral("title")).toString();
}

QString AveraHub::currentSubtitle() const {
    return currentCard().value(QStringLiteral("subtitle")).toString();
}

bool AveraHub::currentPlayable() const {
    return currentCard().value(QStringLiteral("playable")).toBool();
}

bool AveraHub::move(int dx, int dy) {
    if (m_rows.isEmpty()) return false;
    const int oldRow = m_row;
    const int oldIndex = m_index;

    if (dy != 0) {
        m_row += dy;
        // Wrap around the vertical categories for a continuous carousel feel.
        if (m_row < 0) m_row = int(m_rows.size()) - 1;
        if (m_row >= m_rows.size()) m_row = 0;
        m_index = 0;
    }
    if (dx != 0) {
        const QVariantList cards =
            m_rows.at(m_row).toMap().value(QStringLiteral("cards")).toList();
        if (!cards.isEmpty()) {
            m_index += dx;
            if (m_index < 0) m_index = int(cards.size()) - 1;
            if (m_index >= cards.size()) m_index = 0;
        }
    }
    if (m_row == oldRow && m_index == oldIndex) return false;
    emit selectionChanged();
    return true;
}

bool AveraHub::setSelection(int row, int index) {
    if (row < 0 || row >= m_rows.size()) return false;
    if (row == m_row && index == m_index) return false;
    m_row = row;
    m_index = index;
    clamp();
    emit selectionChanged();
    return true;
}

void AveraHub::activateCurrent() { activate(m_row, m_index); }

void AveraHub::activate(int row, int index) {
    if (row < 0 || row >= m_rows.size()) return;
    const QVariantMap r = m_rows.at(row).toMap();
    const QVariantList cards = r.value(QStringLiteral("cards")).toList();
    if (index < 0 || index >= cards.size()) return;
    m_row = row;
    m_index = index;
    emit selectionChanged();
    emit activated(r.value(QStringLiteral("key")).toString(), index,
                   cards.at(index).toMap());
}

}  // namespace avera
