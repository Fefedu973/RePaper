#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QPluginLoader>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2)
        return 2;
    const QString pluginRoot = QString::fromLocal8Bit(argv[1]);
    app.setLibraryPaths({pluginRoot});
    QPluginLoader linuxfb(pluginRoot + QStringLiteral("/platforms/libqlinuxfb.so"));
    const auto metadata = linuxfb.metaData();
    // Qt's plugin metadata stores the major/minor ABI and clears patch bits.
    if (metadata.value(QStringLiteral("version")).toInt() != (QT_VERSION & 0xffff00)
        || !metadata.value(QStringLiteral("MetaData")).toObject()
                .value(QStringLiteral("Keys")).toArray().contains(QStringLiteral("linuxfb"))
        || !linuxfb.instance()) {
        qCritical() << "LinuxFB load or metadata failure" << metadata << linuxfb.errorString();
        return 1;
    }
    QPluginLoader tablet(pluginRoot + QStringLiteral("/generic/libqevdevtablet.so"));
    const auto tabletMetadata = tablet.metaData();
    if (tabletMetadata.value(QStringLiteral("version")).toInt() != (QT_VERSION & 0xffff00)
        || !tabletMetadata.value(QStringLiteral("MetaData")).toObject()
                .value(QStringLiteral("Keys")).toArray().contains(QStringLiteral("EvdevTablet"))
        || !tablet.instance()) {
        qCritical() << "EvdevTablet load or metadata failure" << tabletMetadata << tablet.errorString();
        return 1;
    }
    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"));
    db.setDatabaseName(QStringLiteral(":memory:"));
    if (!db.open() || !db.transaction()) {
        qCritical() << "SQLite open/transaction failure" << db.lastError();
        return 1;
    }
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("CREATE TABLE check_plugin (id INTEGER PRIMARY KEY, text_value TEXT)"))
        || !query.prepare(QStringLiteral("INSERT INTO check_plugin VALUES (?, ?)"))) {
        qCritical() << query.lastError();
        return 1;
    }
    query.addBindValue(42);
    const QString expected = QString::fromUtf8("reMoodle / reAgenda / reCalc — été");
    query.addBindValue(expected);
    if (!query.exec() || !db.commit()
        || !query.exec(QStringLiteral("SELECT id, text_value FROM check_plugin"))
        || !query.next() || query.value(0).toInt() != 42 || query.value(1).toString() != expected) {
        qCritical() << "SQLite Unicode bound query failure" << query.lastError();
        return 1;
    }
    qInfo() << "PASS: Qt" << qVersion() << "LinuxFB and EvdevTablet plugin load and SQLite transaction/Unicode query";
    return 0;
}
