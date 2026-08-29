#pragma once

// MaxWarp corrector core (T8 · issue #9) — the pure, Qt-free, SimConnect-free
// implementation of the locked T3 algorithm (#4) under the T4 timing spec (#5),
// with the T10 policy hooks (#11). This is the headless seam T4 called for: the
// whole per-tick pipeline is `onTick(TickInput) -> TickDecision`, a function of
// explicit state, unit-tested with no sim in the loop. SimConnectWorker adapts
// live telemetry into TickInput and issues the returned writes.
//
// T21 (issue #24) ported it from gallons-on-simvars to **kg on core state**. The
// algorithm did not change — T18 (#20) established that uniform per-tank
// amplification incl. trim survives intact, because the ini core takes 0.1 kg
// L:var writes on every tank (T17, #19). What changed is units, sources, and one
// genuine bug:
//
//   * Fuel quantities are kg from `INI_FUEL_WEIGHT_*` — the aircraft's own state,
//     reached through the bridge. The legacy `FUEL TANK <NAME> QUANTITY` simvars
//     are a mirror the core re-asserts ~1 Hz (T15, #17) and are never read here.
//     Trim in particular must never come from `EXTERNAL1`, whose phantom ±1.14 kg
//     1 Hz square wave would have the corrector chasing error that isn't there.
//   * Writes are signed per-tank kg deltas, applied in-sim by the bridge's atomic
//     read-modify-write. A delta, not a target: computing `target - observed`
//     out here would reintroduce exactly the client-side race the in-sim RMW
//     exists to avoid.
//   * The max-burn clamp references the core's own cumulative burn integrator
//     rather than `ENG FUEL FLOW PPH`, which under-reports by a ratio that is not
//     even constant across variants (T17: 1.3555 on the -200 vs 1.398 on the
//     -300, so no fixed factor patches it).
//   * `baseline()` anchors to what we **commanded**, never to what we observed.
//     Observed-anchoring re-amplified our own surviving writes and produced 12 of
//     T13's (#14) 21 spurious clamp trips.
//
// T24 (issue #34) carries T23's (#31) decisions: **the pairing is with an
// airframe, and the variant is an observation of it**. `TankReading::capacityKg`
// used to answer two incompatible questions at once — how big is this tank
// (structural) and can I read it right now (the bridge's) — so a re-establish,
// which T20 (#23) proved a flight load forces, zeroed every capacity, failed the
// A330 fingerprint, ended the pairing and wiped the session counters mid-flight.
// Splitting `readable` out of capacity dissolves that, and the pairing now keys
// on TITLE rather than on the variant, so a detection gap holds the pairing
// instead of ending it and a same-variant swap is caught for the first time.

#include <string>
#include <vector>

