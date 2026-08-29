#include "Corrector.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace maxwarp {

namespace {

// A330 mains hold 10,848 gal (T2), ~32,970 kg at the core's measured 3.0396
// kg/gal (T17). The band is as loose as the gallons band it replaces — roughly
// 0.73x to 1.21x the real figure — so it tolerates a fuel density that moves.
constexpr double kMainCapacityMinKg = 24000.0;
constexpr double kMainCapacityMaxKg = 40000.0;

// The per-tank clamp, one-way (T28 · issue #43). A bound may decline to move a
// tank further; it may never move one *back*.
//
// The two-argument `max(floor, min(want, cap))` it replaces is a statement about
// where a tank is allowed to sit, and that is a claim the corrector is not
// entitled to make: it only ever gets to say how far the tank moves this tick.
// Where the aircraft has already put a tank outside a bound, the old form
// commanded it back inside — T26 (#38) caught exactly that, the core stepping
// trim to -11.720 past a -6.000 floor and the corrector writing +5.720 kg of
// fuel back in. T28's finding is that this is not a depth the floor was set
// wrongly for: the core does not test for empty before stepping, so it rests one
// transfer step below wherever it last found the tank, and under correction that
// is wherever the clamp just parked it. A deeper floor tracks it 1:1 and the
// write-back survives at any depth, including zero — so the constant was
// standing in for a rule, and this is the rule.
//
// Widening each bound to include the tank's present value is all that takes:
// `kg` below the floor makes the floor `kg` (decline to go lower, never lift),
// `kg` above the ceiling makes the ceiling `kg` (decline to fill further, never
// drain to fit). With the tank inside both bounds — every ordinary tick — this
// is arithmetically the old form.
double clampTarget(double wantKg, double kg, double floorKg, double capKg, bool oneWay)
{
    if (!oneWay)
        return std::max(floorKg, std::min(wantKg, capKg));
    return std::max(std::min(floorKg, kg), std::min(wantKg, std::max(capKg, kg)));
}

bool titleLooksLikeA330(const std::string &title)
{
    std::string upper = title;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return upper.find("A330") != std::string::npos;
}

// T3 GATE 1 — string prefilter + structural fingerprint. Legacy fuel system,
// trim populated, both mains in the A330 capacity band. Accepts the five-tank
// (-300) and six-tank (-200, CENTER populated) signatures alike (T7): center is
// never required.
//
// Every figure here is structural, and since T24 (#34) none of it is gated on the
// bridge. That is the whole point: capacity describes the airframe, so an
// aircraft does not stop being an A330 because a re-establish took core state
// away for a few seconds. GATE 0 already states the bridge-shaped truth ahead of
// this, and states it in bridge words.
bool isDetectedA330(const TickInput &in)
{
    if (!titleLooksLikeA330(in.aircraft.title))
        return false;
    if (!in.aircraft.legacyFuelSystem)
        return false;
    bool trimPopulated = false;
    int mainsInRange = 0;
    for (const auto &t : in.tanks) {
        if (t.role == TankRole::Trim && t.capacityKg > 0.0)
            trimPopulated = true;
        if (t.role == TankRole::Main && t.capacityKg >= kMainCapacityMinKg &&
            t.capacityKg <= kMainCapacityMaxKg)
            ++mainsInRange;
    }
    return trimPopulated && mainsInRange >= 2;
}

std::string stateEventText(CorrectorState state, const std::string &reason)
{
    switch (state) {
        case CorrectorState::Off:
            return "DISENGAGED — master switch OFF";
        case CorrectorState::Standby:
            return "standing by — " + reason;
        case CorrectorState::Correcting:
            return "engaged — correcting";
        case CorrectorState::Suspended:
            return "suspended — " + reason;
    }
    return reason;
}

} // namespace

void Corrector::arm()
{
    m_master = MasterSwitch::Auto;
}

void Corrector::disengage()
{
    m_master = MasterSwitch::Off;
}

std::string Corrector::detectVariant(const TickInput &in) const
{
    // The -200's airframe has a centre tank; the -300's does not (T2/T7 measured
    // 10,735 gal against 0). Structural capacity, never quantity: since T24 (#34)
    // a -200 whose centre has drained to a true zero is still a -200, where the
    // fused field had the identity resting on T14's sub-0.001 kg residual.
    for (const auto &t : in.tanks)
        if (t.role == TankRole::Center && t.capacityKg > 0.0)
            return "A330-200";
    return "A330-300";
}

