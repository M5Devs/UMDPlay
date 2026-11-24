#pragma once

// SPDX-License-Identifier: GPL-2.0-only

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

namespace avera {

/// One entry of the vertical category rail.
struct AveraCategory {
    QString title;
    QString sub;
};

/// The disc presented in the hero slot.
struct AveraHero {
    QString     artUrl;      ///< image://… or qrc:/ URL, may be empty
    QString     title;
    QString     meta;
    QVariantList badges;     ///< [{ text, strong }]
};

/// The dimmed "next up" card beside the hero.
struct AveraPeek {
    QString artUrl;
    QString title;
    QString meta;
    QString kind;
    QString slot;
    QString cta;
};

/// State for the Avera media-center hub.
///
/// Purely presentational: the host fills it from whatever it discovered on the
/// disc (PARAM.SFO, PLAYLIST.UMD, RESOURCE art) and the QML scene renders it.
/// The only interaction state it owns is which rail category and which action
/// pill are focused, so a remote/D-pad can drive the hub.
class AveraHub : public QObject {
    Q_OBJECT

    Q_PROPERTY(QStringList navItems READ navItems NOTIFY infoChanged)
    Q_PROPERTY(int activeNav READ activeNav WRITE setActiveNav NOTIFY selectionChanged)

    Q_PROPERTY(QString driveLabel READ driveLabel NOTIFY infoChanged)
    Q_PROPERTY(QString mediumText READ mediumText NOTIFY infoChanged)
    Q_PROPERTY(QString sizeText READ sizeText NOTIFY infoChanged)
    Q_PROPERTY(QString stateText READ stateText NOTIFY infoChanged)
    Q_PROPERTY(QString codecText READ codecText NOTIFY infoChanged)
    Q_PROPERTY(QString engineText READ engineText NOTIFY infoChanged)

    Q_PROPERTY(QVariantList categories READ categories NOTIFY infoChanged)
    Q_PROPERTY(int currentCategory READ currentCategory WRITE setCurrentCategory
                   NOTIFY selectionChanged)

    Q_PROPERTY(QString heroArt READ heroArt NOTIFY infoChanged)
    Q_PROPERTY(QString heroTitle READ heroTitle NOTIFY infoChanged)
    Q_PROPERTY(QString heroMeta READ heroMeta NOTIFY infoChanged)
    Q_PROPERTY(QVariantList heroBadges READ heroBadges NOTIFY infoChanged)

    Q_PROPERTY(QString peekArt READ peekArt NOTIFY infoChanged)
    Q_PROPERTY(QString peekTitle READ peekTitle NOTIFY infoChanged)
    Q_PROPERTY(QString peekMeta READ peekMeta NOTIFY infoChanged)
    Q_PROPERTY(QString peekKind READ peekKind NOTIFY infoChanged)
    Q_PROPERTY(QString peekSlot READ peekSlot NOTIFY infoChanged)
    Q_PROPERTY(QString peekCta READ peekCta NOTIFY infoChanged)

    Q_PROPERTY(QString chapterPillText READ chapterPillText NOTIFY infoChanged)
    Q_PROPERTY(int focusIndex READ focusIndex WRITE setFocusIndex NOTIFY selectionChanged)
    Q_PROPERTY(QString nowStateText READ nowStateText NOTIFY infoChanged)
    Q_PROPERTY(QString nowDetail READ nowDetail NOTIFY infoChanged)

public:
    explicit AveraHub(QObject* parent = nullptr);

    void setNavItems(const QStringList& items);
    void setStatus(const QString& drive, const QString& medium, const QString& size,
                   const QString& state, const QString& codec, const QString& engine);
    void setCategories(const QVector<AveraCategory>& categories);
    void setHero(const AveraHero& hero);
    void setPeek(const AveraPeek& peek);
    void setChapterPillText(const QString& text);
    void setNowPlaying(const QString& stateText, const QString& detail);

    QStringList navItems() const { return m_navItems; }
    int activeNav() const { return m_activeNav; }
    void setActiveNav(int index);

    QString driveLabel() const { return m_drive; }
    QString mediumText() const { return m_medium; }
    QString sizeText() const { return m_size; }
    QString stateText() const { return m_state; }
    QString codecText() const { return m_codec; }
    QString engineText() const { return m_engine; }

    QVariantList categories() const { return m_categories; }
    int currentCategory() const { return m_category; }
    void setCurrentCategory(int index);

    QString heroArt() const { return m_hero.artUrl; }
    QString heroTitle() const { return m_hero.title; }
    QString heroMeta() const { return m_hero.meta; }
    QVariantList heroBadges() const { return m_hero.badges; }

    QString peekArt() const { return m_peek.artUrl; }
    QString peekTitle() const { return m_peek.title; }
    QString peekMeta() const { return m_peek.meta; }
    QString peekKind() const { return m_peek.kind; }
    QString peekSlot() const { return m_peek.slot; }
    QString peekCta() const { return m_peek.cta; }

    QString chapterPillText() const { return m_chapterPill; }
    int focusIndex() const { return m_focus; }
    void setFocusIndex(int index);
    QString nowStateText() const { return m_nowState; }
    QString nowDetail() const { return m_nowDetail; }

    /// Move the action-pill focus by `dx`; true when it changed.
    Q_INVOKABLE bool moveFocus(int dx);
    /// Move the rail selection by `dy`; true when it changed.
    Q_INVOKABLE bool moveCategory(int dy);

signals:
    void infoChanged();
    void selectionChanged();

private:
    QStringList    m_navItems;
    int            m_activeNav = 0;
    QString        m_drive;
    QString        m_medium;
    QString        m_size;
    QString        m_state;
    QString        m_codec;
    QString        m_engine;
    QVariantList   m_categories;
    int            m_category = 1;      // "Recent Discs" is the live library
    AveraHero      m_hero;
    AveraPeek      m_peek;
    QString        m_chapterPill;
    int            m_focus = 0;
    QString        m_nowState = QStringLiteral("READY");
    QString        m_nowDetail;
};

}  // namespace avera
