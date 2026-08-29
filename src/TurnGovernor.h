#pragma once

// MaxWarp turn governor core (T35 · issue #54) — the pure, Qt-free, SimConnect-
// free decision engine behind the turn governor (map #49), sitting beside
// `Corrector` on the worker's 1 Hz tick. Same shape as the corrector: the whole
// per-tick pipeline is `onTick(GovernorInput) -> GovernorDecision`, a function
// of explicit state, unit-tested against the T37 flight capture with no sim in
// the loop. The worker adapts telemetry into GovernorInput and turns the
// returned rate ceiling into stepper events (that wiring is a later ticket).
//
// The model it implements is T33's (#52) — SimBrief-by-position plus the sensed
// turn — measured by T31/T37 (#50/#56):
//
//   * The plan is the human's SimBrief navlog: ordered fixes with positions,
//     origin dropped as a fix but kept as the start of the first leg, TOC/TOD
//     dropped, destination kept (T32). It knows nothing of MCDU edits or of
//     which leg is active — that is inferred from position, monotonically: the
//     active leg is picked once and only ever advances, the fix being
//     **sequenced** when the aircraft passes abeam it (the turn bisector), and it
//     steps back only by re-acquiring after going off plan.
//   * On plan within 5 nm cross-track of the active leg; off plan after 3 ticks
//     beyond it; re-acquire = nearest leg ahead, again for 3 ticks. Off plan
//     names no fix and predicts no window.
//   * Turn angle = |plan outbound course from the fix − live ground track|,
//     0–180°: inbound taken from the aircraft, so a DIR TO to a planned fix reads
//     correctly unasked. The destination has no outbound leg → no window.
//   * Lead = R·tan(θ/2) + 1.5 nm + margin + GS·rate·2 ticks, R the 25°-bank
//     turn radius at live GS — T37 measured roll-in at R·tan(θ/2) + 0–1.5 nm,
//     the sim rate not moving that point.
//   * Sensed onset = |bank| > 2° for two consecutive ticks or |heading rate|
//     > 0.15°/s (per sim-second — the noise floor was 0.07° / 0.055°/s). It arms
//     a predicted window early whenever the bank arrives first, and stands alone
//     as the plan-less source only when that setting is on.
//   * Restore: plan window on sequenced ∧ turn seen ∧ wings level ≥ 5 sim-s;
//     sensed window on wings level ≥ 5 sim-s after onset; both capped at a flat
//     ~3 sim-minutes so a missed signal never strands the rate.
//   * The ceiling is a ceiling (T41, #60): a rate change inside a window is
//     nothing to the core - the wiring holds the turn rate against every
//     source and the window runs to its restore. Live only airborne.
//
// The governor never touches the rate itself. Its output is a *ceiling* — the
// turn rate while a window is open, none otherwise — and it is the wiring's job
// to lower toward it and to hand the human's cruise rate back on restore. That
// keeps "cruise rate" (the human's last choice) out of here entirely: the core
// names a ceiling from geometry and bank, and the wiring alone decides what the
// stepper does about it.

#include <string>
#include <vector>