// Session lifecycle (T10, rewritten by T24 · issue #34 to carry T23's decisions).
//
// The pairing is with an **airframe**, and what identifies an airframe is the
// aircraft the sim names. Everything else follows:
//
//   * A pairing ends when that name changes, and at no other time. Losing sight
//     of the aircraft — a re-establish, a publish settle, a load — *holds* the
//     pairing; the same airframe coming back resumes it, non-destructively by
//     construction, with no timers and no hysteresis.
//   * The variant is an observation about the pairing, so it is re-read every
//     tick and adopted in place. It never clears anything. Keying the pairing on
//     it (as this did until T24) both missed a same-variant swap entirely and
//     fired on a variant reading that had merely gone quiet.
//
// Accepted cost, stated in T23: loading a *different flight* on the same aircraft
// and livery resumes the previous flight's counters, because the sim publishes
// nothing that tells that apart from a hiccup. The realised anchor voids across
// the gap either way, so only totals carry — nothing is mis-measured.
void Corrector::updateSession(bool detected, const TickInput &in, TickDecision &d)
{
    if (detected) {
        const std::string variant = detectVariant(in);
        if (m_counters.title != in.aircraft.title) {
            // A different airframe. This is the *only* thing that starts a new
            // pairing, and so the only thing that clears the previous flight's
            // lingering counters (T10's lingering-stats rule).
            m_counters = SessionCounters{};
            m_counters.title = in.aircraft.title;
            m_counters.variant = variant;
            m_counters.active = true;
            m_sessionActive = true;
            // A pairing boundary voids any outstanding realised interval with the
            // counters it would have been added to (T19).
            m_haveRealisedAnchor = false;
            d.events.push_back("aircraft detected: " + variant + " — session started");
        } else if (!m_sessionActive) {
            // The same airframe back after a gap: the pairing resumes, because
            // nothing about that flight ended. The counters carry on; only the
            // realised interval is lost, and it is lost by construction — no tick
            // in the gap wrote anything, so no anchor was armed to measure.
            m_counters.active = true;
            m_sessionActive = true;
            d.events.push_back("aircraft re-acquired: " + variant + " — session resumed");
        }
        // Adopted silently, every tick (T23 decision 6). TITLE and the capacities
        // arrive on separate streams, so a swap shows new capacities under the old
        // title for a tick or two; an event here would fire on every swap and mean
        // "a swap happened", not "the model is wrong".
        m_counters.variant = variant;
    } else if (m_sessionActive) {
        // Sight of the aircraft lost. The pairing is *held*, not ended — only a
        // different airframe ends one. The counters keep their values and the
        // session log keeps recording, which is what makes this event loggable at
        // all: under the old rule the row carrying it was skipped for being
        // inactive and the file closed immediately after, so `aircraft lost`
        // appears in no capture ever taken and structurally never could.
        m_sessionActive = false;
        m_counters.active = false;
        d.events.push_back("aircraft lost — session held");
    }

    // Elapsed is wall time since the pairing locked. It ticks regardless of the
    // master switch (T10) and continues across a gap in which the aircraft went
    // unseen, because the pairing is still live through one.
    if (!m_counters.title.empty())
        m_counters.elapsedSeconds += in.dtWall;
}