namespace maxwarp {

// The single user-facing control (T10): AUTO arms the corrector to engage
// itself; OFF is the sticky disengaged state.
enum class MasterSwitch { Auto, Off };

// Corrector state exposed to the UI (T10 fixed vocabulary):
//   Off        — master switch OFF (no writes, sticky until human re-arm)
//   Standby    — armed, but an engage precondition is unmet (no bridge, no
//                A330, R<=1, data stall)
//   Correcting — armed, engaged, actively writing amplified deltas this tick
//   Suspended  — armed and R>1, but a safety gate is holding writes this tick
enum class CorrectorState { Off, Standby, Correcting, Suspended };

// Structural role of a tank slot, set by the caller from the fixed slot order
// (T1/T7 tank map). Keeps the core variant-agnostic: detection and clamp floors
// key off role + capacity, never variable names.
enum class TankRole { Main, Aux, Center, Trim };

struct TankReading {
    TankRole role = TankRole::Main;
    // Core state for this slot: INI_FUEL_WEIGHT_<TANK>, in kg (T17/T18). This is
    // the authority the corrector reads and writes — not the legacy simvar. Only
    // meaningful when `readable` below is set: an unresolved name reads a flat 0,
    // which is a claim about nothing (T17).
    double kg = 0.0;
    // How much this slot holds when full, in kg — a structural fact about the
    // *airframe*, published whether the tank is brimmed or bone dry, and readable
    // with no bridge at all. 0 means the airframe does not have this tank (CENTER
    // on the -300). It is the ceiling of the per-tank clamp, and the ground of
    // both the A330 fingerprint and the variant, precisely because none of those
    // should move when fuel does.
    double capacityKg = 0.0;
    // Whether the bridge has core state for this slot right now — the *bridge's*
    // answer where capacity is the airframe's (T23 · issue #31). The two were one
    // field until T24 (#34), which made a re-establish (zeroing every capacity)
    // read as "not an A330", ending the pairing and wiping the session counters
    // mid-flight on any variant. Only what is readable may be corrected, netted or
    // counted; what *exists* is a separate question.
    //
    // Defaults true for the same reason `TickInput::bridgeUp` does: it describes
    // the caller's transport, not the algorithm. The pure-algorithm tests have no
    // bridge to speak of; the live worker sets it explicitly every tick.
    bool readable = true;
};

// Aircraft identity for the detection fingerprint (T3 GATE 1).
struct AircraftId {
    std::string title;           // TITLE / ATC MODEL
    bool legacyFuelSystem = false; // NEW FUEL SYSTEM == 0
};

// One arriving telemetry packet == one tick (T4). The rate and aircraft fields
// are sampled atomically in a single SimConnect data definition; the core-state
// fields are the bridge's latest block, latched into that packet (T18 keeps the
// 1 Hz SimConnect packet as the tick even though core state arrives on its own
// clock — commanded anchoring makes the phase difference self-cancelling).
struct TickInput {
    double dtWall = 0.0;   // steady_clock seconds since the previous arrival
    double simRate = 1.0;  // SIMULATION RATE (R_now)
    std::vector<TankReading> tanks;
    // The core's own cumulative burn integrator, INI_ENG1_FUEL_USED +
    // INI_ENG2_FUEL_USED (kg). T17 matched it to observed drain to 0.03%, which
    // is why it — and not fuel flow — is the max-burn clamp's reference. A
    // difference of a cumulative integrator also tolerates a variable dt for
    // free. When it is unavailable the clamp self-disables rather than freezing
    // the corrector, exactly as the fuel-flow reference did.
    double coreFuelUsedKg = 0.0;
    bool haveCoreFuelUsed = false;
    // INI_EFB_IS_REFUELING — makes the conservation gate exact instead of
    // inferring a refuel from a net fuel gain. The inference stays on as a
    // backstop for defuelling paths the flag does not cover.
    bool coreRefueling = false;
    bool paused = false;   // pause / active-pause / menu / loading
    bool slew = false;     // IS SLEW ACTIVE
    AircraftId aircraft;
    // Fourth engage precondition (T20 · issue #23). Core state is reachable only
    // through the in-sim bridge, so no bridge means no correcting — MaxWarp
    // declines to engage rather than falling back to a second write path (T18).
    // Defaults true because the field describes the *caller's* transport, not the
    // algorithm: the pure-algorithm tests have no bridge to speak of and are not
    // gated by one. The live worker sets it explicitly from bridge health every
    // tick and never relies on the default.
    bool bridgeUp = true;
    // Note: the master switch is sticky internal state (arm()/disengage()), not
    // a per-tick input — a human toggles it, telemetry never does.
};

struct TankWrite {
    int tankIndex = 0;     // index into TickInput::tanks
    // Signed kg to apply to core state; negative removes fuel. The bridge turns
    // this into one atomic in-sim RMW, so nothing here races the core's own 1 Hz
    // integration step.
    double deltaKg = 0.0;
    // The clamped value this delta was computed to reach. It is what the next
    // tick anchors to (commanded, never observed) and what the session log
    // records as commanded.
    double targetKg = 0.0;
};

// Session counters (T10). Scoped to one A330 pairing; cleared at the start of
// the next pairing (lingering-stats rule), never by disengage.
//
// T19 (#21) split removal into what was **commanded** and what was **realised**,
// because for the whole of T13's (#14) cruise those were 1133 gal and ~zero and
// the screen showed the former. Nothing here is named plainly "removed" any
// more: a counter that has to be read carefully to avoid that mistake is the
// mistake. `burnObservedKg`/`burnEffectiveKg` stay commanded figures — they
// describe the algorithm's arithmetic on the tick, and T16 (#18) made
// `effective == R_eff x observed` hold exactly in them.
struct SessionCounters {
    // The airframe this pairing is *with*, as the sim names it (TITLE). This is
    // the pairing's identity (T23 · issue #31): a pairing ends when this changes
    // and at no other time, so losing sight of the aircraft holds the pairing
    // rather than ending it, and the same airframe coming back resumes it. Empty
    // until the first lock. It catches a same-variant swap, which a variant key
    // cannot see at all.
    std::string title;
    // Whether the fingerprint is locked *this tick*. It drives the rail's
    // SEARCHING/<variant> word and nothing else — it is emphatically not the
    // pairing's life, and no counter clears on its edge.
    bool active = false;
    // Which A330 we are paired with, read from the capacities the airframe
    // publishes. An observation *about* the pairing, never its identity, so it is
    // re-read every tick and adopted in place: TITLE and the capacities arrive on
    // separate SimConnect streams, and a swap shows the new capacities under the
    // old title for a tick or two. Latching would freeze that race into the
    // pairing.
    std::string variant;         // e.g. "A330-300"
    // Wall time since the pairing locked. Ticks regardless of the master switch,
    // and continues across a gap in which the aircraft went unseen — the pairing
    // is still live through one.
    double elapsedSeconds = 0.0;
    double removedCommandedKg = 0.0; // extra fuel the corrector asked the sim to remove
    double burnObservedKg = 0.0;     // natural depletion seen (R=1 equivalent)
    double burnEffectiveKg = 0.0;    // amplified depletion applied (commanded)

