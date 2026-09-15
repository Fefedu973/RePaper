#include "Evaluator.hpp"
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>

namespace recalc {
namespace {
struct Failure { QString message; };
[[noreturn]] void fail(const QString &message) { throw Failure{message}; }
BigInt absInt(BigInt n) { return n < 0 ? -n : n; }
unsigned bits(const BigInt &n) { auto magnitude = absInt(n); return magnitude == 0 ? 0 : boost::multiprecision::msb(magnitude) + 1; }
void check(const BigInt &n) { if (bits(n) > MaxIntegerBits) fail("Limite de calcul exact atteinte (4096 bits). Réduisez l’expression."); }
BigInt gcd(BigInt a, BigInt b) {
    a = absInt(a); b = absInt(b);
    while (b != 0) { BigInt r = a % b; a = b; b = r; } return a;
}
Value rational(BigInt numerator, BigInt denominator = 1) {
    if (denominator == 0) fail("Division par zéro.");
    check(numerator); check(denominator);
    if (denominator < 0) { numerator = -numerator; denominator = -denominator; }
    BigInt divisor = gcd(numerator, denominator);
    Value value; value.numerator = numerator / divisor; value.denominator = denominator / divisor; return value;
}
Value approx(long double number) {
    if (!std::isfinite(number) || std::fabs(number) > std::numeric_limits<double>::max() || (number != 0 && double(number) == 0)) fail("Résultat numérique hors limites.");
    Value value; value.exact = false; value.approximate = number; return value;
}
long double decimal(const Value &n) {
    long double result = n.exact ? n.numerator.convert_to<long double>() / n.denominator.convert_to<long double>() : n.approximate;
    if (!std::isfinite(result)) fail("Nombre trop grand pour une approximation.");
    return result;
}
BigInt multiply(const BigInt &a, const BigInt &b) {
    if (bits(a) + bits(b) > MaxIntegerBits + 1) fail("Limite de calcul exact atteinte (4096 bits). Réduisez l’expression.");
    BigInt n = a * b; check(n); return n;
}
Value product(const Value &a, const Value &b) {
    if (!a.exact || !b.exact) return approx(decimal(a) * decimal(b));
    // Cancel before multiplication so large but reducible products stay exact.
    const BigInt g1 = gcd(a.numerator, b.denominator), g2 = gcd(b.numerator, a.denominator);
    return rational(multiply(a.numerator / g1, b.numerator / g2), multiply(a.denominator / g2, b.denominator / g1));
}
Value divide(const Value &a, const Value &b) {
    if (b.exact ? b.numerator == 0 : b.approximate == 0) fail("Division par zéro.");
    if (!a.exact || !b.exact) return approx(decimal(a) / decimal(b));
    return product(a, rational(b.denominator, b.numerator));
}
Value add(const Value &a, const Value &b, bool subtract = false) {
    if (!a.exact || !b.exact) return approx(decimal(a) + (subtract ? -decimal(b) : decimal(b)));
    BigInt common = gcd(a.denominator, b.denominator);
    BigInt left = multiply(a.numerator, b.denominator / common), right = multiply(b.numerator, a.denominator / common);
    BigInt numerator = subtract ? BigInt(left - right) : BigInt(left + right);
    return rational(numerator, multiply(a.denominator, b.denominator / common));
}
Value negate(Value value) { if (value.exact) value.numerator = -value.numerator; else value.approximate = -value.approximate; return value; }
BigInt parseInteger(const QString &s) {
    // cpp_int's string constructor treats leading zero as octal; parse decimal explicitly.
    bool negative = s.startsWith('-'); BigInt result = 0;
    for (int i = negative ? 1 : 0; i < s.size(); ++i) { result *= 10; result += s[i].unicode() - '0'; check(result); }
    return negative ? -result : result;
}
Value parseNumber(const QString &text) {
    QString digits = text; const int point = digits.indexOf('.'); BigInt denominator = 1;
    if (point >= 0) { for (int i = point + 1; i < digits.size(); ++i) denominator *= 10; digits.remove(point, 1); }
    return rational(parseInteger(digits), denominator);
}
Value power(Value base, const Value &exponent) {
    if (exponent.exact && exponent.denominator == 1) {
        if (absInt(exponent.numerator) > 10000) fail("Exposant limité à 10 000.");
        int count = exponent.numerator.convert_to<int>();
        if (count == 0) { if (decimal(base) == 0) fail("0⁰ n’est pas défini."); return rational(1); }
        if (count < 0) { base = divide(rational(1), base); count = -count; }
        Value result = rational(1);
        while (count) { if (count & 1) result = product(result, base); count >>= 1; if (count) base = product(base, base); }
        return result;
    }
    const long double b = decimal(base), e = decimal(exponent);
    if (b < 0) fail("Puissance non entière d’un nombre négatif : résultat non réel.");
    if (b == 0 && e <= 0) fail("Puissance de zéro non définie.");
    return approx(std::pow(b, e));
}
BigInt integerRoot(const BigInt &number, int degree) {
    if (number <= 1) return number;
    // Binary search with at most 4096 steps; the AST/bit budget bounds all work.
    BigInt low = 0, high = BigInt(1) << ((bits(number) + degree - 1) / degree);
    while (low + 1 < high) {
        BigInt middle = (low + high) / 2;
        BigInt p = middle * middle; if (degree == 3) p *= middle;
        if (p <= number) low = middle; else high = middle;
    }
    BigInt highPower = high * high; if (degree == 3) highPower *= high;
    return highPower == number ? high : low;
}
Value rootValue(const Value &value, int degree) {
    const bool negative = value.exact ? value.numerator < 0 : value.approximate < 0;
    if (negative && degree == 2) fail("Racine carrée d’un nombre négatif : résultat non réel.");
    if (value.exact) {
        const BigInt n = absInt(value.numerator), d = value.denominator;
        BigInt nr = integerRoot(n, degree), dr = integerRoot(d, degree);
        BigInt np = nr * nr, dp = dr * dr;
        if (degree == 3) { np *= nr; dp *= dr; }
        if (np == n && dp == d) return rational(negative ? -nr : nr, dr);
    }
    return approx(degree == 2 ? std::sqrt(decimal(value)) : std::cbrt(decimal(value)));
}
class Evaluator {
public:
    explicit Evaluator(const EvalContext &context) : context(context) {}
    Value run(const NodePtr &node) {
        if (++operations > 2048) fail("Expression trop complexe.");
        switch(node->kind) {
        case Kind::Row: { int position = 0; Value result = sum(node, position); if (position != node->children.size()) fail("Opérateur ou expression incomplet."); return result; }
        case Kind::Number: return parseNumber(node->value);
        case Kind::Symbol:
            if (node->value == "pi") return approx(std::acos(-1.L));
            if (node->value == "e") return approx(std::exp(1.L));
            if (node->value == "Ans") { if (!context.hasAnswer) fail("Aucun résultat précédent (Ans)."); return context.answer; }
            if (node->value == "M") { if (!context.hasMemory) fail("La mémoire est vide."); return context.memory; }
            fail("Symbole inconnu.");
        case Kind::Fraction: return divide(run(node->children[0]), run(node->children[1]));
        case Kind::Power: return power(run(node->children[0]), run(node->children[1]));
        case Kind::Root: return rootValue(run(node->children[0]), node->value.toInt());
        case Kind::Group: return run(node->children[0]);
        case Kind::Function: return function(node->value, run(node->children[0]));
        case Kind::Operator: fail("Expression incomplète.");
        }
        fail("Expression inconnue.");
    }
private:
    const EvalContext &context;
    int operations = 0;
    Value sum(const NodePtr &row, int &p) {
        Value result = term(row, p);
        while (p < row->children.size()) {
            auto next = row->children[p];
            if (next->kind != Kind::Operator || (next->value != "+" && next->value != "-")) break;
            ++p; result = add(result, term(row, p), next->value == "-");
        }
        return result;
    }
    Value term(const NodePtr &row, int &p) {
        Value result = unary(row, p);
        while (p < row->children.size()) {
            auto next = row->children[p];
            if (next->kind == Kind::Operator) {
                if (next->value != "*" && next->value != "/") break;
                ++p; auto rhs = unary(row, p); result = next->value == "*" ? product(result, rhs) : divide(result, rhs);
            } else result = product(result, unary(row, p)); // Natural implicit multiplication: 2π or 2(3).
        }
        return result;
    }
    Value unary(const NodePtr &row, int &p) {
        if (p >= row->children.size()) fail("Complétez les cases vides et les opérateurs.");
        auto n = row->children[p++];
        if (n->kind == Kind::Operator) {
            if (n->value == "-") return negate(unary(row, p));
            if (n->value == "+") return unary(row, p);
            fail("Opérateur sans nombre à gauche.");
        }
        return run(n);
    }
    Value function(const QString &name, const Value &value) {
        if (name == "abs") return (value.exact ? value.numerator < 0 : value.approximate < 0) ? negate(value) : value;
        const long double x = decimal(value), pi = std::acos(-1.L), scale = context.degrees ? pi / 180.L : 1.L;
        if (name == "sin" || name == "cos" || name == "tan") {
            if (std::fabs(x) > 1e12L) fail("Angle trop grand pour une approximation fiable.");
            if (name == "sin") return approx(std::sin(x * scale));
            if (name == "cos") return approx(std::cos(x * scale));
            if (std::fabs(std::cos(x * scale)) < 1e-15L) fail("Tangente non définie pour cet angle.");
            return approx(std::tan(x * scale));
        }
        if (name == "asin" || name == "acos") {
            if (x < -1 || x > 1) fail("Argument attendu entre −1 et 1.");
            return approx((name == "asin" ? std::asin(x) : std::acos(x)) / scale);
        }
        if (name == "atan") return approx(std::atan(x) / scale);
        if (name == "ln" || name == "log") {
            if (x <= 0) fail("Le logarithme exige un nombre strictement positif.");
            return approx(name == "ln" ? std::log(x) : std::log10(x));
        }
        if (name == "exp") return approx(std::exp(x));
        fail("Fonction inconnue.");
    }
};
QString integerText(const BigInt &number) { return QString::fromStdString(number.convert_to<std::string>()); }
}
QJsonObject Value::json() const {
    if (exact) return {{"exact", true}, {"n", integerText(numerator)}, {"d", integerText(denominator)}};
    return {{"exact", false}, {"value", QString::number(double(approximate), 'g', 17)}};
}
bool Value::read(const QJsonObject &json, Value &value) {
    try {
        if (!json.value("exact").isBool()) return false;
        if (json.value("exact").toBool()) {
            static const QRegularExpression number("^-?[0-9]{1,1234}$"), denominator("^[0-9]{1,1234}$");
            auto n = json.value("n").toString(), d = json.value("d").toString();
            if (!number.match(n).hasMatch() || !denominator.match(d).hasMatch()) return false;
            value = rational(parseInteger(n), parseInteger(d));
        } else {
            bool ok; double d = json.value("value").toString().toDouble(&ok); if (!ok) return false;
            value = approx(d);
        }
        return true;
    } catch (...) { return false; }
}
QString approximateValue(const Value &value, int precision) {
    try {
        const auto n = decimal(value);
        if (std::fabs(n) > std::numeric_limits<double>::max() || (n != 0 && double(n) == 0)) return {};
        auto text = QString::number(double(n), 'g', std::clamp(precision, 6, 15));
        text.replace('.', ','); return "≈ " + text;
    } catch (...) { return {}; }
}
QString formatValue(const Value &value, int precision) {
    if (!value.exact) return approximateValue(value, precision);
    return "= " + integerText(value.numerator) + (value.denominator == 1 ? "" : " / " + integerText(value.denominator));
}
EvalResult evaluate(const NodePtr &root, const EvalContext &context) {
    EvalResult result;
    try {
        if (!validTree(root, &result.error)) return result;
        Evaluator evaluator(context); result.value = evaluator.run(root); result.ok = true;
        result.display = formatValue(result.value, context.precision);
        result.approximation = result.value.exact && result.value.denominator != 1 ? approximateValue(result.value, context.precision) : QString{};
        if (result.value.exact) result.latex = result.value.denominator == 1 ? integerText(result.value.numerator) : "\\frac{" + integerText(result.value.numerator) + "}{" + integerText(result.value.denominator) + "}";
        else result.latex = "\\approx " + QString::number(double(result.value.approximate), 'g', std::clamp(context.precision, 6, 15));
    } catch (const Failure &failure) { result.error = failure.message; }
    catch (const std::exception &) { result.error = "Limite de calcul atteinte."; }
    return result;
}
}
