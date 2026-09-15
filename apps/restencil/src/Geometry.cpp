#include "Geometry.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTransform>
#include <QXmlStreamWriter>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <tuple>

namespace PaperDrawing {
namespace {
Stroke line(std::initializer_list<QPointF> points) { return {Polyline(points), 2.3, Qt::black}; }
Stroke circle(qreal x, qreal y, qreal r) {
    Stroke result;
    result.width = 2.3;
    for (int i = 0; i <= 48; ++i) {
        const qreal t = i * 2.0 * 3.141592653589793 / 48.0;
        result.points.append(QPointF(x + r * std::cos(t), y + r * std::sin(t)));
    }
    return result;
}
bool commitBytes(const QString &path, const QByteArray &bytes, QString *error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}
bool finiteBounds(const QRectF &rect) {
    return std::isfinite(rect.x()) && std::isfinite(rect.y()) &&
        std::isfinite(rect.width()) && std::isfinite(rect.height()) &&
        std::isfinite(rect.right()) && std::isfinite(rect.bottom());
}

constexpr qreal Pi = 3.14159265358979323846;
const QRectF PlotRect(5, 5, 110, 70);
QVariantMap numericField(QString key, QString label, qreal value, qreal minimum,
                         qreal maximum, qreal step, bool integer = false) {
    return {{"key",key},{"label",label},{"type",integer ? "integer" : "number"},
            {"default",value},{"min",minimum},{"max",maximum},{"step",step}};
}
QVariantMap boolField(QString key, QString label, bool value) {
    return {{"key",key},{"label",label},{"type","bool"},{"default",value}};
}
QVariantMap choiceField(QString key, QString label, QString value, QVariantList options) {
    return {{"key",key},{"label",label},{"type","choice"},{"default",value},{"options",options}};
}
QVariantMap choice(QString value, QString label) { return {{"value",value},{"label",label}}; }
Stroke thinLine(QPointF a, QPointF b, qreal width = 0.75) {
    return {{a,b},width,Qt::black};
}

// Unit-step response of 1/(s² + 2*zeta*s + 1), with x = omega0*t.
// Pole/expm1 forms avoid the exp(-zeta*x)*cosh(...) overflow of the usual
// overdamped expression. Series and sinc forms cover zero and critical damping.
// Reference: https://lpsa.swarthmore.edu/SecondOrder/SOI.html
qreal secondOrderStep(qreal x, qreal zeta) {
    if (x == 0) return 0;
    if (x * std::max(qreal(1),zeta) < 1e-3)
        return x*x*(0.5 - zeta*x/3 + (4*zeta*zeta-1)*x*x/24);
    if (zeta < 1) {
        const qreal v = x*std::sqrt((1-zeta)*(1+zeta));
        const qreal sinc = std::abs(v)<1e-4 ? 1-v*v/6+v*v*v*v/120 : std::sin(v)/v;
        return -std::expm1(-zeta*x) - std::exp(-zeta*x)*
            (-2*std::sin(v/2)*std::sin(v/2)+zeta*x*sinc);
    }
    if (zeta == 1) return -std::expm1(-x)-x*std::exp(-x);
    const qreal beta = std::sqrt((zeta-1)*(zeta+1));
    const qreal v = beta*x;
    if (v < 1e-3) {
        const qreal v2 = v*v;
        const qreal sinhc = 1+v2/6+v2*v2/120;
        return -std::expm1(-zeta*x)-std::exp(-zeta*x)*
            (v2/2+v2*v2/24+zeta*x*sinhc);
    }
    const qreal fast = zeta+beta, slow = 1/fast;
    return (-fast*std::expm1(-slow*x)+slow*std::expm1(-fast*x))/(fast-slow);
}

// Liang-Barsky clips segments, not just sampled vertices. This preserves the
// exact entry/exit at the plot border without drawing lines along that border.
bool clipSegment(QPointF a, QPointF b, QPointF *first, QPointF *last) {
    if (!std::isfinite(a.x()) || !std::isfinite(a.y()) ||
        !std::isfinite(b.x()) || !std::isfinite(b.y())) return false;
    const QPointF d = b-a;
    const qreal p[] = {-d.x(),d.x(),-d.y(),d.y()};
    const qreal q[] = {a.x()-PlotRect.left(),PlotRect.right()-a.x(),
                       a.y()-PlotRect.top(),PlotRect.bottom()-a.y()};
    qreal low = 0, high = 1;
    for (int i=0;i<4;++i) {
        if (p[i] == 0) { if (q[i]<0) return false; continue; }
        const qreal r=q[i]/p[i];
        if (p[i]<0) low=std::max(low,r); else high=std::min(high,r);
        if (low>high) return false;
    }
    auto bound = [](QPointF point) {
        return QPointF(std::clamp(point.x(),PlotRect.left(),PlotRect.right()),
                       std::clamp(point.y(),PlotRect.top(),PlotRect.bottom()));
    };
    *first=bound(a+d*low); *last=bound(a+d*high);
    return QLineF(*first,*last).length()>1e-10;
}

void appendClippedCurve(QVector<Stroke> &strokes, const Polyline &points) {
    Stroke current; current.width=2.3;
    auto flush = [&] { if(current.points.size()>1) strokes.append(current); current.points.clear(); };
    for (int i=1;i<points.size();++i) {
        QPointF a,b;
        if (!clipSegment(points[i-1],points[i],&a,&b)) { flush(); continue; }
        if (!current.points.isEmpty() && QLineF(current.points.last(),a).length()>1e-7) flush();
        if (current.points.isEmpty()) current.points.append(a);
        current.points.append(b);
    }
    flush();
}

qreal plotY(qreal value, const QVariantMap &parameters) {
    const qreal minimum=parameters["yMin"].toDouble(), maximum=parameters["yMax"].toDouble();
    return PlotRect.bottom()-(value-minimum)/(maximum-minimum)*PlotRect.height();
}

void appendGraph(QVector<Stroke> &strokes, const QVariantMap &p) {
    // The first stroke carries the semantic foreground width for native editing.
    const qreal axisY=std::clamp(plotY(0,p),PlotRect.top(),PlotRect.bottom());
    strokes.append(line({{5,75},{5,2},{2,7}}));
    strokes.append(line({{5,2},{8,7}}));
    strokes.append(line({{5,axisY},{118,axisY},{113,axisY-3}}));
    strokes.append(line({{118,axisY},{113,axisY+3}}));
    if (p["grid"].toBool()) {
        for (int i=1;i<=10;++i) {
            const qreal x=PlotRect.left()+PlotRect.width()*i/10;
            strokes.append(thinLine({x,5},{x,75}));
        }
        for (int i=0;i<=10;++i) {
            const qreal y=PlotRect.top()+PlotRect.height()*i/10;
            if(std::abs(y-axisY)>1e-7) strokes.append(thinLine({5,y},{115,y}));
        }
    }
    const QString curve=p["curveType"].toString();
    if (curve=="none") return;
    const qreal tMax=p["tMax"].toDouble(), gain=p["gain"].toDouble();
    const qreal tau=p["tau"].toDouble(), omega=p["omega0"].toDouble();
    const qreal zeta=p["damping"].toDouble(), initial=p["initialValue"].toDouble();
    QVector<qreal> times;
    for (int i=0;i<=1024;++i) times.append(tMax*i/1024);
    if(curve=="sine") {
        // Keep zero crossings and extrema exact even when the requested
        // frequency/phase does not coincide with the uniform sample grid.
        const qreal frequency=p["frequencyHz"].toDouble(),phaseCycles=p["phaseDegrees"].toDouble()/360;
        const int first=int(std::ceil(4*phaseCycles)),last=int(std::floor(4*(frequency*tMax+phaseCycles)));
        for(int quarter=first;quarter<=last;++quarter) {
            const qreal t=(quarter/4.0-phaseCycles)/frequency;
            if(t>0&&t<tMax)times.append(t);
        }
    } else {
        // Retain a rapid transient even when it occupies a small part of the time span.
        const qreal timeScale=curve=="exponential" ? tau : 1/omega;
        for (int i=-32;i<=32;++i) {
            const qreal t=timeScale*std::pow(2.0,i/4.0);
            if(t>0 && t<tMax) times.append(t);
        }
    }
    std::sort(times.begin(),times.end());
    times.erase(std::unique(times.begin(),times.end()),times.end());
    Polyline points;
    for(qreal t:times) {
        const qreal value=curve=="sine" ? p["offset"].toDouble()+p["amplitude"].toDouble()*
            std::sin(2*Pi*(p["frequencyHz"].toDouble()*t+p["phaseDegrees"].toDouble()/360)) :
            curve=="exponential" ? initial+(gain-initial)*(-std::expm1(-t/tau)) :
            gain*secondOrderStep(omega*t,zeta);
        points.append({PlotRect.left()+t/tMax*PlotRect.width(),plotY(value,p)});
    }
    appendClippedCurve(strokes,points);
}

void appendBode(QVector<Stroke> &strokes, const QVariantMap &p) {
    strokes.append(line({{5,5},{115,5},{115,75},{5,75},{5,5}}));
    const qreal logMinimum=std::log10(p["frequencyMin"].toDouble());
    const qreal logMaximum=std::log10(p["frequencyMax"].toDouble());
    auto mapX = [&](qreal logFrequency) {
        return PlotRect.left()+(logFrequency-logMinimum)/(logMaximum-logMinimum)*PlotRect.width();
    };
    for(int exponent=int(std::floor(logMinimum));exponent<=int(std::ceil(logMaximum));++exponent) {
        for(int multiplier=1;multiplier<=9;++multiplier) {
            const qreal value=exponent+std::log10(qreal(multiplier));
            if(value<=logMinimum+1e-10 || value>=logMaximum-1e-10) continue;
            const qreal x=mapX(value);
            strokes.append(thinLine({x,5},{x,75},multiplier==1 ? 1.15 : 0.75));
        }
    }
    for(int i=1;i<10;++i) {
        const qreal y=PlotRect.top()+PlotRect.height()*i/10;
        strokes.append(thinLine({5,y},{115,y}));
    }
    if (!p["curve"].toBool()) return;
    const qreal omega=p["omega0"].toDouble(), zeta=p["damping"].toDouble();
    const qreal gain=p["gain"].toDouble();
    QVector<qreal> frequencies;
    for (int i=0;i<=1024;++i) frequencies.append(logMinimum+(logMaximum-logMinimum)*i/1024);
    const qreal naturalLogFrequency=std::log10(omega/(2*Pi));
    // Resolve narrow resonance near omega0 even at very small damping.
    for(int i=-32;i<=32;++i) {
        const qreal logFrequency=naturalLogFrequency+zeta*i/8;
        if(logFrequency>=logMinimum && logFrequency<=logMaximum) frequencies.append(logFrequency);
    }
    std::sort(frequencies.begin(),frequencies.end());
    frequencies.erase(std::unique(frequencies.begin(),frequencies.end()),frequencies.end());
    Polyline points;
    for(qreal logFrequency:frequencies) {
        const qreal ratio=2*Pi*std::pow(10.0,logFrequency)/omega;
        const qreal real=1-ratio*ratio, imaginary=2*zeta*ratio;
        const qreal value=p["mode"].toString()=="phase" ? -std::atan2(imaginary,real)*180/Pi :
            20*(std::log10(gain)-std::log10(std::hypot(real,imaginary)));
        points.append({mapX(logFrequency),plotY(value,p)});
    }
    appendClippedCurve(strokes,points);
}
}

bool isConfigurableStencil(QString id) { return id=="table" || id=="graph" || id=="bode"; }

QVariantList stencilParameterSchema(QString id) {
    QVariantList fields;
    if(id=="table") {
        fields={numericField("rows","Lignes",4,1,30,1,true),
                numericField("columns","Colonnes",4,1,30,1,true)};
    } else if(id=="graph") {
        fields={boolField("grid","Quadrillage",true),
            choiceField("curveType","Courbe","none",{choice("none","À compléter"),
                choice("exponential","Exponentielle"),choice("second-order","Réponse du second ordre"),choice("sine","Sinusoïde")}),
            numericField("gain","Valeur finale / gain K",1,-1e6,1e6,0.1),
            numericField("tau","Constante de temps τ (s)",1,1e-6,1e6,0.1),
            numericField("initialValue","Valeur initiale (exponentielle)",0,-1e6,1e6,0.1),
            numericField("omega0","Pulsation propre ω₀ (rad/s)",1,1e-6,1e6,0.1),
            numericField("damping","Amortissement ζ",0.5,0,100,0.05),
            numericField("amplitude","Amplitude A",1,0,1e6,0.1),
            numericField("offset","Décalage vertical",0,-1e6,1e6,0.1),
            numericField("frequencyHz","Fréquence f (Hz)",1,1e-6,1e6,0.1),
            numericField("phaseDegrees","Phase φ (°)",0,-360,360,15),
            numericField("tMax","Durée (s)",10,1e-6,1e6,1),
            numericField("yMin","Ordonnée minimale",-0.2,-1e6,1e6,0.1),
            numericField("yMax","Ordonnée maximale",1.4,-1e6,1e6,0.1)};
    } else if(id=="bode") {
        fields={choiceField("mode","Ordonnée","gain",{choice("gain","Gain (dB)"),choice("phase","Phase (°)")}),
            boolField("curve","Tracer la réponse du second ordre",false),
            numericField("frequencyMin","Fréquence minimale (Hz)",0.1,1e-6,1e6,0.1),
            numericField("frequencyMax","Fréquence maximale (Hz)",100,1e-6,1e6,1),
            numericField("gain","Gain statique K",1,1e-6,1e6,0.1),
            numericField("omega0","Pulsation propre ω₀ (rad/s)",2*Pi,1e-6,1e6,0.1),
            numericField("damping","Amortissement ζ",0.5,1e-6,100,0.05),
            numericField("yMin","Ordonnée minimale (dB ou °)",-60,-1e6,1e6,10),
            numericField("yMax","Ordonnée maximale (dB ou °)",20,-1e6,1e6,10)};
    } else return {};
    fields.append(boolField("opaqueBackground","Fond blanc plein",true));
    return fields;
}

QVariantMap stencilDefaults(QString id) {
    QVariantMap result;
    for(const auto &entry:stencilParameterSchema(id)) {
        const auto field=entry.toMap();
        result.insert(field["key"].toString(),field["default"]);
    }
    return result;
}

bool normalizeStencilParameters(QString id, const QVariantMap &input, QVariantMap *output, QString *error) {
    auto fail = [&](QString message) { if(error) *error=message; return false; };
    if(!isConfigurableStencil(id)) return fail("Stencil configurable inconnu : "+id);
    QVariantMap normalized=stencilDefaults(id);
    for(auto it=input.cbegin();it!=input.cend();++it)
        if(!normalized.contains(it.key())) return fail("Paramètre inconnu : "+it.key());
    for(const auto &entry:stencilParameterSchema(id)) {
        const auto field=entry.toMap();
        const auto key=field["key"].toString(), type=field["type"].toString();
        if(!input.contains(key)) continue;
        const QVariant value=input[key];
        const QString label=field["label"].toString();
        if(type=="bool") {
            if(value.metaType().id()!=QMetaType::Bool) return fail(label+" : valeur oui/non requise.");
            normalized[key]=value.toBool();
        } else if(type=="choice") {
            bool found=false;
            for(const auto &option:field["options"].toList())
                if(value.metaType().id()==QMetaType::QString && option.toMap()["value"]==value) found=true;
            if(!found) return fail(label+" : choix non reconnu.");
            normalized[key]=value.toString();
        } else {
            bool ok=false;
            const double number=value.toDouble(&ok);
            const bool sineField=id=="graph"&&(key=="amplitude"||key=="offset"||key=="frequencyHz"||key=="phaseDegrees");
            const int meta=value.metaType().id();
            const bool numeric=meta==QMetaType::Double||meta==QMetaType::Float||meta==QMetaType::Int||meta==QMetaType::UInt||
                meta==QMetaType::LongLong||meta==QMetaType::ULongLong||meta==QMetaType::Long||meta==QMetaType::ULong||
                meta==QMetaType::Short||meta==QMetaType::UShort;
            if(!ok || meta==QMetaType::Bool || !std::isfinite(number)||(sineField&&!numeric))
                return fail(label+" : nombre fini requis.");
            if(number<field["min"].toDouble() || number>field["max"].toDouble())
                return fail(label+" : valeur hors limites ("+field["min"].toString()+" à "+field["max"].toString()+").");
            if(type=="integer" && std::floor(number)!=number) return fail(label+" : entier requis.");
            normalized[key]=type=="integer" ? QVariant(int(number)) : QVariant(number);
        }
    }
    if(id!="table" && normalized["yMax"].toDouble()<=normalized["yMin"].toDouble())
        return fail("L’ordonnée maximale doit dépasser l’ordonnée minimale.");
    if(id!="table" && normalized["yMax"].toDouble()-normalized["yMin"].toDouble()<1e-9)
        return fail("L’intervalle des ordonnées doit mesurer au moins 10⁻⁹.");
    if(id=="bode") {
        const qreal minimum=normalized["frequencyMin"].toDouble(), maximum=normalized["frequencyMax"].toDouble();
        if(maximum<=minimum) return fail("La fréquence maximale doit dépasser la fréquence minimale.");
        if(std::log10(maximum)-std::log10(minimum)<1e-9)
            return fail("L’intervalle de fréquences est trop étroit.");
        if(std::log10(maximum/minimum)>8+1e-10) return fail("Le quadrillage est limité à huit décades.");
    }
    if(id=="graph" && normalized["curveType"].toString()=="second-order" &&
       normalized["damping"].toDouble()<1) {
        const qreal zeta=normalized["damping"].toDouble();
        const qreal cycles=normalized["omega0"].toDouble()*normalized["tMax"].toDouble()*
            std::sqrt((1-zeta)*(1+zeta))/(2*Pi);
        if(cycles>32) return fail("Réduisez la durée ou la pulsation : 32 oscillations maximum par graphe.");
    }
    if(id=="graph"&&normalized["curveType"].toString()=="sine"&&
        normalized["frequencyHz"].toDouble()*normalized["tMax"].toDouble()>32)
        return fail("Réduisez la durée ou la fréquence : 32 périodes maximum par graphe.");
    if(output) *output=normalized;
    if(error) error->clear();
    return true;
}

Symbol configuredStencil(QString id, const QVariantMap &parameters) {
    QVariantMap p;
    if(!normalizeStencilParameters(id,parameters,&p)) return {};
    Symbol symbol;
    symbol.id=id;
    symbol.category="Tableaux et graphes";
    symbol.standard="Configurable";
    symbol.anchors={{5,5},{115,5},{115,75},{5,75}};
    symbol.portIds={"top-left","top-right","bottom-right","bottom-left"};
    if(id=="table") {
        symbol.name="Tableau";
        symbol.strokes.append(line({{5,5},{115,5},{115,75},{5,75},{5,5}}));
        for(int i=1;i<p["rows"].toInt();++i) {
            const qreal y=5+70.0*i/p["rows"].toInt();
            symbol.strokes.append(thinLine({5,y},{115,y},1.15));
        }
        for(int i=1;i<p["columns"].toInt();++i) {
            const qreal x=5+110.0*i/p["columns"].toInt();
            symbol.strokes.append(thinLine({x,5},{x,75},1.15));
        }
    } else if(id=="graph") {
        symbol.name="Graphe à deux axes";
        appendGraph(symbol.strokes,p);
    } else {
        symbol.name="Diagramme de Bode";
        appendBode(symbol.strokes,p);
    }
    return symbol;
}

Polyline roundedRectangle(QRectF rect, qreal radius) {
    if (!finiteBounds(rect) || !std::isfinite(radius)) return {};
    rect = rect.normalized();
    if (rect.isEmpty()) return {};
    radius = std::clamp(radius, qreal(0), std::min(rect.width(), rect.height()) / 2);
    if (radius == 0) return {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft(), rect.topLeft()};
    const QPointF centers[] = {{rect.right()-radius, rect.top()+radius},
        {rect.right()-radius, rect.bottom()-radius}, {rect.left()+radius, rect.bottom()-radius},
        {rect.left()+radius, rect.top()+radius}};
    Polyline result;
    for (int corner=0; corner<4; ++corner) for (int step=0; step<=16; ++step) {
        const qreal angle = (-1 + corner + step / 16.0) * 3.141592653589793 / 2;
        const QPointF point = centers[corner] + QPointF(radius*std::cos(angle), radius*std::sin(angle));
        result.append({std::clamp(point.x(), rect.left(), rect.right()),
                       std::clamp(point.y(), rect.top(), rect.bottom())});
    }
    result.append(result.first());
    return result;
}

Polyline ellipse(QRectF rect) {
    if (!finiteBounds(rect)) return {};
    rect = rect.normalized();
    if (rect.isEmpty()) return {};
    Polyline result;
    const qreal rx=rect.width()/2, ry=rect.height()/2;
    const QPointF center(rect.left()+rx, rect.top()+ry);
    for (int i=0; i<96; ++i) {
        const qreal angle = i * 2 * 3.141592653589793 / 96;
        result.append({std::clamp(center.x()+rx*std::cos(angle), rect.left(), rect.right()),
                       std::clamp(center.y()+ry*std::sin(angle), rect.top(), rect.bottom())});
    }
    result.append(result.first());
    return result;
}

QVector<Symbol> electronicsCatalogue() {
    QVector<Symbol> out;
    const QVector<QPointF> horizontal{{0, 40}, {120, 40}};
    auto add = [&](QString id, QString name, QString category, QString standard,
                   QVector<Stroke> strokes, QVector<QPointF> anchors = {}) {
        QStringList ports{"left", "right"};
        if (id=="diode" || id=="led" || id=="zener") ports={"anode","cathode"};
        else if (id=="npn" || id=="pnp") ports={"base","collector","emitter"};
        else if (id=="mosfet") ports={"gate","drain","source"};
        else if (id=="opamp") ports={"inverting","non-inverting","output"};
        else if (id=="voltage-source" || id=="battery" || id=="capacitor-polarized") ports={"positive","negative"};
        else if (id=="current-source") ports={"top","bottom"};
        else if (id=="voltage-generator-simple") ports={"start","end"};
        else if (id=="ground" || id=="signal-ground") ports={"ground"};
        else if (id=="node") ports={"left","right","top","bottom"};
        else if (id=="connector") ports={"terminal"};
        else if (id=="arrow") ports={"start","end"};
        else if (id=="lightbulb") ports={"base"};
        out.append({id, name, category, standard, strokes,
                    anchors.isEmpty() ? horizontal : anchors, ports});
    };
    add("resistor-iec", "Résistance IEC", "Passifs", "IEC", {
        line({{0,40},{30,40},{30,26},{90,26},{90,54},{30,54},{30,40}}), line({{90,40},{120,40}})});
    add("resistor-ansi", "Résistance ANSI", "Passifs", "ANSI", {
        line({{0,40},{25,40},{31,26},{43,54},{55,26},{67,54},{79,26},{91,54},{97,40},{120,40}})});
    add("capacitor", "Condensateur", "Passifs", "IEC / ANSI", {
        line({{0,40},{53,40}}), line({{53,15},{53,65}}),
        line({{67,15},{67,65}}), line({{67,40},{120,40}})});
    add("capacitor-polarized", "Condensateur polarisé", "Passifs", "IEC / ANSI", {
        line({{0,40},{53,40}}), line({{53,15},{53,65}}), line({{67,15},{67,65}}),
        line({{67,40},{120,40}}), line({{30,10},{30,24}}), line({{23,17},{37,17}})});
    Stroke coil; coil.width = 2.3; coil.points = {{0,40},{25,40}};
    for (int i=0; i<4; ++i) for (int j=0; j<=16; ++j) {
        qreal t = 3.141592653589793 * j / 16;
        coil.points.append({25 + i*17.5 + 8.75*(1-std::cos(t)),40-17*std::sin(t)});
    }
    coil.points.append({120,40});
    add("inductor", "Inductance", "Passifs", "IEC / ANSI", {coil});
    QVector<Stroke> diode{
        line({{0,40},{40,40}}), line({{40,20},{40,60},{80,40},{40,20}}),
        line({{80,18},{80,62}}), line({{80,40},{120,40}})};
    add("diode", "Diode", "Semi-conducteurs", "IEC / ANSI", diode);
    auto led = diode;
    led.append(line({{60,12},{78,0},{70,1}}));
    led.append(line({{78,0},{76,8}}));
    led.append(line({{76,18},{94,6},{86,7}}));
    led.append(line({{94,6},{92,14}}));
    add("led", "LED", "Semi-conducteurs", "IEC / ANSI", led);
    auto zener = diode;
    zener[2] = line({{72,12},{80,18},{80,62},{88,68}});
    add("zener", "Diode Zener", "Semi-conducteurs", "IEC / ANSI", zener);
    QVector<Stroke> bjt{line({{0,40},{45,40}}),line({{45,18},{45,62}}),
        line({{45,29},{85,6},{85,0}}),line({{45,51},{85,74},{85,80}}),
        line({{73,60},{79,71},{66,71}})};
    add("npn", "Transistor NPN", "Semi-conducteurs", "IEC / ANSI", bjt, {{0,40},{85,0},{85,80}});
    bjt[4] = line({{67,68},{56,57},{69,57}});
    add("pnp", "Transistor PNP", "Semi-conducteurs", "IEC / ANSI", bjt, {{0,40},{85,0},{85,80}});
    add("mosfet", "MOSFET canal N", "Semi-conducteurs", "IEC / ANSI", {
        line({{0,40},{40,40}}),line({{40,15},{40,65}}),line({{50,15},{50,65}}),
        line({{50,20},{90,20},{90,0}}),line({{50,60},{90,60},{90,80}}),
        line({{90,40},{57,40},{66,34}}),line({{57,40},{66,46}})}, {{0,40},{90,0},{90,80}});
    add("opamp", "Amplificateur opérationnel", "Amplificateurs", "IEC / ANSI", {
        line({{25,2},{25,78},{95,40},{25,2}}),line({{0,20},{25,20}}),
        line({{0,60},{25,60}}),line({{95,40},{120,40}}),line({{33,20},{45,20}}),
        line({{33,60},{45,60}}),line({{39,54},{39,66}})}, {{0,20},{0,60},{120,40}});
    add("voltage-source", "Source de tension", "Sources", "IEC / ANSI", {
        line({{60,0},{60,14}}),circle(60,40,26),line({{60,66},{60,80}}),
        line({{50,29},{70,29}}),line({{60,19},{60,39}}),line({{50,52},{70,52}})}, {{60,0},{60,80}});
    add("current-source", "Source de courant", "Sources", "IEC / ANSI", {
        line({{60,0},{60,14}}),circle(60,40,26),line({{60,66},{60,80}}),
        line({{60,57},{60,23},{51,33}}),line({{60,23},{69,33}})}, {{60,0},{60,80}});
    add("voltage-generator-simple", "Générateur de tension simple", "Sources", "Universel", {
        line({{0,40},{120,40}}),circle(60,40,26)});
    Stroke signalWave; signalWave.width=2.3;
    for(int i=0;i<=48;++i){
        const qreal t=qreal(i)/48;
        signalWave.points.append({42+36*t,40-10*std::sin(2*Pi*t)});
    }
    add("signal-generator", "Générateur de signal", "Sources", "Universel", {
        line({{0,40},{34,40}}),circle(60,40,26),signalWave,line({{86,40},{120,40}})});
    add("battery", "Pile", "Sources", "IEC / ANSI", {
        line({{0,40},{50,40}}),line({{50,10},{50,70}}),line({{68,25},{68,55}}),
        line({{68,40},{120,40}})});
    add("ground", "Masse", "Connexions", "IEC / ANSI", {
        line({{60,0},{60,35}}),line({{28,35},{92,35}}),
        line({{40,48},{80,48}}),line({{51,61},{69,61}})}, {{60,0}});
    add("signal-ground", "Masse signal", "Connexions", "IEC / ANSI", {
        line({{60,0},{60,32},{28,32},{60,65},{92,32},{60,32}})}, {{60,0}});
    add("switch", "Interrupteur ouvert", "Connexions", "IEC / ANSI", {
        line({{0,40},{30,40}}),circle(33,40,3),line({{35,38},{86,14}}),
        circle(91,40,3),line({{94,40},{120,40}})});
    add("switch-closed", "Interrupteur fermé", "Connexions", "Universel", {
        line({{0,40},{30,40}}),circle(33,40,3),line({{36,40},{88,40}}),
        circle(91,40,3),line({{94,40},{120,40}})});
    add("node", "Nœud de connexion", "Connexions", "IEC / ANSI", {
        line({{0,40},{120,40}}),line({{60,0},{60,80}}),circle(60,40,4),circle(60,40,2)},
        {{0,40},{120,40},{60,0},{60,80}});
    add("connector", "Connecteur", "Connexions", "IEC / ANSI", {
        line({{0,40},{48,40}}),circle(60,40,12)}, {{0,40}});
    add("arrow", "Flèche", "Connexions", "Universel", {
        line({{0,40},{115,40},{95,23}}),line({{115,40},{95,57}})});
    Stroke bulb; bulb.width=2.3; bulb.points={{49,60},{49,49}};
    for(int i=0;i<=60;++i) {
        const qreal angle=2*Pi/3+5*Pi*i/180;
        bulb.points.append({60+23*std::cos(angle),29+23*std::sin(angle)});
    }
    bulb.points.append({71,60}); bulb.points.append({49,60});
    add("lightbulb", "Ampoule", "Icônes", "Universel", {bulb,
        line({{50,66},{70,66}}),line({{52,72},{68,72},{64,76},{56,76},{52,72}}),
        line({{55,60},{55,42},{50,36},{56,40},{60,35},{64,40},{70,36},{65,42},{65,60}}),
        line({{25,29},{31,29}}),line({{89,29},{95,29}}),
        line({{34,4},{39,9}}),line({{81,9},{86,4}})}, {{60,76}});
    out.append({"square-root","Racine carrée","Mathématiques","Universel",
        {{squareRoot(QRectF(0,0,120,80)),2.3,Qt::black}}, {}, {}});
    for(const auto &id:{QString("table"),QString("bode"),QString("graph")})
        out.append(configuredStencil(id));
    return out;
}

bool supportsVoltageArrow(const Symbol &symbol) {
    if(symbol.id=="arrow" || symbol.strokes.isEmpty() || symbol.anchors.size()!=2 || symbol.portIds.size()!=2)
        return false;
    const auto a=symbol.anchors[0],b=symbol.anchors[1];
    return std::isfinite(a.x())&&std::isfinite(a.y())&&std::isfinite(b.x())&&std::isfinite(b.y())
        && QLineF(a,b).length()>1e-6;
}
Polyline squareRoot(QRectF rect) {
    if(!finiteBounds(rect))return {};
    rect=rect.normalized();if(rect.isEmpty())return {};
    const qreal hook=std::min(rect.width()*.6,rect.height()*.55);
    const qreal height=rect.height();
    return {rect.topLeft()+QPointF(0,height*.55),rect.topLeft()+QPointF(hook*.22,height*.47),
        rect.topLeft()+QPointF(hook*.5,height*.9),rect.topLeft()+QPointF(hook,height*.1),
        rect.topLeft()+QPointF(rect.width(),height*.1)};
}
bool supportsVoltageArrow(const QString &symbolId) {
    for(const auto &symbol:electronicsCatalogue())if(symbol.id==symbolId)return supportsVoltageArrow(symbol);
    return false;
}
QVector<Stroke> voltageArrowStrokes(const Symbol &symbol,bool reversed,bool otherSide) {
    if(!supportsVoltageArrow(symbol))return {};
    const auto origin=symbol.anchors[0];
    const qreal length=QLineF(origin,symbol.anchors[1]).length();
    const auto axis=(symbol.anchors[1]-origin)/length;
    // Above a horizontal dipole; a clockwise quarter-turn puts the same
    // annotation on the right of a vertical dipole, pointing upward.
    const QPointF normal(axis.y(),-axis.x());
    qreal low=0,high=0;
    for(const auto &stroke:symbol.strokes)for(auto point:stroke.points) {
        const qreal projection=QPointF::dotProduct(point-origin,normal);
        if(!std::isfinite(projection))return {};
        low=std::min(low,projection);high=std::max(high,projection);
    }
    const auto offset=normal*(otherSide?low-14:high+14);
    const auto from=symbol.anchors[reversed?0:1]+offset;
    const auto to=symbol.anchors[reversed?1:0]+offset;
    QVector<Stroke> result{line({from,to})};
    result+=arrowHead(from,to,std::clamp(length*.1,qreal(6),qreal(14)),2.3);
    return result;
}

QVector<Polyline> dashByArcLength(const Polyline &points, const QVector<qreal> &input, qreal phase) {
    if (points.size() < 2) return points.isEmpty() ? QVector<Polyline>{} : QVector<Polyline>{points};
    QVector<qreal> pattern = input;
    if (pattern.isEmpty()) return {points};
    qreal total = 0;
    for (qreal length : pattern) { if (!std::isfinite(length) || length <= 0) return {}; total += length; }
    if (pattern.size() % 2) { pattern += input; total *= 2; }
    if (!std::isfinite(phase) || total < 0.01) return {};
    phase = std::fmod(std::fmod(phase, total) + total, total);
    int index = 0;
    while (phase >= pattern[index]) { phase -= pattern[index]; index = (index+1) % pattern.size(); }
    qreal remaining = pattern[index] - phase;
    QVector<Polyline> output;
    Polyline current;
    for (int i = 1; i < points.size(); ++i) {
        const QLineF segment(points[i-1], points[i]);
        const qreal length = segment.length();
        if (!std::isfinite(length)) return {};
        if (length < 1e-9) continue;
        qreal consumed = 0;
        while (consumed < length - 1e-9) {
            if (output.size() > 100000) return {};
            const qreal step = std::min(remaining, length-consumed);
            if (!(index % 2)) {
                if (current.isEmpty()) current.append(segment.pointAt(consumed / length));
                current.append(segment.pointAt((consumed + step) / length));
            }
            consumed += step;
            remaining -= step;
            if (remaining <= 1e-9) {
                if (!(index % 2) && !current.isEmpty()) { output.append(current); current.clear(); }
                index = (index+1) % pattern.size();
                remaining = pattern[index];
            }
        }
    }
    if (!current.isEmpty()) output.append(current);
    return output;
}

Polyline orthogonalRoute(QPointF start, QPointF end, bool horizontalFirst) {
    Polyline result{start};
    QPointF corner = horizontalFirst ? QPointF(end.x(), start.y()) : QPointF(start.x(), end.y());
    if (QLineF(start, corner).length() > 1e-6 && QLineF(end, corner).length() > 1e-6) result.append(corner);
    result.append(end);
    return result;
}

namespace {
constexpr qreal RouteEpsilon = 1e-7;
bool finiteRoutePoint(QPointF point) {
    return std::isfinite(point.x()) && std::isfinite(point.y()) &&
        std::abs(point.x()) <= 1e9 && std::abs(point.y()) <= 1e9;
}
int routeDirection(QPointF vector) {
    if (std::abs(vector.x()) < RouteEpsilon && std::abs(vector.y()) < RouteEpsilon) return -1;
    if (std::abs(vector.x()) >= std::abs(vector.y())) return vector.x() > 0 ? 0 : 2;
    return vector.y() > 0 ? 1 : 3;
}
QPointF directionVector(int direction) {
    static const QPointF vectors[]={{1,0},{0,1},{-1,0},{0,-1}};
    return direction < 0 ? QPointF() : vectors[direction];
}
bool axisSegment(QPointF a, QPointF b) {
    return a.x() == b.x() || a.y() == b.y();
}
bool overlapsOccupied(QPointF a,QPointF b,const OccupiedRouteSegment &occupied) {
    const bool horizontal=a.y()==b.y();
    if(horizontal ? std::abs(occupied.from.y()-occupied.to.y())>RouteEpsilon ||
                         std::abs(a.y()-occupied.from.y())>RouteEpsilon
                  : std::abs(occupied.from.x()-occupied.to.x())>RouteEpsilon ||
                         std::abs(a.x()-occupied.from.x())>RouteEpsilon)return false;
    const qreal low=horizontal?std::max(std::min(a.x(),b.x()),std::min(occupied.from.x(),occupied.to.x()))
                              :std::max(std::min(a.y(),b.y()),std::min(occupied.from.y(),occupied.to.y()));
    const qreal high=horizontal?std::min(std::max(a.x(),b.x()),std::max(occupied.from.x(),occupied.to.x()))
                               :std::min(std::max(a.y(),b.y()),std::max(occupied.from.y(),occupied.to.y()));
    if(high-low<=RouteEpsilon)return false;
    for(auto junction:occupied.sharedJunctions) {
        const qreal along=horizontal?junction.x():junction.y();
        const qreal across=horizontal?std::abs(junction.y()-a.y()):std::abs(junction.x()-a.x());
        if(across<=RouteEpsilon&&along>=low-RouteEpsilon&&along<=high+RouteEpsilon)return false;
    }
    return true;
}
bool crossesInterior(QPointF a, QPointF b, const QRectF &rect) {
    if (a.y() == b.y())
        return a.y() > rect.top()+RouteEpsilon && a.y() < rect.bottom()-RouteEpsilon &&
            std::max(a.x(),b.x()) > rect.left()+RouteEpsilon &&
            std::min(a.x(),b.x()) < rect.right()-RouteEpsilon;
    return a.x() > rect.left()+RouteEpsilon && a.x() < rect.right()-RouteEpsilon &&
        std::max(a.y(),b.y()) > rect.top()+RouteEpsilon &&
        std::min(a.y(),b.y()) < rect.bottom()-RouteEpsilon;
}
Polyline simplifyRoute(const Polyline &input) {
    Polyline out;
    for (const auto &point : input) {
        if (!out.isEmpty() && out.last() == point) continue;
        while (out.size() >= 2) {
            const auto a=out[out.size()-2], b=out.last();
            if (!axisSegment(a,b) || !axisSegment(b,point) ||
                routeDirection(b-a) != routeDirection(point-b)) break;
            out.removeLast();
        }
        out.append(point);
    }
    return out;
}
int nearestExit(QPointF point, const QVector<QRectF> &rectangles, const QVector<int> &owners,
                QPointF target, bool horizontalFirst) {
    int best=-1;
    qreal distance=std::numeric_limits<qreal>::infinity();
    const int toward=routeDirection(target-point);
    const int order[4] = {horizontalFirst ? 0 : 1, horizontalFirst ? 2 : 3,
                          horizontalFirst ? 1 : 0, horizontalFirst ? 3 : 2};
    for (int direction : order) for (int index : owners) {
        const auto &rect=rectangles[index];
        const qreal candidate=direction==0 ? rect.right()-point.x() : direction==1 ? rect.bottom()-point.y() :
                              direction==2 ? point.x()-rect.left() : point.y()-rect.top();
        if (candidate < distance-RouteEpsilon || (std::abs(candidate-distance)<RouteEpsilon && direction==toward)) {
            distance=candidate; best=direction;
        }
    }
    return best;
}
}

QPointF portExitDirection(const Symbol &symbol, const QString &portId, int quarterTurns) {
    const int index=symbol.portIds.indexOf(portId);
    if (index < 0 || index >= symbol.anchors.size() || !finiteRoutePoint(symbol.anchors[index])) return {};
    const auto box=bounds(symbol.strokes).normalized();
    if (!finiteBounds(box) || box.isEmpty()) return {};
    const auto point=symbol.anchors[index];
    const qreal distances[]={std::abs(point.x()-box.right()),std::abs(point.y()-box.bottom()),
                             std::abs(point.x()-box.left()),std::abs(point.y()-box.top())};
    int direction=0;
    for (int i=1;i<4;++i) if (distances[i]<distances[direction]) direction=i;
    return directionVector((direction+(quarterTurns%4+4)%4)%4);
}

Polyline orthogonalRoute(QPointF start, QPointF end, const OrthogonalRouteOptions &options) {
    if (!finiteRoutePoint(start) || !finiteRoutePoint(end) ||
        !finiteRoutePoint(options.startDirection) || !finiteRoutePoint(options.endDirection) ||
        !std::isfinite(options.clearance) || options.clearance < 0 || options.clearance > 1e6 ||
        !std::isfinite(options.grid) || options.grid < 0 || options.grid > 1e6 ||
        (options.grid > 0 && options.grid < 1e-6) || options.obstacles.size() > 2048 ||
        options.occupiedSegments.size()>16384) return {};
    for(const auto &segment:options.occupiedSegments) {
        if(!finiteRoutePoint(segment.from)||!finiteRoutePoint(segment.to)||
            !axisSegment(segment.from,segment.to)||segment.sharedJunctions.size()>4)return {};
        for(auto point:segment.sharedJunctions)if(!finiteRoutePoint(point))return {};
    }
    if (start == end) return {start};
    QVector<QRectF> raw, obstacles;
    QVector<int> startOwners, endOwners;
    for (auto rect : options.obstacles) {
        if (!finiteBounds(rect)) return {};
        rect=rect.normalized();
        if (rect.isEmpty()) continue;
        const int index=raw.size();
        raw.append(rect);
        const auto expanded=rect.adjusted(-options.clearance,-options.clearance,options.clearance,options.clearance);
        if (!finiteRoutePoint(expanded.topLeft()) || !finiteRoutePoint(expanded.bottomRight())) return {};
        obstacles.append(expanded);
        const auto inclusive=rect.adjusted(-RouteEpsilon,-RouteEpsilon,RouteEpsilon,RouteEpsilon);
        if (inclusive.contains(start)) startOwners.append(index);
        if (inclusive.contains(end)) endOwners.append(index);
    }
    int startDirection=routeDirection(options.startDirection), endDirection=routeDirection(options.endDirection);
    if (startDirection<0 && !startOwners.isEmpty()) startDirection=nearestExit(start,raw,startOwners,end,options.horizontalFirst);
    if (endDirection<0 && !endOwners.isEmpty()) endDirection=nearestExit(end,raw,endOwners,start,options.horizontalFirst);
    auto clearSegment=[&](QPointF a,QPointF b,const QVector<int> &ignored) {
        if (!axisSegment(a,b)) return false;
        for (int i=0;i<obstacles.size();++i)
            if (!ignored.contains(i) && crossesInterior(a,b,obstacles[i])) return false;
        for(const auto &segment:options.occupiedSegments)if(overlapsOccupied(a,b,segment))return false;
        return true;
    };
    auto validRoute=[&](const Polyline &route) {
        if (route.size()<2 || route.first()!=start || route.last()!=end) return false;
        int previous=-1;
        for (int i=1;i<route.size();++i) {
            if (!finiteRoutePoint(route[i]) || route[i]==route[i-1] || !axisSegment(route[i-1],route[i])) return false;
            const int direction=routeDirection(route[i]-route[i-1]);
            if (direction<0 || (previous>=0 && direction==(previous+2)%4)) return false;
            if (i==1 && startDirection>=0 && direction!=startDirection) return false;
            if (i==route.size()-1 && endDirection>=0 && direction!=(endDirection+2)%4) return false;
            QVector<int> ignored;
            if (i==1) ignored=startOwners;
            if (i==route.size()-1) ignored+=endOwners;
            if (!clearSegment(route[i-1],route[i],ignored)) return false;
            // Nonconsecutive orthogonal segments must not cross, touch or
            // overlap. Moving an endpoint can otherwise fold a retained lane
            // back through an earlier leg and leave a visible loop.
            for (int j=1;j<i-1;++j) {
                const auto a=route[i-1], b=route[i], c=route[j-1], d=route[j];
                if (std::max(std::min(a.x(),b.x()),std::min(c.x(),d.x())) <=
                        std::min(std::max(a.x(),b.x()),std::max(c.x(),d.x())) &&
                    std::max(std::min(a.y(),b.y()),std::min(c.y(),d.y())) <=
                        std::min(std::max(a.y(),b.y()),std::max(c.y(),d.y()))) return false;
            }
            previous=direction;
        }
        return true;
    };
    Polyline preferred;
    if (options.preferredRoute.size()<=256) {
        bool usable=true;
        for (int i=0;i<options.preferredRoute.size();++i)
            if (!finiteRoutePoint(options.preferredRoute[i]) ||
                (i && !axisSegment(options.preferredRoute[i-1],options.preferredRoute[i]))) usable=false;
        if (usable) preferred=simplifyRoute(options.preferredRoute);
    }
    // Retain a legal existing posture while dragging a connected component.
    // Only its endpoint legs move; interior lanes stay put whenever still valid.
    if (preferred.size()>=2) {
        auto adjusted=preferred;
        adjusted.first()=start; adjusted.last()=end;
        if (adjusted.size()>2) {
            if (preferred[0].x()==preferred[1].x()) adjusted[1].setX(start.x());
            else adjusted[1].setY(start.y());
            const int last=preferred.size()-1;
            if (preferred[last].x()==preferred[last-1].x()) adjusted[last-1].setX(end.x());
            else adjusted[last-1].setY(end.y());
        }
        adjusted=simplifyRoute(adjusted);
        if (validRoute(adjusted)) return adjusted;
    }
    // Most wires need no search. Try both elbow postures automatically.
    for (bool horizontal : {options.horizontalFirst,!options.horizontalFirst}) {
        const auto simple=simplifyRoute(orthogonalRoute(start,end,horizontal));
        if (validRoute(simple)) return simple;
    }
    const qreal pitch=std::max({qreal(8),options.grid,options.clearance});
    auto floorGrid=[&](qreal value) { return options.grid>0 ? std::floor(value/options.grid)*options.grid : value; };
    auto ceilGrid=[&](qreal value) { return options.grid>0 ? std::ceil(value/options.grid)*options.grid : value; };
    auto escape=[&](QPointF point,int direction,const QVector<int> &owners) {
        if (direction<0) return point;
        qreal length=pitch;
        for (int index : owners) {
            const auto &rect=obstacles[index];
            const qreal distance=direction==0 ? rect.right()-point.x() : direction==1 ? rect.bottom()-point.y() :
                                 direction==2 ? point.x()-rect.left() : point.y()-rect.top();
            length=std::max(length,distance);
        }
        auto result=point+directionVector(direction)*length;
        if (direction==0) result.setX(ceilGrid(result.x()));
        if (direction==1) result.setY(ceilGrid(result.y()));
        if (direction==2) result.setX(floorGrid(result.x()));
        if (direction==3) result.setY(floorGrid(result.y()));
        // A free endpoint/short port gap may need its first bend sooner than
        // the preferred lead length. Stop at the nearest legal obstacle lane.
        for (int i=0;i<obstacles.size();++i) {
            if (owners.contains(i) || !crossesInterior(point,result,obstacles[i])) continue;
            const auto &rect=obstacles[i];
            if (direction==0) result.setX(floorGrid(rect.left()));
            if (direction==1) result.setY(floorGrid(rect.top()));
            if (direction==2) result.setX(ceilGrid(rect.right()));
            if (direction==3) result.setY(ceilGrid(rect.bottom()));
        }
        return result;
    };
    auto from=escape(start,startDirection,startOwners), to=escape(end,endDirection,endOwners);
    // A detached copied endpoint may coincide with another wire's port. It
    // has no enforced port direction: if the nearest exit shares that wire's
    // lead, choose another clear side instead of inventing an attachment.
    auto freeLead=[&](QPointF point,int &direction,const QVector<int> &owners,QPointF requested,QPointF &lead) {
        if(routeDirection(requested)>=0||owners.isEmpty()||clearSegment(point,lead,owners))return;
        qreal bestLength=std::numeric_limits<qreal>::infinity();int bestDirection=-1;QPointF bestLead;
        for(int candidateDirection=0;candidateDirection<4;++candidateDirection) {
            const auto candidate=escape(point,candidateDirection,owners);
            const qreal length=QLineF(point,candidate).length();
            if(routeDirection(candidate-point)!=candidateDirection||!clearSegment(point,candidate,owners))continue;
            if(length<bestLength) {bestLength=length;bestDirection=candidateDirection;bestLead=candidate;}
        }
        if(bestDirection>=0){direction=bestDirection;lead=bestLead;}
    };
    freeLead(start,startDirection,startOwners,options.startDirection,from);
    freeLead(end,endDirection,endOwners,options.endDirection,to);
    if (!finiteRoutePoint(from) || !finiteRoutePoint(to) ||
        (startDirection>=0 && routeDirection(from-start)!=startDirection) ||
        (endDirection>=0 && routeDirection(to-end)!=endDirection) ||
        !clearSegment(start,from,startOwners) || !clearSegment(end,to,endOwners)) return {};
    QVector<qreal> xs{from.x(),to.x(),start.x(),end.x()}, ys{from.y(),to.y(),start.y(),end.y()};
    xs.append(floorGrid((from.x()+to.x())/2)); ys.append(floorGrid((from.y()+to.y())/2));
    for (const auto &rect : obstacles) {
        xs.append(floorGrid(rect.left())); xs.append(ceilGrid(rect.right()));
        ys.append(floorGrid(rect.top())); ys.append(ceilGrid(rect.bottom()));
    }
    for (const auto &point : preferred) { xs.append(point.x()); ys.append(point.y()); }
    const qreal wireSpacing=std::max(qreal(8),options.grid);
    for(const auto &segment:options.occupiedSegments) {
        xs.append(segment.from.x());xs.append(segment.to.x());ys.append(segment.from.y());ys.append(segment.to.y());
        if(segment.from.y()==segment.to.y()) {
            ys.append(floorGrid(segment.from.y()-wireSpacing));ys.append(ceilGrid(segment.from.y()+wireSpacing));
        } else {
            xs.append(floorGrid(segment.from.x()-wireSpacing));xs.append(ceilGrid(segment.from.x()+wireSpacing));
        }
        for(auto point:segment.sharedJunctions){xs.append(point.x());ys.append(point.y());}
    }
    xs.append(floorGrid(*std::min_element(xs.begin(),xs.end())-pitch));
    xs.append(ceilGrid(*std::max_element(xs.begin(),xs.end())+pitch));
    ys.append(floorGrid(*std::min_element(ys.begin(),ys.end())-pitch));
    ys.append(ceilGrid(*std::max_element(ys.begin(),ys.end())+pitch));
    auto unique=[](QVector<qreal> &values) {
        std::sort(values.begin(),values.end());
        values.erase(std::unique(values.begin(),values.end()),values.end());
    };
    unique(xs); unique(ys);
    const int nx=xs.size(), ny=ys.size();
    if (qint64(nx)*ny>200000) return {};
    const int nodes=nx*ny;
    QVector<quint8> horizontalBlocked(nodes,0), verticalBlocked(nodes,0);
    // Block edges on the compressed grid in rectangles, avoiding a per-node
    // scan over every component during the A* search. Boundary lanes are legal.
    auto lower=[](const QVector<qreal> &values,qreal value) { return int(std::lower_bound(values.begin(),values.end(),value)-values.begin()); };
    auto upper=[](const QVector<qreal> &values,qreal value) { return int(std::upper_bound(values.begin(),values.end(),value)-values.begin()); };
    for (const auto &rect : obstacles) {
        const int xBegin=std::max(0,upper(xs,rect.left()+RouteEpsilon)-1), xEnd=std::min(nx-1,lower(xs,rect.right()-RouteEpsilon));
        const int yBegin=std::max(0,upper(ys,rect.top()+RouteEpsilon)-1), yEnd=std::min(ny-1,lower(ys,rect.bottom()-RouteEpsilon));
        for (int y=upper(ys,rect.top()+RouteEpsilon);y<lower(ys,rect.bottom()-RouteEpsilon);++y)
            for (int x=xBegin;x<xEnd;++x) horizontalBlocked[y*nx+x]=1;
        for (int x=upper(xs,rect.left()+RouteEpsilon);x<lower(xs,rect.right()-RouteEpsilon);++x)
            for (int y=yBegin;y<yEnd;++y) verticalBlocked[y*nx+x]=1;
    }
    // Occupied wires block only parallel edges on their own lane. Crossing a
    // wire perpendicularly is allowed and never creates a logical junction.
    for(const auto &segment:options.occupiedSegments) {
        if(segment.from.y()==segment.to.y()) {
            const int begin=std::max(0,upper(xs,std::min(segment.from.x(),segment.to.x()))-1);
            const int end=std::min(nx-1,lower(xs,std::max(segment.from.x(),segment.to.x())));
            for(int y=lower(ys,segment.from.y()-RouteEpsilon);y<upper(ys,segment.from.y()+RouteEpsilon);++y)
                for(int x=begin;x<end;++x)if(overlapsOccupied({xs[x],ys[y]},{xs[x+1],ys[y]},segment))horizontalBlocked[y*nx+x]=1;
        } else {
            const int begin=std::max(0,upper(ys,std::min(segment.from.y(),segment.to.y()))-1);
            const int end=std::min(ny-1,lower(ys,std::max(segment.from.y(),segment.to.y())));
            for(int x=lower(xs,segment.from.x()-RouteEpsilon);x<upper(xs,segment.from.x()+RouteEpsilon);++x)
                for(int y=begin;y<end;++y)if(overlapsOccupied({xs[x],ys[y]},{xs[x],ys[y+1]},segment))verticalBlocked[y*nx+x]=1;
        }
    }
    const int origin=lower(ys,from.y())*nx+lower(xs,from.x());
    const int goal=lower(ys,to.y())*nx+lower(xs,to.x());
    auto pointAt=[&](int node) { return QPointF(xs[node%nx],ys[node/nx]); };
    auto heuristic=[&](int node) { const auto point=pointAt(node); return std::abs(point.x()-to.x())+std::abs(point.y()-to.y()); };
    auto newLength=[&](QPointF a,QPointF b) {
        qreal overlap=0;
        const bool horizontal=a.y()==b.y();
        const qreal lo=horizontal ? std::min(a.x(),b.x()) : std::min(a.y(),b.y());
        const qreal hi=horizontal ? std::max(a.x(),b.x()) : std::max(a.y(),b.y());
        for (int i=1;i<preferred.size();++i) {
            const auto c=preferred[i-1], d=preferred[i];
            if (horizontal ? c.y()!=a.y() || d.y()!=a.y() : c.x()!=a.x() || d.x()!=a.x()) continue;
            const qreal p=horizontal ? std::min(c.x(),d.x()) : std::min(c.y(),d.y());
            const qreal q=horizontal ? std::max(c.x(),d.x()) : std::max(c.y(),d.y());
            overlap+=std::max(qreal(0),std::min(hi,q)-std::max(lo,p));
        }
        return std::max(qreal(0),hi-lo-overlap);
    };
    const qreal infinity=std::numeric_limits<qreal>::infinity();
    QVector<qreal> costs(nodes*5,infinity);
    QVector<int> previous(nodes*5,-1);
    using QueueEntry=std::tuple<qreal,qreal,int>;
    std::priority_queue<QueueEntry,std::vector<QueueEntry>,std::greater<QueueEntry>> queue;
    const int initial=origin*5+(startDirection<0 ? 4 : startDirection);
    costs[initial]=0; queue.emplace(heuristic(origin),0,initial);
    const qreal bendCost=std::max({qreal(4),options.grid*1.5,options.clearance*0.75});
    const int arrival=endDirection<0 ? -1 : (endDirection+2)%4;
    qreal best=infinity;
    int finish=-1;
    const int order[4]={options.horizontalFirst ? 0 : 1,options.horizontalFirst ? 2 : 3,
                        options.horizontalFirst ? 1 : 0,options.horizontalFirst ? 3 : 2};
    while (!queue.empty()) {
        const auto [estimate,cost,state]=queue.top(); queue.pop();
        if (cost>costs[state]+RouteEpsilon) continue;
        if (estimate>best+RouteEpsilon) break;
        const int node=state/5, incoming=state%5;
        if (node==goal) {
            if (arrival>=0 && incoming<4 && arrival==(incoming+2)%4) continue;
            const qreal score=cost+(arrival>=0 && incoming<4 && arrival!=incoming ? bendCost : 0);
            if (score<best-RouteEpsilon) { best=score; finish=state; }
            continue;
        }
        const int x=node%nx,y=node/nx;
        for (int direction : order) {
            if (incoming<4 && direction==(incoming+2)%4) continue;
            if ((direction==0 && (x==nx-1 || horizontalBlocked[node])) ||
                (direction==1 && (y==ny-1 || verticalBlocked[node])) ||
                (direction==2 && (x==0 || horizontalBlocked[node-1])) ||
                (direction==3 && (y==0 || verticalBlocked[node-nx]))) continue;
            const int next=node+(direction==0 ? 1 : direction==1 ? nx : direction==2 ? -1 : -nx);
            const int nextState=next*5+direction;
            const auto a=pointAt(node),b=pointAt(next);
            const qreal length=std::abs(a.x()-b.x())+std::abs(a.y()-b.y());
            const qreal turn=incoming<4 && direction!=incoming ? bendCost : 0;
            const qreal posture=incoming==4 && ((direction%2==0)!=options.horizontalFirst) ? 0.001 : 0;
            const qreal score=cost+length+turn+posture+(preferred.isEmpty() ? 0 : newLength(a,b)*0.15);
            if (score<costs[nextState]-RouteEpsilon) {
                costs[nextState]=score; previous[nextState]=state;
                queue.emplace(score+heuristic(next),score,nextState);
            }
        }
    }
    if (finish<0) return {};
    Polyline core;
    for (int state=finish;state>=0;state=previous[state]) core.append(pointAt(state/5));
    std::reverse(core.begin(),core.end());
    Polyline result{start}; result+=core; result.append(end);
    result=simplifyRoute(result);
    return validRoute(result) ? result : Polyline();
}

QPointF snapPoint(QPointF point, const QVector<QPointF> &anchors, qreal radius, qreal grid) {
    qreal best = radius;
    QPointF result = point;
    bool matched = false;
    for (QPointF anchor : anchors) {
        const qreal distance = QLineF(point, anchor).length();
        if (distance <= best) { best = distance; result = anchor; matched = true; }
    }
    if (!matched && grid > 0) return {std::round(point.x()/grid)*grid, std::round(point.y()/grid)*grid};
    return result;
}

QVector<Stroke> arrowHead(QPointF from, QPointF to, qreal size, qreal width) {
    const QLineF segment(from, to);
    if (segment.length() < 1e-6) return {};
    const QPointF tangent = (to-from) / segment.length();
    const QPointF normal(-tangent.y(), tangent.x());
    return {{{to-tangent*size+normal*size*0.45, to, to-tangent*size-normal*size*0.45},width,Qt::black}};
}

QVector<Stroke> transformStrokes(const QVector<Stroke> &strokes, QPointF origin, qreal scale, int quarterTurns) {
    QTransform transform;
    transform.translate(origin.x(), origin.y());
    transform.scale(scale, scale);
    transform.rotate((quarterTurns % 4) * 90);
    QVector<Stroke> output = strokes;
    for (auto &stroke : output) {
        stroke.width *= std::abs(scale);
        for (auto &point : stroke.points) point = transform.map(point);
    }
    return output;
}

QRectF bounds(const QVector<Stroke> &strokes) {
    qreal left=0, top=0, right=0, bottom=0;
    bool any = false;
    for (const auto &stroke : strokes) for (auto point : stroke.points) {
        if (!any) { left=right=point.x(); top=bottom=point.y(); any=true; }
        left=std::min(left,point.x()); right=std::max(right,point.x());
        top=std::min(top,point.y()); bottom=std::max(bottom,point.y());
    }
    return any ? QRectF(QPointF(left,top),QPointF(right,bottom)) : QRectF();
}

void paintStrokes(QPainter &painter, const QVector<Stroke> &strokes) {
    for (const auto &stroke : strokes) {
        painter.setPen(QPen(stroke.color, stroke.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (stroke.points.size() == 1) painter.drawPoint(stroke.points.front());
        else if (!stroke.points.isEmpty()) painter.drawPolyline(stroke.points.constData(), stroke.points.size());
    }
}

bool exportSvg(const QString &path, const QVector<Stroke> &strokes, QString *error) {
    QByteArray bytes;
    QXmlStreamWriter writer(&bytes);
    writer.setAutoFormatting(true);
    writer.writeStartDocument();
    writer.writeStartElement("svg"); writer.writeDefaultNamespace("http://www.w3.org/2000/svg");
    writer.writeAttribute("viewBox", "0 0 1404 1872");
    writer.writeAttribute("width", "179.7mm"); writer.writeAttribute("height", "239.6mm");
    writer.writeAttribute("fill", "none"); writer.writeAttribute("stroke-linecap", "round");
    writer.writeAttribute("stroke-linejoin", "round");
    for (const auto &stroke : strokes) {
        if (stroke.points.isEmpty()) continue;
        writer.writeStartElement(stroke.points.size() == 1 ? "circle" : "polyline");
        if (stroke.points.size() == 1) {
            writer.writeAttribute("cx",QString::number(stroke.points[0].x(),'f',3));
            writer.writeAttribute("cy",QString::number(stroke.points[0].y(),'f',3));
            writer.writeAttribute("r",QString::number(stroke.width/2,'f',3));
            writer.writeAttribute("fill",stroke.color.name());
            writer.writeAttribute("stroke","none");
        } else {
            QStringList points;
            for (auto p : stroke.points) points.append(QString::number(p.x(),'f',3)+","+QString::number(p.y(),'f',3));
            writer.writeAttribute("points",points.join(' '));
            writer.writeAttribute("stroke",stroke.color.name());
            writer.writeAttribute("stroke-width",QString::number(stroke.width,'f',3));
        }
        writer.writeEndElement();
    }
    writer.writeEndElement(); writer.writeEndDocument();
    return commitBytes(path, bytes, error);
}

bool exportPdf(const QString &path, const QVector<Stroke> &strokes, const QString &title, QString *error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { if (error) *error=file.errorString(); return false; }
    {
        QPdfWriter pdf(&file);
        pdf.setTitle(title); pdf.setCreator("reStencil / reInk");
        pdf.setPageSize(QPageSize(QSizeF(179.7,239.6),QPageSize::Millimeter));
        pdf.setPageMargins(QMarginsF(0,0,0,0)); pdf.setResolution(229);
        QPainter painter;
        if (!painter.begin(&pdf)) { if (error) *error="Impossible de créer le PDF"; return false; }
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(pdf.width()/PageWidth,pdf.height()/PageHeight);
        paintStrokes(painter,strokes); painter.end();
    }
    if (!file.commit()) { if (error) *error=file.errorString(); return false; }
    return true;
}

bool exportScene(const QString &path, const QVector<Stroke> &strokes, const QString &title, QString *error) {
    QJsonArray items;
    for (const auto &stroke : strokes) {
        QJsonArray points;
        for (auto p : stroke.points) points.append(QJsonArray{p.x(),p.y()});
        // The native writer requires a polyline; preserve a pen tap as a tiny segment.
        if(stroke.points.size()==1) {
            auto p=stroke.points.first();
            const qreal delta=p.x()<PageWidth-0.02?0.02:-0.02;
            points.append(QJsonArray{p.x()+delta,p.y()});
        }
        items.append(QJsonObject{{"points",points},{"width",stroke.width},{"color",stroke.color.name()}});
    }
    const QJsonObject scene{{"schemaVersion",1},{"title",title},
        {"page",QJsonObject{{"width",PageWidth},{"height",PageHeight}}},{"strokes",items}};
    return commitBytes(path,QJsonDocument(scene).toJson(),error);
}

QString exportDirectory(const QString &appName) {
    Q_UNUSED(appName)
    const QString path=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/exports";
    QDir().mkpath(path);
    return path;
}
}
