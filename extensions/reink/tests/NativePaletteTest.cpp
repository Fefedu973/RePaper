#include <QtQuickTest/quicktest.h>
#include <QFontDatabase>
#include <QQmlEngine>
#include <QQmlContext>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include "Geometry.h"
#include "MockNativePenColorModel.h"

// Screenshots use the shipped catalogue and parameter labels. Interaction tests
// retain their deterministic mock and do not require a native SceneController.
class PaletteGeometry : public QObject {
    Q_OBJECT
    static QVariantList strokes(const QVector<PaperDrawing::Stroke> &value) {
        QVariantList result;
        for (const auto &stroke : value) {
            QVariantList points;
            for (const auto &point : stroke.points) points.append(QVariantMap{{"x", point.x()}, {"y", point.y()}});
            result.append(QVariantMap{{"points", points}, {"width", stroke.width}, {"color", stroke.color.name()}});
        }
        return result;
    }
public:
    using QObject::QObject;
    Q_INVOKABLE QVariantList schema(const QString &id) const { return PaperDrawing::stencilParameterSchema(id); }
    Q_INVOKABLE QVariantList catalogue() const {
        QVariantList result;
        for (const auto &symbol : PaperDrawing::electronicsCatalogue())
            result.append(QVariantMap{{"id", symbol.id}, {"name", symbol.name}, {"strokes", strokes(symbol.strokes)},
                {"width", 120}, {"height", 80}, {"configurable", PaperDrawing::isConfigurableStencil(symbol.id)}});
        return result;
    }
    Q_INVOKABLE QVariantMap preview(const QString &id, const QVariantMap &parameters) const {
        QVariantMap normalized; QString error;
        if (!PaperDrawing::normalizeStencilParameters(id, parameters, &normalized, &error)) return {{"valid", false}, {"error", error}};
        const auto symbol = PaperDrawing::configuredStencil(id, normalized);
        return {{"valid", !symbol.strokes.isEmpty()}, {"strokes", strokes(symbol.strokes)}, {"parameters", normalized}, {"width", 120}, {"height", 80}};
    }
};

// Count real QML-to-C++ getter calls while exercising the shipped components.
// The geometry is fixed before each case: it does not manufacture per-read
// conversion cost or drive another binding notification from the counters.
class PalettePerformanceEditor : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool captureEnabled READ available CONSTANT)
    Q_PROPERTY(QString backend READ backend CONSTANT)
    Q_PROPERTY(QString status READ status CONSTANT)
    Q_PROPERTY(QVariantList stencils READ stencils NOTIFY catalogueChanged)
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
public:
    explicit PalettePerformanceEditor(QObject *parent) : QObject(parent) { resetCatalogue(0); }
    bool available() const { return true; }
    QString backend() const { return QStringLiteral("xochitl-performance-fixture"); }
    QString status() const { return {}; }
    QVariantList stencils() const { ++m_stencilReads; return m_catalogue; }
    QVariantMap state() const { ++m_stateReads; return m_state; }
    Q_INVOKABLE void resetCounters() { m_stencilReads = 0; m_stateReads = 0; m_choices = 0; }
    Q_INVOKABLE QVariantMap counters() const {
        return {{"stencilReads",m_stencilReads},{"stateReads",m_stateReads},{"choices",m_choices}};
    }
    Q_INVOKABLE int catalogueSize() const { return m_catalogue.size(); }
    Q_INVOKABLE void resetCatalogue(int requestedSize) {
        PaletteGeometry geometry;
        const auto original = geometry.catalogue();
        m_catalogue = original;
        for (int i=original.size(); i<requestedSize && !original.isEmpty(); ++i) {
            auto row=original.at(i % original.size()).toMap();
            row.insert("id",QStringLiteral("fixture-%1").arg(i));
            row.insert("name",QStringLiteral("Symbole de test %1").arg(i));
            m_catalogue.append(row);
        }
        emit catalogueChanged();
    }
    Q_INVOKABLE void resetState() {
        m_state={{"tool","line"},{"hasSelection",false},{"lineColor","#000000"},
            {"lineStyle","solid"},{"lineWidth",3},{"maximumStrokeWidth",8},
            {"canPlaceStencils",true},{"working",false},{"nativeGestureActive",false},
            {"nativeSelectionWaiting",false},{"nativeCreationInFlight",false},
            {"recentStencils",QStringList{"resistor-iec","capacitor","voltage-source"}}};
        emit changed();
    }
    Q_INVOKABLE void changeState(const QVariantMap &values) {
        for(auto it=values.cbegin();it!=values.cend();++it)m_state.insert(it.key(),it.value());
        emit changed();
    }
    Q_INVOKABLE bool chooseTool(const QString &tool) { ++m_choices;changeState({{"tool",tool}});return true; }
    Q_INVOKABLE bool beginStencil(const QString &id) { ++m_choices;changeState({{"tool","symbol"},{"activeStencilId",id}});return true; }
    Q_INVOKABLE QVariantList stencilSchema(const QString &id) const { return PaperDrawing::stencilParameterSchema(id); }
    Q_INVOKABLE QVariantMap stencilDefaults(const QString &id) const {
        QVariantMap values;
        for(const auto &entry:PaperDrawing::stencilParameterSchema(id)) {
            const auto field=entry.toMap();values.insert(field.value("key").toString(),field.value("default"));
        }
        return values;
    }
    Q_INVOKABLE QVariantMap stencilPreview(const QString &id,const QVariantMap &parameters) const {
        PaletteGeometry geometry;return geometry.preview(id,parameters);
    }
