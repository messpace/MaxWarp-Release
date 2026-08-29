#include "RateDriver.h"

#include <algorithm>
#include <cmath>

namespace maxwarp {

namespace {

bool nearRate(double a, double b)
{
    return std::fabs(a - b) <= 1e-6 * std::max(std::fabs(a), std::fabs(b));
}

// Sim rates move in powers of two; a runaway loop can never owe more than the
// full 0.25–128 span.
constexpr int kMaxSteps = 12;

// Whole rate steps from `a` up to `b` — positive when `b` is the faster rate.
// Rates move in powers of two (T2/T9), so anything that is not a clean step
// apart is not something the driver's own stepper could have produced: it
// answers 0, which every caller reads as "not ours".
int stepDistance(double a, double b)
{
    if (a <= 0.0 || b <= 0.0)
        return 0;
    const double d = std::log2(b / a);
    const int n = static_cast<int>(std::lround(d));
    if (std::fabs(d - static_cast<double>(n)) > 1e-3)
        return 0;
    return n;
}

} // namespace

bool RateDriver::reconcile(double observedRate, bool uiStepped)
{
    if (m_first) {
        // Whatever the sim wakes at is the human's standing choice, not a press.
        m_first = false;
        m_expected = m_cruise = observedRate;
        return false;
    }

    const double prior = m_expected;
    const bool moved = !nearRate(observedRate, prior);
    m_expected = observedRate; // the sim's answer is the truth either way

    if (m_holding) {
        // T41 (#60): the ceiling is a ceiling. A rate above it — the human's
        // +, the sim's own key, a DECR that read back short — is never
        // adopted: drive() steps it back down and the cruise rate stands. A
        // rate *below* the turn rate is the human lowering, which is always
        // safe: it becomes the cruise rate, and the restore never exceeds it.
        //
        // But only if it is *theirs*. The sim answers rate events late, short
        // and sometimes doubled (T37/T40), so a readback below the ceiling can
        // just as easily be the driver's own DECRs landing. Outstanding steps
        // are the discriminator: anything they explain is never a human rate
        // change. That is what keeps a raised Turn rate setting — which lifts
        // the ceiling clear above the rate the governor is already holding —
        // from reading the governor's own held rate as the human choosing it.
        if (!moved) {
            // The sim agrees with the expectation: everything transmitted has
            // either landed or been swallowed, so nothing is outstanding.
            m_owedDown = m_owedUp = 0;
            return false;
        }
        const int below = -stepDistance(prior, observedRate);
        if (below > 0 && below <= m_owedDown) {
            m_owedDown -= below; // our own DECRs, arriving late or doubled
            return false;
        }
        if (m_ceiling <= 0.0)
            return false; // no ceiling to judge against: fail closed (never adopt)
        if (observedRate < prior * (1.0 - 1e-6) && observedRate < m_ceiling * (1.0 - 1e-6)) {
            m_cruise = observedRate;
            return true;
        }
        return false;
    }

    if (m_restoring) {
        // The restore is owed until the readback confirms it. Same rule: a
        // shortfall our own outstanding steps explain is ours — drive() sends
        // the rest — while anything they cannot explain, in particular a rate
        // moving *away* from the cruise rate, is the human, and their choice
        // ends the restore.
        if (nearRate(observedRate, m_cruise)) {
            m_restoring = false;
            m_restoreTicks = 0;
            m_owedUp = m_owedDown = 0;
            return false;
        }
        const int need = stepDistance(observedRate, m_cruise);
        if (need > 0 && need <= m_owedUp)
            return false; // our INCRs still landing
        if (need < 0 && -need <= m_owedDown)
            return false; // our DECRs still landing
        m_restoring = false;
        m_restoreTicks = 0;
        m_owedUp = m_owedDown = 0;
        m_cruise = observedRate;
        return true;
    }

    // Outside a window every change the driver did not command is the human's:
    // adopt it as the cruise rate. `uiStepped` is authoritative here even when
    // presses net out to the same rate.
    m_cruise = observedRate;
    return moved || uiStepped;
}

RateSteps RateDriver::drive(bool haveCeiling, double rateCeiling)
{
    RateSteps s;
    if (haveCeiling) {
        // Only ever lower toward the ceiling — a ceiling above the rate asks
        // nothing, and nothing here ever steps up while a window is open. Runs
        // every tick, so a rate that crept above the ceiling is re-lowered;
        // because reconcile() has already synced m_expected to the readback,
        // the loop re-sends the shortfall, not the whole burst.
        if (!m_holding) { // the window opens: nothing from before is owed
            m_owedDown = m_owedUp = 0;
            m_restoring = false;
            m_restoreTicks = 0;
        }
        m_holding = true;
        m_ceiling = rateCeiling;
        while (m_expected > rateCeiling * (1.0 + 1e-6) && s.down < kMaxSteps) {
            m_expected /= 2.0;
            ++s.down;
        }
        m_owedDown = std::min(m_owedDown + s.down, kMaxSteps);
    } else if (m_holding || m_restoring) {
        // Restore: the window closed, so the cruise rate comes back — to it
        // exactly, never above. A rate the ceiling clamped *up* to is given
        // back the same way, so the step runs in whichever direction the
        // cruise rate lies (only one of the two loops can ever turn). It runs
        // again every tick until reconcile() sees the readback confirm the
        // cruise rate, bounded so a sim that never answers cannot spin.
        m_holding = false;
        if (m_restoreTicks >= kMaxSteps) {
            m_restoring = false;
            return s;
        }
        while (m_expected < m_cruise * (1.0 - 1e-6) && s.up < kMaxSteps) {
            m_expected *= 2.0;
            ++s.up;
        }
        while (m_expected > m_cruise * (1.0 + 1e-6) && s.down < kMaxSteps) {
            m_expected /= 2.0;
            ++s.down;
        }
        m_owedUp = s.up;
        m_owedDown = s.down;
        m_restoring = s.up > 0 || s.down > 0;
        m_restoreTicks = m_restoring ? m_restoreTicks + 1 : 0;
    }
    return s;
}

} // namespace maxwarp
