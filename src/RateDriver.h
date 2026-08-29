#pragma once

// MaxWarp rate driver (T39 · issue #58) — the pure, Qt-free wiring logic
// between the turn governor's rate *ceiling* and the sim-rate stepper. The
// governor core (T35) never touches the rate; this is the piece that does:
// it steps down toward the ceiling while a window is open, remembers the
// human's cruise rate, and hands it back on restore — never raising above it.
//
// It is also the arbiter: outside a window, every rate change the driver did
// not command is the human's and becomes the cruise rate. Inside a window the
// ceiling is a ceiling (T41 · #60): the driver tracks the rate its own steps
// imply, and a readback *above* the turn rate — the UI's +, the sim's own key,
// a DECR that answered a step short — is stepped back down on the next tick
// rather than adopted; the cruise rate it will restore to is untouched. A
// readback *below* the turn rate is the human lowering, which is always safe:
// it becomes the new cruise rate, and the restore never exceeds it. Nothing
// stands the governor down any more; the escape from a window is the governor
// switch.
//
// The one discriminator throughout is outstanding steps: the driver counts the
// events it has itself transmitted and not yet seen land, and a readback those
// explain is never read as the human — not a burst the sim answers doubled,
// not the held rate left below a Turn rate setting the human just raised, not
// a restore the sim answers short. Everything else is theirs, whichever key
// they used.
//
// Per tick, in order: reconcile(observed, uiStepped), then TurnGovernor::onTick,
// then drive(decision) -> stepper events to send.

namespace maxwarp {

struct RateSteps {
    int down = 0; // SIM_RATE_DECR events to transmit this tick
    int up = 0;   // SIM_RATE_INCR events to transmit this tick
};

class RateDriver {
public:
    // Before the governor's tick: compare the observed rate with the
    // expectation and sync to it. `uiStepped` = the app's own stepper was
    // pressed since the last tick; it is authoritative *outside* a window,
    // even when presses net out to the same rate. Inside a window it is not
    // read at all: what decides there is the readback against the ceiling and
    // against the driver's own outstanding steps, so that the sim's own rate
    // keys count exactly as much as the app's.
    //
    // Returns true when the human moved the cruise rate. This is a test and
    // diagnostic signal only: since T41 the governor core no longer hears the
    // human, so no production code acts on it (SimConnectWorker discards it).
    bool reconcile(double observedRate, bool uiStepped);

    // After the governor's tick: the stepper events this tick owes. While a
    // ceiling stands, steps down toward it (never up) — every tick, so a rate
    // that crept above it is re-lowered; when it lifts, steps toward the
    // cruise rate — up from a held rate, down from one the ceiling clamped —
    // and keeps stepping until the readback confirms it.
    RateSteps drive(bool haveCeiling, double rateCeiling);

    // Back to the constructed state — no ceiling, no cruise rate, no owed
    // restore, and `m_first` again, so the next reconcile adopts whatever rate
    // the sim wakes at exactly like first boot. Called when the sim connection
    // drops (SimConnectWorker::closeSim): a window left open by a disconnect
    // must not fire its restore into a fresh flight at the gate.
    void reset() { *this = RateDriver{}; }

    // The governor let go mid-window: drop the hold and the owed restore
    // without transmitting anything, and take the rate we are sitting at as
    // the human's cruise rate. Called when the turn governor is switched off
    // while a window is open — the switch is the escape from a window, and it
    // takes effect at the moment it is thrown, not at the next live tick. The
    // rate is deliberately left where the governor put it: a restore burst
    // here could fire into a menu or a loading screen (the sim reports both as
    // paused), and a governor on its way out should not be transmitting. The
    // cost is accepted and one-way — the rate the window was holding becomes
    // the cruise rate, so switching back on does not recover the old one.
    void release()
    {
        m_holding = false;
        m_restoring = false;
        m_owedDown = 0;
        m_owedUp = 0;
        m_restoreTicks = 0;
        m_ceiling = 0.0;
        m_cruise = m_expected;
    }

    // The human's last-chosen rate — what a restore returns to, and what the
    // hero's held state names ("returns to 8×").
    double cruiseRate() const { return m_cruise; }
    // A window stepped the rate down and the restore is still owed.
    bool holding() const { return m_holding; }

private:
    bool m_first = true;
    double m_expected = 1.0; // the rate if only the driver has acted
    double m_cruise = 1.0;
    bool m_holding = false;
    // The turn rate the open window binds to (T41). Written only by drive(),
    // in the same breath as m_holding; read only by reconcile(), on the *next*
    // tick, and only while m_holding — so the 0.0 sentinel is unreachable
    // there. reconcile() still guards it and fails closed (adopts nothing)
    // rather than comparing a rate against 0.0.
    double m_ceiling = 0.0;
    // Steps the driver has transmitted and not yet seen land. The sim answers
    // rate events late, short and sometimes doubled (T37/T40), so a readback
    // these explain is the driver's own work, never a human rate change.
    int m_owedDown = 0;
    int m_owedUp = 0;
    bool m_restoring = false;  // a restore is in flight, unconfirmed
    int m_restoreTicks = 0;    // retries spent on it, bounded so it cannot spin
};

} // namespace maxwarp