    // Realised removal (T19): the tanks' measured departure from their natural-
    // drain trajectory, over the intervals where that departure is measurable.
    // Derived from core state and the core's own burn integrator alone — never
    // from anything the corrector commanded — so it cannot inherit the
    // corrector's own optimism about whether a write landed.
    double removedRealisedKg = 0.0;
    // The commanded removal covering exactly those same measured intervals, so
    // `realised / measured` is an apples-to-apples write-fidelity ratio and
    // `measured / commanded` is how much of the session that ratio speaks for.
    // Without it a voided interval (refuel, pause, long frame) would be
    // indistinguishable from a lost write.
    double removedMeasuredKg = 0.0;

    // The intervals realised removal could *not* be measured across, and the
    // commanded kg they cover (T25 · issue #37). T19 defined voiding but counted
    // nothing, so `commanded - measured` was a gap with no name — and T12 (#13)
    // reported "0 voided" by inferring it from a figure nothing counts, on a run
    // whose one voided interval accounted for the whole 11.029 kg gap. With these
    // the identity closes: commanded = measured + voided.
    long voidedIntervals = 0;
    double voidedKg = 0.0;
};

struct TickDecision {
    CorrectorState state = CorrectorState::Off;
    std::vector<TankWrite> writes;
    std::vector<std::string> events; // human-readable transitions/gate trips
    SessionCounters counters;
    double rEff = 0.0;   // R_eff actually used this tick (0 if not engaged)
    bool reBaselined = false; // this tick discarded/suspended and re-anchored baseline
    // Per-slot natural delta this tick (kg, index-aligned to TickInput::tanks).
    // The engaged loop's delta vs the commanded anchor is the aircraft's own
    // burn — core state after a write includes the correction, so the UI can't
    // recover this itself. Empty on any tick that didn't reach delta computation.
    std::vector<double> observedDeltaKg;
};

// Tuning constants, set against live accelerated data by T16 (#18) — the kg
// figures T21 carried across from gallons were placeholders sized for simvar
// noise on a path the corrector no longer uses.
//
// The governing measurement is that **core state carries no noise worth a
// threshold**. Over 825 ticks of a live -300 every quiescent tank read a delta
// of exactly 0.000 kg: a value that does not change arrives as the same float32
// bits, so it differences to zero. The only real floor is float32 transport
// quantisation on a tank that *is* moving — one ULP, 0.0039 kg on a brim-full
// main (T17 measured ~0.002 kg at 20,000 kg, which is that same ULP).
//
// Both magnitude thresholds below were therefore two orders too coarse, and both
// failed the same way: an absolute kg threshold discriminating a signal that
// scales with fuel flow. Sized for cruise, they break at idle descent — the
// deadband by swallowing the whole correction, the gain epsilon by letting a
// lost write pass as a real gain, at which point the corrector commands fuel
// *added*. Sized against the ULP instead, both hold at any flow.
struct CorrectorConfig {
    double engageThreshold = 1.0;    // engage only when R_now > this
    // ε_noise — per-tank |delta| floor. 2.6x the worst-case float32 ULP, and ~70x
    // below a cruise tick's per-main drain (0.73 kg). At the old 0.25 kg the
    // corrector silently stopped correcting below ~34% of cruise fuel flow, i.e.
    // for the whole of an idle descent, while still reporting CORRECTING.
    double noiseDeadbandKg = 0.01;
    // ε_gain — net system gain => refuel/lost write. In flight the net is always
    // negative (fuel only leaves), so this only has to clear the summed ULP of
    // six tanks (~0.024 kg). Live: healthy ticks never came within 1.476 kg of
    // zero, while the four genuine lost writes read +2.955 to +9.027 kg.
    double gainEpsilonKg = 0.1;
    // max |total correction| = margin × expBurn. A healthy correction is
    // structurally (R-1)/R of expected burn — under 1.0 at every rate, measured
    // at 0.877 max on an 8x cruise with 0.15% spread — so the margin's real job
    // is to clear the excursions no capture has measured yet, chiefly APU draw,
    // which comes out of a main but is absent from INI_ENG*_FUEL_USED and can
    // push a low-flow tick to ~1.15. A jettison ratios around 21.
    double clampMargin = 1.5;
    // Residue tolerance — how far the write set may sum from the net it intends
    // before the corrector says so. Its own constant since T28 (#43): the check
    // used to borrow the per-tank deadband, and the two are arithmetically
    // guaranteed to collide, because a deadband skip is *exactly* what put the
    // residue there. That is fixed at the reference now (see decide()), so what
    // is left for this to clear is double-precision arithmetic over six tanks —
    // hence a figure three orders below the deadband rather than equal to it.
    double residueToleranceKg = 0.001;
    double maxDtSeconds = 5.0;       // dt cap — discard + re-baseline beyond this
    // Consecutive engaged ticks that may burn fuel while writing nothing before
    // the corrector says so out loud. See the deadband-silence guard in decide().
    int deadbandSilenceTicks = 5;
    // Reconciliation (T25 · issue #37) — on in the app, always, and never a
    // tuning question. It exists as a switch for the same reason the thresholds
    // above are overridable: the acceptance case is a *capture*, and the only way
    // to keep "exit 1 before the fix, exit 0 after" reproducible once the fix has
    // landed is to be able to ask the harness for the old output. Nothing but
    // MaxWarpReplay's `--no-reconcile` ever sets it false.
    bool reconcileDeficit = true;
    // The one-way clamp (T28 · issue #43) — on in the app, always, and a *rule*
    // rather than a constant, which is what let `trimFloorKg` be deleted outright
    // instead of deepened. Same affordance as `reconcileDeficit` above and for
    // the same reason: only MaxWarpReplay's `--no-backward-clamp` ever sets it
    // false, so "exit 1 before the fix, exit 0 after" stays reproducible from a
    // capture once the fix has landed.
    bool oneWayClamp = true;
};

class Corrector {
public:
    Corrector() = default;
    explicit Corrector(const CorrectorConfig &config) : m_config(config) {}