signals:
    void changed();
    void catalogueChanged();
private:
    QVariantList m_catalogue;
    QVariantMap m_state;
    mutable int m_stencilReads=0,m_stateReads=0;
    int m_choices=0;
};

class NativePaletteSetup : public QObject {
    Q_OBJECT
public slots:
    void applicationAvailable() {
        Q_INIT_RESOURCE(editor_panels);
        qmlRegisterType<MockNativePenColorModel>("xofm.libs.toolbar", 1, 0, "PenColorModel");
        const auto font = QString::fromLocal8Bit(qgetenv("REPAPER_NATIVE_UI_FONT"));
        if (!font.isEmpty()) QFontDatabase::addApplicationFont(font);
    }
    void qmlEngineAvailable(QQmlEngine *engine) {
        QFile qmd(QFileInfo(QStringLiteral(__FILE__)).absoluteDir().filePath("../native/editor.qmd"));
        if (qmd.open(QIODevice::ReadOnly)) {
            const QString source=QString::fromUtf8(qmd.readAll()).replace("\r\n","\n");
            const int begin=source.indexOf("            property string repaperLastWritingPenType:");
            const int end=source.indexOf("\n        }\n        TRAVERSE",begin);
            QString methods=begin>=0&&end>begin?source.mid(begin,end-begin):QString();
            methods.replace("~&6504254477&~","toolbar");
            engine->rootContext()->setContextProperty("nativeQuickColorToolbarMethods",methods);
        }
        engine->rootContext()->setContextProperty("nativePaletteGeometry", new PaletteGeometry(engine));
        engine->rootContext()->setContextProperty("nativePalettePerformanceEditor",new PalettePerformanceEditor(engine));
        engine->rootContext()->setContextProperty("nativePaletteBaselineDirectory",
            QString::fromLocal8Bit(qgetenv("REPAPER_NATIVE_UI_BASELINE")));
        engine->rootContext()->setContextProperty("nativePalettePreviewDirectory",
            QString::fromLocal8Bit(qgetenv("REPAPER_NATIVE_UI_PREVIEWS")));
    }
};
QUICK_TEST_MAIN_WITH_SETUP(NativePalette, NativePaletteSetup)
#include "NativePaletteTest.moc"
