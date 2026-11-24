// SPDX-License-Identifier: GPL-2.0-only

#include "avera_ui/hub.h"

#include <algorithm>

namespace avera {

AveraHub::AveraHub(QObject* parent) : QObject(parent) {}

void AveraHub::setNavItems(const QStringList& items) {
    if (m_navItems == items) return;
    m_navItems = items;
    emit infoChanged();
}

void AveraHub::setStatus(const QString& drive, const QString& medium, const QString& size,
                         const QString& state, const QString& codec, const QString& engine) {
    m_drive = drive;
    m_medium = medium;
    m_size = size;
    m_state = state;
    m_codec = codec;
    m_engine = engine;
    emit infoChanged();
}

void AveraHub::setCategories(const QVector<AveraCategory>& categories) {
    m_categories.clear();
    m_categories.reserve(categories.size());
    for (const auto& c : categories) {
        QVariantMap map;
        map.insert(QStringLiteral("title"), c.title);
        map.insert(QStringLiteral("sub"), c.sub);
        m_categories.push_back(map);
    }
    m_category = std::clamp(m_category, 0, std::max(0, int(m_categories.size()) - 1));
    emit infoChanged();
    emit selectionChanged();
}

void AveraHub::setHero(const AveraHero& hero) {
    m_hero = hero;
    emit infoChanged();
}

void AveraHub::setPeek(const AveraPeek& peek) {
    m_peek = peek;
    emit infoChanged();
}

void AveraHub::setChapterPillText(const QString& text) {
    m_chapterPill = text;
    emit infoChanged();
}

void AveraHub::setNowPlaying(const QString& stateText, const QString& detail) {
    m_nowState = stateText;
    m_nowDetail = detail;
    emit infoChanged();
}

void AveraHub::setActiveNav(int index) {
    const int clamped = std::max(0, index);
    if (clamped == m_activeNav) return;
    m_activeNav = clamped;
    emit selectionChanged();
}

void AveraHub::setCurrentCategory(int index) {
    if (m_categories.isEmpty()) return;
    const int clamped = std::clamp(index, 0, int(m_categories.size()) - 1);
    if (clamped == m_category) return;
    m_category = clamped;
    emit selectionChanged();
}

void AveraHub::setFocusIndex(int index) {
    const int clamped = std::clamp(index, 0, 3);
    if (clamped == m_focus) return;
    m_focus = clamped;
    emit selectionChanged();
}

bool AveraHub::moveFocus(int dx) {
    const int next = std::clamp(m_focus + dx, 0, 3);
    if (next == m_focus) return false;
    m_focus = next;
    emit selectionChanged();
    return true;
}

bool AveraHub::moveCategory(int dy) {
    if (m_categories.isEmpty()) return false;
    const int next = std::clamp(m_category + dy, 0, int(m_categories.size()) - 1);
    if (next == m_category) return false;
    m_category = next;
    emit selectionChanged();
    return true;
}

}  // namespace avera