    // Run the full per-tick pipeline. Pure w.r.t. the sim: same inputs +
    // internal state => same decision.
    TickDecision onTick(const TickInput &in);

    // Master switch control (T10). DISENGAGE => sticky OFF; only this (a human
    // action) re-arms.
    void arm();       // AUTO
    void disengage(); // OFF, sticky
    MasterSwitch masterSwitch() const { return m_master; }

    const SessionCounters &counters() const { return m_counters; }

private:
    // Session lifecycle (T10), independent of the master switch.
    std::string detectVariant(const TickInput &in) const;
    void updateSession(bool detected, const TickInput &in, TickDecision &d);
    // Realised-removal accounting (T19). Measures the interval that has just
    // elapsed, so it runs before any gate: what the corrector is about to decide
    // cannot change what the aircraft already did. Purely observational — it
    // takes `d` only to speak, never to steer.
    void accountRealised(bool detected, const TickInput &in, TickDecision &d);
    // The write-decision pipeline (T3 gates + amplification). Sets state/writes/
    // rEff/reBaselined on `d` and accumulates the correcting-tick counters.
    void decide(bool detected, const TickInput &in, TickDecision &d);

    CorrectorConfig m_config;
    MasterSwitch m_master = MasterSwitch::Auto;
    SessionCounters m_counters;
    bool m_sessionActive = false;

