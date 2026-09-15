#pragma once
#include "Geometry.h"
#include <QObject>
#include <QQuickPaintedItem>
#include <QSet>
#include <QVariantList>
namespace repaper { class BridgeClient; }

class StencilCatalogue : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList symbols READ symbols NOTIFY symbolsChanged)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY symbolsChanged)
    Q_PROPERTY(QString category READ category WRITE setCategory NOTIFY symbolsChanged)
    Q_PROPERTY(bool favoritesOnly READ favoritesOnly WRITE setFavoritesOnly NOTIFY symbolsChanged)
    Q_PROPERTY(QString selectedId READ selectedId WRITE setSelectedId NOTIFY selectedChanged)
    Q_PROPERTY(QVariantMap selected READ selected NOTIFY selectedChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString outputDirectory READ outputDirectory CONSTANT)
    Q_PROPERTY(QString insertionStatus READ insertionStatus CONSTANT)
    Q_PROPERTY(bool emulatorMode READ emulatorMode CONSTANT)
public:
    explicit StencilCatalogue(QObject *parent = nullptr);
    QVariantList symbols() const;
    QString query() const { return m_query; }
    QString category() const { return m_category; }
    bool favoritesOnly() const { return m_favoritesOnly; }
    QString selectedId() const { return m_selectedId; }
    QVariantMap selected() const;
    QString status() const { return m_status; }
    QString outputDirectory() const;
    QString insertionStatus() const;
    bool emulatorMode() const;
    void setQuery(const QString &value);
    void setCategory(const QString &value);
    void setFavoritesOnly(bool value);
    void setSelectedId(const QString &value);
    Q_INVOKABLE void toggleFavorite(const QString &id);
    Q_INVOKABLE void exportSelected(const QString &format);
    Q_INVOKABLE void exportPack(const QString &format);
    Q_INVOKABLE void importNative(bool entirePack);
signals:
    void symbolsChanged();
    void selectedChanged();
    void statusChanged();
private:
    void writeExport(const QVector<PaperDrawing::Stroke> &strokes, const QString &name,
                     const QString &format);
    QVector<PaperDrawing::Symbol> m_symbols;
    QSet<QString> m_favorites;
    QString m_query, m_category, m_selectedId, m_status;
    bool m_favoritesOnly = false;
    repaper::BridgeClient *m_bridge;
};

class SymbolPreview : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString symbolId READ symbolId WRITE setSymbolId NOTIFY symbolIdChanged)
    Q_PROPERTY(bool showAnchors READ showAnchors WRITE setShowAnchors NOTIFY showAnchorsChanged)
public:
    explicit SymbolPreview(QQuickItem *parent = nullptr);
    QString symbolId() const { return m_symbolId; }
    bool showAnchors() const { return m_showAnchors; }
    void setSymbolId(const QString &id);
    void setShowAnchors(bool value);
    void paint(QPainter *painter) override;
signals:
    void symbolIdChanged();
    void showAnchorsChanged();
private:
    QString m_symbolId;
    bool m_showAnchors = false;
};