// Realised removal (T19 · issue #21). `removedCommandedKg` is the corrector's own
// account of its intentions, and T13 (#14) proved that an account of intentions
// can be off by everything: 1133 gal commanded against a drop that was pure
// natural drain. What settles the question is an identity that mentions nothing
// the corrector did —
//
//     fuel in tanks now = fuel in tanks then - engines burned - we removed
//
// so, rearranged and measured over one tick:
//
//     realised = (tank total then - tank total now) - (burn integrator advance)
//
// Both terms come from the core's own published state: `INI_FUEL_WEIGHT_*` and
// `INI_ENG1/2_FUEL_USED`, which T17 (#19) matched to observed drain within 0.03%
// and T16 (#18) cross-checked over 817 ticks at 1.00000 +/- 0.35%. A landed write
// shows up in the first term and not the second; a swallowed one shows up in
// neither, and reads a clean zero. The signal is large next to that noise: at 4x
// a tick's realised removal is 3x its natural burn.
//
// This is also the only instrument that can see a lost write at 2x, where the
// conservation gate is structurally blind (a lost write reads as a gain of
// (R-2)*drain, which at R=2 is nothing at all).
//
// T25 (#37) sharpened the one branch that had been guessing. A negative
// measurement — fuel where none should be — was attributed wholesale to fuel
// arriving from outside the model, a sign test that never considered **our own
// writes** as a source. On T12's clamp-boundary tick it voided the only interval
// of 1,146 and threw away the only evidence of the defect anyone had. What
// separates the two cases is the sign of the write set itself: see
// `m_lastWritesHadPositive`.
//
// Nothing downstream reads these counters — no gate, no write, no state. The
// measurement observes the corrector; it never steers it.
void Corrector::accountRealised(bool detected, const TickInput &in, TickDecision &d)
{
    // The anchor is armed by a writing tick and consumed by the next one,
    // whatever that tick turns out to be. Clearing it up front means an interval
    // is measured at most once, and a tick that writes nothing leaves nothing to
    // measure — which is what keeps a long stand-down from accruing the APU's
    // draw (real fuel out of a main, absent from the burn integrator) as if the
    // corrector had removed it.
    const bool hadAnchor = m_haveRealisedAnchor;
    m_haveRealisedAnchor = false;
    if (!hadAnchor)
        return;

    // Voiding is now *counted* (T25): an interval scores neither realised nor
    // measured, so before these counters existed the kg it covered simply went
    // missing from both and `commanded - measured` could not be read as anything.
    auto voidInterval = [&] {
        ++m_counters.voidedIntervals;
        m_counters.voidedKg += m_lastCommandedRemovalKg;
    };

    // Void the interval whenever something other than the engines and our own
    // writes could have moved the fuel, or the reference is missing at either
    // end. A voided interval costs its commanded kg from `removedMeasuredKg` too,
    // so the ratio it feeds never silently reads as write loss.
    if (!detected || !in.bridgeUp || in.paused || in.slew || in.coreRefueling ||
        in.dtWall > m_config.maxDtSeconds || !in.haveCoreFuelUsed ||
        !m_haveFuelUsedAnchor) {
        voidInterval();
        return;
    }
    if (in.tanks.size() != m_lastPopulated.size()) {
        voidInterval();
        return;
    }

    // Readability, not capacity (T24): this sums `kg`, and an unreadable slot's
    // kg is a lie — a name the bridge never resolved reads a flat 0 (T17). What
    // the airframe *has* is beside the point when the question is what we can
    // currently see.
    double observedTotalKg = 0.0;
    for (size_t i = 0; i < in.tanks.size(); ++i) {
        const char populated = in.tanks[i].readable ? 1 : 0;
        if (populated != m_lastPopulated[i]) {
            // A slot resolved or dropped mid-interval; the totals differ by a
            // whole tank, which is not a measurement of anything.
            voidInterval();
            return;
        }
        if (populated)
            observedTotalKg += in.tanks[i].kg;
    }

    const double tankDropKg = m_lastObservedTotalKg - observedTotalKg;
    const double engineBurnKg = in.coreFuelUsedKg - m_lastFuelUsedKg;
    const double realisedKg = tankDropKg - engineBurnKg;

    // Fuel where none should be. Which of the two things that means comes from
    // the signs of our own write set (T25 · issue #37) — until then this whole
    // branch was attributed to the first, and voided.
    if (realisedKg < -m_config.gainEpsilonKg) {
        if (m_lastWritesHadPositive) {
            // A mixed-sign set moved fuel *between* tanks, so our own writes are
            // a candidate source for what appeared, and the reading that fits is
            // a lost negative write: the removal never landed while the transfers
            // into the other tanks did. A lost write realises exactly zero and
            // never less (T19), so score zero — but *count* the interval, so
            // fidelity takes the hit honestly instead of the evidence being
            // discarded. This fails pessimistic on purpose: a genuine defuel
            // during a transfer tick understates fidelity rather than hiding a
            // loss, and the EFB flag has already voided a real refuel a branch
            // earlier.
            m_counters.removedRealisedKg += 0.0;
            m_counters.removedMeasuredKg += m_lastCommandedRemovalKg;
            return;
        }
        // Nothing in an all-negative write set could have added fuel, so it came
        // from outside the model — a defuel panel, a third-party tool, a bad
        // read. This is the one cause of a void with no other voice: refuel,
        // pause, slew, long frame, bridge gap and slot changes all announce
        // themselves as suspensions, and it is precisely that silence which let
        // T12 report zero voids.
        voidInterval();
        d.events.push_back("interval voided — fuel arrived from outside the model");
        return;
    }

    m_counters.removedRealisedKg += realisedKg;
    m_counters.removedMeasuredKg += m_lastCommandedRemovalKg;
}

