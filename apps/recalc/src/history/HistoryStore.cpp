#include "HistoryStore.hpp"
#include <QDateTime>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace recalc {
HistoryStore::HistoryStore(const QString &path) : m_connection("recalc-" + QUuid::createUuid().toString()) {
    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connection); m_db.setDatabaseName(path);
    if (!m_db.open()) { m_error = "Historique indisponible : " + m_db.lastError().text(); return; }
    QSqlQuery query(m_db);
    query.exec("PRAGMA busy_timeout=3000"); query.exec("PRAGMA journal_mode=WAL");
    if (!query.exec("PRAGMA user_version") || !query.next()) { m_error = "Impossible de lire la version d’historique."; return; }
    const int version = query.value(0).toInt();
    if (version > 1) { m_error = "Historique créé par une version plus récente : lecture désactivée."; return; }
    if (!m_db.transaction()) { m_error = m_db.lastError().text(); return; }
    const bool ok = query.exec("CREATE TABLE IF NOT EXISTS history (id TEXT PRIMARY KEY, ast_json TEXT NOT NULL, expression_text TEXT NOT NULL, expression_latex TEXT NOT NULL, exact_result TEXT, approximate_result TEXT, angle_mode TEXT NOT NULL, created_at INTEGER NOT NULL, starred INTEGER NOT NULL DEFAULT 0)") && query.exec("PRAGMA user_version=1");
    if (!ok || !m_db.commit()) { m_db.rollback(); m_error = "Impossible de préparer l’historique : " + query.lastError().text(); return; }
    m_ready = true;
}
HistoryStore::~HistoryStore() { m_db.close(); m_db = QSqlDatabase(); QSqlDatabase::removeDatabase(m_connection); }
bool HistoryStore::save(const NodePtr &root, const EvalResult &result, bool degrees) {
    if (!m_ready || !result.ok) return false;
    QSqlQuery query(m_db);
    query.prepare("INSERT INTO history (id,ast_json,expression_text,expression_latex,exact_result,approximate_result,angle_mode,created_at) VALUES (?,?,?,?,?,?,?,?)");
    query.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
    query.addBindValue(QString::fromUtf8(QJsonDocument(toJson(root)).toJson(QJsonDocument::Compact)));
    query.addBindValue(asText(root)); query.addBindValue(asLatex(root));
    query.addBindValue(result.value.exact ? result.display : QString{});
    query.addBindValue(result.value.exact ? result.approximation : result.display);
    query.addBindValue(degrees ? "DEG" : "RAD"); query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) { m_error = "Échec de sauvegarde de l’historique : " + query.lastError().text(); return false; }
    query.exec("DELETE FROM history WHERE starred=0 AND id NOT IN (SELECT id FROM history ORDER BY created_at DESC LIMIT 200)");
    return true;
}
QVariantList HistoryStore::entries() const {
    QVariantList result; if (!m_ready) return result;
    QSqlQuery query(m_db);
    if (!query.exec("SELECT id,expression_text,exact_result,approximate_result,angle_mode,created_at FROM history ORDER BY created_at DESC LIMIT 200")) return result;
    while (query.next()) result.append(QVariantMap{
        {"id", query.value(0)}, {"expression", query.value(1)},
        {"result", query.value(2).toString().isEmpty() ? query.value(3) : query.value(2)},
        {"mode", query.value(4)}, {"date", QDateTime::fromMSecsSinceEpoch(query.value(5).toLongLong()).toString("dd/MM HH:mm")}});
    return result;
}
NodePtr HistoryStore::load(const QString &id) const {
    if (!m_ready) return {};
    QSqlQuery query(m_db); query.prepare("SELECT ast_json FROM history WHERE id=?"); query.addBindValue(id);
    if (!query.exec() || !query.next()) return {};
    const QByteArray data = query.value(0).toByteArray();
    if (data.size() > 131072) { m_error = "Entrée d’historique trop volumineuse."; return {}; }
    return fromJson(QJsonDocument::fromJson(data).object(), &m_error);
}
}
