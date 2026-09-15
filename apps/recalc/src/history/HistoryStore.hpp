#pragma once
#include "evaluation/Evaluator.hpp"
#include <QSqlDatabase>
#include <QVariantList>

namespace recalc {
class HistoryStore {
public:
    explicit HistoryStore(const QString &path);
    ~HistoryStore();
    bool save(const NodePtr &root, const EvalResult &result, bool degrees);
    QVariantList entries() const;
    NodePtr load(const QString &id) const;
    QString error() const { return m_error; }
    bool ready() const { return m_ready; }
private:
    QString m_connection;
    QSqlDatabase m_db;
    mutable QString m_error;
    bool m_ready = false;
};
}
