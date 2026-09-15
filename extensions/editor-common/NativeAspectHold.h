#pragma once
#include <QPointF>
#include <QtGlobal>
#include <cmath>

// Per-gesture hold decision, with no clock or input ownership. Before advancing
// the timer, callers must synchronously flush buffered input through move(),
// including every intermediate point. All calls use the same nonnegative
// monotonic millisecond clock and view-space pixel coordinates.
class NativeAspectHold final {
public:
    enum class Phase { Off, Waiting, Cancelled, Locked };
    static constexpr qint64 HoldDurationMs = 1000;
    static constexpr qreal MovementTolerance = 8;

    Phase phase() const { return m_phase; }
    bool waiting() const { return m_phase == Phase::Waiting; }
    bool locked() const { return m_phase == Phase::Locked; }

    bool begin(const QPointF &viewPoint, qint64 timeMs) {
        reset();
        if (!finite(viewPoint) || timeMs < 0) {
            cancel();
            return false;
        }
        m_origin = viewPoint;
        m_startedMs = m_lastMs = timeMs;
        m_phase = Phase::Waiting;
        return true;
    }

    // Movement never activates the lock. Until advance() does so, any excursion
    // cancels the attempt, even after the deadline or when a later point returns
    // to the origin. Once locked, valid movement is unrestricted.
    void move(const QPointF &viewPoint, qint64 timeMs) {
        if (!active()) return;
        if (!finite(viewPoint) || !acceptTime(timeMs)) {
            cancel();
            return;
        }
        if (waiting() && std::hypot(viewPoint.x() - m_origin.x(),
                                   viewPoint.y() - m_origin.y()) > MovementTolerance)
            cancel();
    }

    // A timer calls this even when no move arrives, so a stationary contact
    // locks. Repeated calls never report the same transition twice.
    bool advance(qint64 timeMs) {
        if (!active()) return false;
        if (!acceptTime(timeMs)) return false;
        return lockIfDue(timeMs);
    }

    void cancel() { m_phase = Phase::Cancelled; }
    void reset() {
        m_phase = Phase::Off;
        m_origin = {};
        m_startedMs = m_lastMs = 0;
    }

private:
    static bool finite(const QPointF &point) {
        return std::isfinite(point.x()) && std::isfinite(point.y());
    }
    bool active() const { return waiting() || locked(); }
    bool acceptTime(qint64 timeMs) {
        if (timeMs < 0 || timeMs < m_lastMs) {
            cancel();
            return false;
        }
        m_lastMs = timeMs;
        return true;
    }
    bool lockIfDue(qint64 timeMs) {
        // Both values are nonnegative and ordered, avoiding deadline-addition
        // overflow even near the largest representable monotonic timestamp.
        if (!waiting() || timeMs - m_startedMs < HoldDurationMs) return false;
        m_phase = Phase::Locked;
        return true;
    }

    Phase m_phase = Phase::Off;
    QPointF m_origin;
    qint64 m_startedMs = 0;
    qint64 m_lastMs = 0;
};
