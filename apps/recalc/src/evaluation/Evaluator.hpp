#pragma once
#include "expression/Ast.hpp"
#include <boost/multiprecision/cpp_int.hpp>

namespace recalc {
using BigInt = boost::multiprecision::cpp_int;
constexpr unsigned MaxIntegerBits = 4096;
struct Value {
    bool exact = true;
    BigInt numerator = 0;
    BigInt denominator = 1;
    long double approximate = 0;
    QJsonObject json() const;
    static bool read(const QJsonObject &json, Value &value);
};
struct EvalContext {
    bool degrees = true;
    int precision = 12;
    Value answer, memory;
    bool hasAnswer = false;
    bool hasMemory = false;
};
struct EvalResult {
    bool ok = false;
    Value value;
    QString display;
    QString approximation;
    QString latex;
    QString error;
};
EvalResult evaluate(const NodePtr &root, const EvalContext &context = {});
QString formatValue(const Value &value, int precision = 12);
QString approximateValue(const Value &value, int precision = 12);
}