namespace maxwarp {

struct PlanFix {
    std::string ident;
    double latDeg = 0.0;
    double lonDeg = 0.0;
};

// The human's filed route (T32 shape, already filtered by the caller). `origin`
// is the departure aerodrome — never named, never a window, but the first leg
// needs somewhere to start; without it the first fix has no leg to be on and
// acquisition begins at the second fix.
struct Plan {
    bool haveOrigin = false;
    PlanFix origin;
    std::vector<PlanFix> fixes; // navlog fixes in order, destination last
    bool empty() const { return fixes.empty(); }
};

struct GovernorSettings {
    bool enabled = false;          // the governor switch (default off)
    double turnRate = 1.0;         // rate held through a turn window
    double angleThresholdDeg = 5.0;  // below this a fix is not a turn (0–180); T40: the ini A330 mishandles ≥ ~6° at 4×+
    double marginNm = 2.0;         // human's margin ahead of the roll-in point
    bool sensedTurnSource = false; // the plan-less role of the sensed turn
};

struct GovernorInput {
    double dtWall = 1.0;        // real seconds since the previous tick
    double simRate = 1.0;       // SIMULATION RATE readback
    bool onGround = true;       // SIM ON GROUND
    double latDeg = 0.0;        // PLANE LATITUDE
    double lonDeg = 0.0;        // PLANE LONGITUDE
    double groundTrackDeg = 0.0; // GPS GROUND TRUE TRACK (live airborne, T37)
    double groundSpeedKt = 0.0; // GROUND VELOCITY
    double bankDeg = 0.0;       // PLANE BANK DEGREES (negative = right, T37)
    double headingDeg = 0.0;    // PLANE HEADING DEGREES TRUE (for the rate)
};

// The governor stage on the status rail — fixed words (T33/T34).
enum class GovernorStage { Off, NoPlan, OffPlan, OnPlan, Turn };

// What opened the window (the TURN sub-word names it).
enum class WindowSource { None, Predicted, SensedEarly, Sensed };

struct GovernorDecision {
    GovernorStage stage = GovernorStage::Off;
    bool airborne = false;
    // Rate ceiling this tick: the turn rate while a window is open. `haveCeiling`
    // false means the governor asks nothing of the rate.
    bool haveCeiling = false;
    double rateCeiling = 0.0;
    bool windowOpen = false;
    WindowSource source = WindowSource::None;
    // The named next fix while ON PLAN / TURN (empty otherwise), with its turn
    // angle (negative when there is none — the destination), the distance to it
    // and the lead the window opens at.
    std::string nextFixIdent;
    double turnAngleDeg = -1.0;
    double distanceToFixNm = -1.0;
    double leadNm = -1.0;
    std::vector<std::string> events; // human-readable transitions
    // The rail's words: "OFF" · "NO PLAN" · "OFF PLAN" · "ON PLAN" + sub ·
    // "TURN" + sub — the sub-word is `stageSub()`.
    std::string stageWord() const;
    std::string stageSub() const;
};

struct GovernorConfig {
    double bankOnsetDeg = 2.0;        // |bank| above this ...
    int bankOnsetTicks = 2;           // ... for this many consecutive ticks
    double headingRateOnsetDegPerS = 0.15; // or |hdg rate| above this (per sim-s)
    double wingsLevelDeg = 3.0;       // |bank| below this counts as level
    double wingsLevelSimSeconds = 5.0;
    double windowCapSimSeconds = 180.0;
    double offPlanNm = 5.0;
    int offPlanTicks = 3;
    double rollInSlackNm = 1.5;       // T37: roll-in = R·tan(θ/2) + 0–1.5 nm
    double tickAllowanceTicks = 2.0;
    double establishedDeg = 20.0;     // track within this of the leg course = established
    double airborneMinGsKt = 80.0;
    double bankLimitDeg = 25.0;       // the AP's turn bank, for R
};

class TurnGovernor {
public:
    TurnGovernor() = default;
    explicit TurnGovernor(const GovernorConfig &config) : m_config(config) {}

    void setSettings(const GovernorSettings &s);
    const GovernorSettings &settings() const { return m_settings; }
    void setPlan(const Plan &plan);
    void clearPlan();
    bool hasPlan() const { return !m_plan.empty(); }

    // Run the per-tick pipeline. Pure w.r.t. the sim: same inputs + internal
    // state => same decision.
    GovernorDecision onTick(const GovernorInput &in);

    // Index of the active leg's end fix in the plan (-1 when none is named).
    int activeFixIndex() const { return m_activeFix; }

private:
    GovernorConfig m_config;
    GovernorSettings m_settings;
    Plan m_plan;
    int m_activeFix = -1;
    bool m_onPlan = false;
    bool m_planComplete = false; // destination sequenced: nothing left to acquire
    bool m_legLevelSeen = false; // a level tick, established on the leg, since it became active
    int m_offPlanTicks = 0;
    int m_candidateFix = -1;
    int m_candidateTicks = 0;
    // window
    bool m_windowOpen = false;
    const char *m_pendingClose = nullptr; // a close outside a tick, said on the next
    WindowSource m_source = WindowSource::None;
    int m_windowFix = -1;
    bool m_sequenced = false;
    bool m_turnSeen = false;
    double m_levelSimS = 0.0;
    double m_windowSimS = 0.0;
    // sensed onset
    int m_bankTicks = 0;
    bool m_haveHeading = false;
    double m_lastHeading = 0.0;
};

} // namespace maxwarp