    // Event emission (T10): a message fires only when the corrector state
    // changes, tagged with the reason the current branch recorded.
    CorrectorState m_lastState = CorrectorState::Standby;
    std::string m_reason;

    // Engaged-loop anchor (T3). Maintained only while engaged; re-anchored to
    // observed core state on every engage transition and after any discarded or
    // suspended tick, and to the *commanded* value after every correcting tick.
    bool m_engaged = false;
    double m_lastRate = 1.0;            // R_last
    std::vector<double> m_lastKg;       // last commanded core state, per slot
    double m_lastFuelUsedKg = 0.0;      // clamp reference anchor (always observed)
    bool m_haveFuelUsedAnchor = false;

    // Deadband-silence guard (T16): consecutive engaged ticks that burned fuel
    // yet wrote nothing. Edge-triggered so the event fires once per episode
    // rather than every tick. Both reset on any re-baseline.
    int m_silentTicks = 0;
    bool m_silenceReported = false;

    // Residue guard (T25 · issue #37): a write set that does not sum to what the
    // max-burn clamp judged. Edge-triggered for the same reason as the silence
    // guard above — a residue with a standing cause would otherwise put a fresh
    // event on the last-event line every second — and reset alongside it.
    bool m_residueReported = false;

    // Realised-removal anchor (T19). Armed only by a tick that actually issued
    // writes, and consumed by the very next tick — the interval across which
    // that write either landed or did not. Deliberately separate from the
    // engaged-loop anchor above, which holds *commanded* values: the whole point
    // of this measurement is that no commanded number enters it.
    bool m_haveRealisedAnchor = false;
    std::vector<char> m_lastPopulated;     // which slots counted toward the total
    double m_lastObservedTotalKg = 0.0;    // core state as the sim reported it
    double m_lastCommandedRemovalKg = 0.0; // kg the writes asked to remove (>= 0)
    // Whether that write set contained a positive delta (T25 · issue #37). It is
    // what tells fuel arriving from outside apart from a lost write when the
    // measurement comes out negative: a set that only ever removed fuel cannot
    // have added any, so the fuel came from outside — while a set that moved fuel
    // *between* tanks reads exactly the same way when only its removal went
    // missing, and that is a lost write. Every transfer tick is mixed-sign, so
    // this reaches far beyond the clamp boundary that surfaced it.
    bool m_lastWritesHadPositive = false;
};

} // namespace maxwarp