TickDecision Corrector::onTick(const TickInput &in)
{
    TickDecision d;
    const bool detected = isDetectedA330(in);

    // Session accounting runs first and independent of the master switch, so
    // elapsed and the pairing lifecycle advance even while OFF (T10).
    updateSession(detected, in, d);

    // Then the realised measurement (T19), which describes the interval that has
    // already elapsed — before any gate, because nothing this tick decides can
    // change what the aircraft has already done. In particular the tick that
    // stands down, disengages or suspends still credits the write issued a
    // second earlier, and a tick that suspends *because* a write went missing
    // records that as a realised zero rather than voiding the evidence.
    accountRealised(detected, in, d);

    decide(detected, in, d);

    // Emit an event only on a genuine state change (T10) — never per tick, so the
    // last-event line doesn't churn while a state persists.
    if (d.state != m_lastState) {
        d.events.push_back(stateEventText(d.state, m_reason));
        m_lastState = d.state;
    }

    d.counters = m_counters;
    return d;
}

void Corrector::decide(bool detected, const TickInput &in, TickDecision &d)
{
    // Master switch OFF — sticky, no writes (T10).
    if (m_master == MasterSwitch::Off) {
        m_engaged = false;
        m_reason = "master switch OFF";
        d.state = CorrectorState::Off;
        return;
    }

    // Re-anchor to core state as the sim actually shows it. Every path that
    // writes nothing takes this: engage transitions, suspensions, discarded
    // ticks. The clamp's burn reference always anchors to what was observed —
    // it counts engine fuel used, which our writes never touch.
    auto anchorObserved = [&] {
        m_lastKg.resize(in.tanks.size());
        for (size_t i = 0; i < in.tanks.size(); ++i)
            m_lastKg[i] = in.tanks[i].kg;
        m_lastRate = in.simRate;
        m_lastFuelUsedKg = in.coreFuelUsedKg;
        m_haveFuelUsedAnchor = in.haveCoreFuelUsed;
        // Any re-baseline restarts the deadband-silence observation: the ticks
        // before it were measured against an anchor that no longer stands. The
        // residue episode goes with it, for the same reason.
        m_silentTicks = 0;
        m_silenceReported = false;
        m_residueReported = false;
    };

    // GATE 0 — the bridge (T20). Core state lives inside the sim and MaxWarp is
    // outside it, so with no bridge there is nothing to read and nowhere to
    // write; T18 ruled out any fallback path deliberately. Checked ahead of
    // aircraft detection because it is the more fundamental lack — the reason
    // *which* way the bridge is unavailable rides on its own event, so this one
    // stays generic.
    if (!in.bridgeUp) {
        m_engaged = false;
        m_reason = "no bridge to core state";
        d.state = CorrectorState::Standby;
        return;
    }

    // GATE 1 — aircraft detection. Mismatch drops to STANDBY (armed, precondition
    // unmet); the anchor is dropped so the next engage re-anchors.
    if (!detected) {
        m_engaged = false;
        m_reason = "no A330 detected";
        d.state = CorrectorState::Standby;
        return;
    }

    // GATE 2 — abnormal state. Pause / active-pause / slew suspends and
    // re-anchors; clearing m_engaged makes the first post-resume tick a fresh
    // engage transition, which is exactly T4's "skip one tick" (T3 decision 6).
    if (in.paused || in.slew) {
        m_engaged = false;
        m_reason = in.slew ? "slew active" : "sim paused";
        d.state = CorrectorState::Suspended;
        d.reBaselined = true;
        anchorObserved();
        return;
    }

    // GATE 3 — engage only above the rate threshold.
    if (in.simRate <= m_config.engageThreshold) {
        m_engaged = false;
        m_reason = "sim rate 1x";
        d.state = CorrectorState::Standby;
        return;
    }

    d.state = CorrectorState::Correcting;
    d.rEff = in.simRate;

    // dt cap (T4). An interval longer than the cap (long frame / hitch) can't be
    // trusted for endpoint-average amplification: discard the tick, re-anchor,
    // write nothing. The next healthy tick corrects off this fresh anchor.
    if (in.dtWall > m_config.maxDtSeconds) {
        m_reason = "long frame — tick discarded";
        d.state = CorrectorState::Suspended;
        d.reBaselined = true;
        anchorObserved();
        return;
    }

    // Engage transition — re-anchor, skip correction this tick (T3 GATE 3).
    if (!m_engaged) {
        m_engaged = true;
        anchorObserved();
        return;
    }

    // GATE 4a — conservation, stated exactly. The core's own EFB refuel flag
    // (T17/T18) replaces inferring a refuel from the numbers: the moment fuel
    // starts arriving, amplifying anything is wrong, and the flag says so on the
    // first tick rather than once the gain has grown past a threshold.
    if (in.coreRefueling) {
        m_reason = "refuel in progress (EFB)";
        d.state = CorrectorState::Suspended;
        d.reBaselined = true;
        anchorObserved();
        return;
    }

    // GATE 4b — conservation, inferred. Still on as a backstop: it covers the
    // defuelling and third-party paths the EFB flag does not, and it is how a
    // write that never landed surfaces at all, since the bridge acknowledges
    // nothing (T17). Against a *commanded* anchor a lost write reads as a gain,
    // so the tick fails safe instead of the corrector clawing at the difference.
    double netDeltaKg = 0.0;
    for (size_t i = 0; i < in.tanks.size(); ++i) {
        if (!in.tanks[i].readable)
            continue; // no core state for this slot; its kg is a flat 0, not a
                      // quantity, and differencing it would net a whole tank
        netDeltaKg += in.tanks[i].kg - m_lastKg[i];
    }
    if (netDeltaKg > m_config.gainEpsilonKg) {
        m_reason = "unexpected fuel gain (conservation gate)";
        d.state = CorrectorState::Suspended;
        d.reBaselined = true;
        anchorObserved();
        return;
    }

    // Steady-state: uniform per-tank amplification (T3 decision 1). R_eff is the
    // endpoint average of the interval's rates (decision 3).
    const double rEff =
        (m_lastRate == in.simRate) ? in.simRate : (m_lastRate + in.simRate) / 2.0;
    d.rEff = rEff;

    // The tanks this tick may write to, as a table rather than a write list:
    // reconciliation below revises the whole set, so no target is final until
    // every clamp has been settled against every other.
    struct Slot {
        int index;
        double kg;       // core state now — what a write is a movement from
        double floorKg;  // per-tank clamp bounds (T3 decision 7)
        double capKg;
        double weightKg; // |natural delta|: its share of any deficit
        double wantKg;   // the target it should reach, revised by redistribution
        double targetKg; // the target it can reach: wantKg, clamped
        bool pinned;     // a bound holds it, so it can absorb no more
    };
    std::vector<Slot> slots;
    d.observedDeltaKg.assign(in.tanks.size(), 0.0);
    int correctableSlots = 0;
    // Readable movement the amplification declined — the deadband skips, and the
    // readable-but-capacity-less slots the loop steps over just above. Signed,
    // and accumulated here because this is the loop that already knows every
    // delta. It exists for the output check below, which was measuring the write
    // set against a net that included movement nothing was ever going to amplify
    // (T28 · issue #43): that difference is not a residue, it is a decision, and
    // naming it here is also what stops it hiding inside the check's slack.
    double skippedDeltaKg = 0.0;
    for (size_t i = 0; i < in.tanks.size(); ++i) {
        const TankReading &tank = in.tanks[i];
        // Correcting a tank needs both answers T24 (#34) just separated: the
        // bridge must have core state for it, and the airframe must actually have
        // it. Readability alone is not enough, because the ceiling a line below is
        // the structural capacity and a slot without one has no ceiling to clamp
        // against — a zero there would command the whole tank away.
        if (!tank.readable || tank.capacityKg <= 0.0) {
            if (tank.readable)
                skippedDeltaKg += tank.kg - m_lastKg[i];
            continue;
        }
        ++correctableSlots;
        const double delta = tank.kg - m_lastKg[i];
        d.observedDeltaKg[i] = delta;
        if (std::fabs(delta) < m_config.noiseDeadbandKg) {
            skippedDeltaKg += delta;
            continue; // per-tank noise deadband (T3 decision 5); a tank that has
                      // not moved also carries no weight in the redistribution
                      // below, which needs no special case to say so
        }
        const double correction = (rEff - 1.0) * delta;

        // Per-tank [floor, capacity] clamp (T3 decision 7), one-way since T28
        // (#43). Every tank floors at empty — trim's small negative floor is
        // gone with the constant, because the rule tolerates the core resting
        // trim at *any* depth where a floor only ever tolerated depths shallower
        // than a guessed number.
        Slot s;
        s.index = static_cast<int>(i);
        s.kg = tank.kg;
        s.floorKg = 0.0;
        s.capKg = tank.capacityKg;
        s.weightKg = std::fabs(delta);
        s.wantKg = tank.kg + correction;
        s.targetKg =
            clampTarget(s.wantKg, s.kg, s.floorKg, s.capKg, m_config.oneWayClamp);
        s.pinned = (s.targetKg != s.wantKg);
        slots.push_back(s);
    }

    // RECONCILIATION (T25 · issue #37) — what the per-tank clamp implies for
    // every *other* tank.
    //
    // A clamped tank is not simply corrected less; the fuel it could not give up
    // is a **deficit** the rest of the aircraft has to carry, because what T3
    // conserves is the aircraft's net and never the ratio between tanks. Until
    // this existed, a transfer whose source ran dry had the source pinned at empty
    // while the destinations kept their gain amplified in full, and the aircraft
    // net-*gained* for a tick: 23.8 kg of the 4,519.8 kg T12 (#13) commanded, and
    // the whole of its 8x deviation. Every gate read pre-clamp quantities, so all
    // three of them passed it.
    //
    // Spread pro rata by |natural delta| across the tanks no bound is holding,
    // regardless of sign — a destination's write may go negative, because the
    // engines feed from the inners whether or not the centre is still filling
    // them. Re-clamp and repeat, since a share can pin another tank. Scaling the
    // whole tick by the clamped tank's factor instead was rejected in T25: it
    // preserves a ratio nothing depends on while under-correcting the *burn*, and
    // one dry tank would throttle the whole aircraft as its factor went to zero.
    //
    // With deficit zero this is arithmetically the loop above and nothing else,
    // which is why it reduces exactly to T3 on every tick that clamps nothing.
    double deficitKg = 0.0;
    for (const Slot &s : slots)
        deficitKg += s.wantKg - s.targetKg;
    if (m_config.reconcileDeficit) {
        // Each pass either clears the deficit or pins at least one more tank, so
        // one pass per slot is a hard bound on the iteration.
        for (size_t pass = 0; pass < slots.size() && std::fabs(deficitKg) > 0.0; ++pass) {
            double weightSum = 0.0;
            for (const Slot &s : slots)
                if (!s.pinned)
                    weightSum += s.weightKg;
            if (weightSum <= 0.0)
                break; // residue — every tank that moved is already at a bound
            const double perKg = deficitKg / weightSum;
            deficitKg = 0.0;
            for (Slot &s : slots) {
                if (s.pinned || s.weightKg <= 0.0)
                    continue;
                s.wantKg += perKg * s.weightKg;
                s.targetKg =
                    clampTarget(s.wantKg, s.kg, s.floorKg, s.capKg, m_config.oneWayClamp);
                if (s.targetKg != s.wantKg) {
                    s.pinned = true;
                    deficitKg += s.wantKg - s.targetKg;
                }
            }
        }
    }

    std::vector<TankWrite> candidates;
    candidates.reserve(slots.size());
    for (const Slot &s : slots) {
        // The bridge applies a signed delta through an in-sim RMW, so what leaves
        // here is the movement, not the destination — computing the difference
        // any later would race the core's own 1 Hz integration step (T17).
        candidates.push_back({s.index, s.targetKg - s.kg, s.targetKg});
    }

    // CLAMP — max-burn backstop (T3 decision 7), referenced to the core's own
    // cumulative burn integrator (T17/T21). `ENG FUEL FLOW PPH` used to play this
    // role and under-reported the real drain by a ratio that is not even constant
    // across variants (1.3555 on the -200 vs 1.398 on the -300), which ate the
    // margin and tripped the clamp every 10-50 s under live correction (T13).
    // Differencing a cumulative integrator also makes a variable dt free — the
    // reference covers exactly the interval that elapsed, whatever it was.
    // Unavailable reference (names unresolved) self-disables the clamp rather
    // than freezing the corrector.
    //
    // Its subject is the amplified **net** system change, not the sum of the
    // per-tank corrections above (T16). The two differ only by the tanks the
    // deadband skipped, but that difference is exactly T13's second fault: a
    // transfer is mass-neutral and cancels in a signed sum, so it can only
    // distort the clamp when the deadband drops one side of it — a main
    // receiving fuel while it burns can net to nearly nothing and be skipped,
    // leaving the tank it came from counted alone. Taking the net makes the
    // clamp immune to transfers by construction rather than by the deadband
    // happening to be small. Neither subject sees a *balanced* bad read; the
    // per-tank [floor, capacity] clamp above is what bounds that.
    const double totalCorrectionKg = (rEff - 1.0) * netDeltaKg;
    const double naturalBurnKg = (in.haveCoreFuelUsed && m_haveFuelUsedAnchor)
                                     ? in.coreFuelUsedKg - m_lastFuelUsedKg
                                     : 0.0;
    const double expBurnKg = naturalBurnKg * rEff;
    if (expBurnKg > 0.0 &&
        std::fabs(totalCorrectionKg) > m_config.clampMargin * expBurnKg) {
        m_reason = "max-burn clamp — implausible loss (jettison / bad read)";
        d.state = CorrectorState::Suspended;
        d.reBaselined = true;
        anchorObserved();
        return;
    }

    // OUTPUT CHECK — residue (T25 · issue #37). Nothing in the corrector had ever
    // looked at its own write set: both conservation gates and the max-burn clamp
    // read what the aircraft has already done, which is exactly why T12's tick
    // passed all three while the sum of the writes it was about to issue — a
    // quantity computed nowhere — stood at +12.755 kg against an intended
    // -11.032. The next tick could not catch it either, since the anchor is
    // commanded, so the fuel stayed in the aircraft permanently.
    //
    // Reconciliation is what makes this checkable at all: with the deficit
    // carried, the writes sum to the very quantity the clamp above judged, so the
    // clamp governs what is actually *written* rather than what was merely wanted.
    // What is left over is a residue — a deficit no tank had room to absorb.
    //
    // The reference is the amplified net **of the movement that was amplified**,
    // which is not the same net the clamp above judges (T28 · issue #43). The
    // clamp's subject is the whole readable net on purpose — that is what makes
    // it immune to transfers — but the write set can only ever sum to the part of
    // it the amplification loop accepted, so measuring one against the other
    // reports every deadband skip as a residue:
    //
    //     residue = -(R_eff - 1) x skipped
    //
    // At 8x a tank moving 1.9 g therefore reported a 13 g residue, which is
    // precisely the two events T26 (#38) logged, and it means the check had never
    // once fired on a real reconciliation failure. The fault is in the reference,
    // not the tolerance: the skipped movement was declined deliberately and is
    // known exactly, so it belongs on this side of the subtraction rather than
    // inside a slack wide enough to hide it. Equivalently, the clamp's subject is
    // `writeSum + (R_eff - 1) x skipped` — T27's identity, restated.
    double writeSumKg = 0.0;
    for (const TankWrite &w : candidates)
        writeSumKg += w.deltaKg;
    const double intendedWriteSumKg = (rEff - 1.0) * (netDeltaKg - skippedDeltaKg);
    const double residueKg = writeSumKg - intendedWriteSumKg;
    if (std::fabs(residueKg) > m_config.residueToleranceKg) {
        // A residue that would leave the aircraft *gaining* is correcting
        // backwards (CONTEXT.md) — the opposite of the job, and silent for the
        // same reason correcting nothing is. Suspend and re-baseline; anything
        // else commands fuel in.
        if (netDeltaKg + writeSumKg > m_config.gainEpsilonKg) {
            m_reason = "correcting backwards — writes would add fuel";
            d.state = CorrectorState::Suspended;
            d.reBaselined = true;
            anchorObserved();
            return;
        }
        // Still a net loss, so write it: best effort beats nothing, since writing
        // nothing is a larger departure from the intended net than writing most
        // of it. Never silently, though — edge-triggered like the silence guard,
        // so a standing cause says so once rather than every second.
        if (!m_residueReported) {
            m_residueReported = true;
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "residue %.3f kg — writes do not sum to the intended net, "
                          "written best effort",
                          residueKg);
            d.events.push_back(msg);
        }
    } else {
        m_residueReported = false;
    }

    // GUARD — correcting in name only (T16). A deadband coarser than the drain
    // it sits under does not fail loudly: every tank is skipped, the state stays
    // CORRECTING, the stage rail stays green, and not a gram is amplified. The
    // discriminator is the burn integrator. If it is advancing then fuel is
    // leaving the tanks, so an empty write set means the deadband ate the
    // correction — where a frozen bridge sample (the ~1% beat between our tick
    // and the core's own 1 Hz integration) and engines-off both advance nothing
    // and are correctly silent. Edge-triggered, so an episode says so once.
    // `correctableSlots` is T24's addition: with capacity ungated the corrector
    // now reaches this point during the bridge's publish settle, where there is
    // simply no tank to write to yet. That is not the deadband swallowing a
    // correction, and saying so would put a false accusation on the last-event
    // line at the start of every session.
    if (naturalBurnKg > 0.0 && correctableSlots > 0 && candidates.empty()) {
        if (++m_silentTicks >= m_config.deadbandSilenceTicks && !m_silenceReported) {
            m_silenceReported = true;
            d.events.push_back(
                "correcting nothing — every tank is under the noise deadband");
        }
    } else {
        m_silentTicks = 0;
        m_silenceReported = false;
    }

    d.writes = std::move(candidates);

    // Correcting-tick counters (T10). observed = the natural burn this tick;
    // removed = the extra the corrector commanded away; effective = the two
    // summed. Both derive from the same net, so effective is exactly
    // R_eff x observed — the T3 invariant the algorithm claims, stated in the
    // counters rather than approximated by them. These pause when disengaged
    // (OFF returns above), never clear on disengage. All three are *commanded*,
    // and since T19 all three are named so.
    const double observed = netDeltaKg < 0.0 ? -netDeltaKg : 0.0;
    const double removed = totalCorrectionKg < 0.0 ? -totalCorrectionKg : 0.0;
    m_counters.burnObservedKg += observed;
    m_counters.removedCommandedKg += removed;
    m_counters.burnEffectiveKg += observed + removed;

    // Arm the realised-removal anchor (T19), but only if something was actually
    // written — an interval with no write in it has no fidelity to measure. The
    // commanded figure kept here is the sum of the write deltas rather than the
    // `removed` above: it is what the bridge was actually asked to do, and so
    // the only quantity the next tick's measurement is the outcome *of*. The two
    // differ exactly by the tanks the deadband skipped and the per-tank clamp
    // bound, which is a difference between the counters worth being able to see.
    if (!d.writes.empty()) {
        m_lastPopulated.assign(in.tanks.size(), 0);
        m_lastObservedTotalKg = 0.0;
        for (size_t i = 0; i < in.tanks.size(); ++i) {
            m_lastPopulated[i] = in.tanks[i].readable ? 1 : 0;
            if (m_lastPopulated[i])
                m_lastObservedTotalKg += in.tanks[i].kg;
        }
        double commandedRemovalKg = 0.0;
        bool hadPositive = false;
        for (const TankWrite &w : d.writes) {
            commandedRemovalKg -= w.deltaKg; // removal is a negative delta
            if (w.deltaKg > 0.0)
                hadPositive = true;
        }
        m_lastCommandedRemovalKg = commandedRemovalKg;
        // Kept for the next tick's classification of a negative measurement (T25):
        // only a positive write of ours can be the source of fuel that appeared.
        m_lastWritesHadPositive = hadPositive;
        m_haveRealisedAnchor = true;
    }

    // Anchor to what we *commanded*, never to what we observed (T21, from T13's
    // diagnosis). Core state after a write includes the correction, so anchoring
    // on the observed pre-write value re-amplifies our own surviving writes —
    // which is what produced 12 of T13's 21 spurious clamp trips. Against the
    // commanded anchor, next tick's natural drain is
    //
    //     natural_delta = current_core_state - our_last_commanded_value
    //
    // exactly, over whatever interval actually elapsed. That is also why the
    // clock phase between the core's own 1 Hz integration and SimConnect
    // delivery is harmless (T18): a tick straddling two core updates sees a
    // double delta and the next sees ~zero, and since the correction is linear
    // in the observed delta, the pair sums correctly.
    m_lastKg.resize(in.tanks.size());
    for (size_t i = 0; i < in.tanks.size(); ++i)
        m_lastKg[i] = in.tanks[i].kg; // an unwritten tank commands its own value
    for (const TankWrite &w : d.writes)
        m_lastKg[w.tankIndex] = w.targetKg;
    m_lastRate = in.simRate;
    // The burn reference is engine fuel used, which our writes never move: it
    // always anchors to what the core reported.
    m_lastFuelUsedKg = in.coreFuelUsedKg;
    m_haveFuelUsedAnchor = in.haveCoreFuelUsed;
}

} // namespace maxwarp
