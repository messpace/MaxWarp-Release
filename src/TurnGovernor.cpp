#include "TurnGovernor.h"

#include <cmath>
#include <cstdio>

namespace maxwarp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusNm = 3440.065;
constexpr double kG = 9.80665;
constexpr double kKtToMps = 0.514444;
constexpr double kMetresPerNm = 1852.0;

double rad(double deg) { return deg * kPi / 180.0; }
double deg(double r) { return r * 180.0 / kPi; }

double wrap360(double d)
{
    d = std::fmod(d, 360.0);
    if (d < 0) d += 360.0;
    return d;
}

// Signed difference a - b folded to (-180, 180].
double angleDiff(double a, double b)
{
    double d = std::fmod(a - b, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d <= -180.0) d += 360.0;
    return d;
}

// Great-circle distance, nm.
double distanceNm(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = rad(lat1), p2 = rad(lat2);
    double dp = p2 - p1, dl = rad(lon2 - lon1);
    double a = std::sin(dp / 2) * std::sin(dp / 2) +
               std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * kEarthRadiusNm * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

// Initial great-circle bearing from 1 to 2, degrees true 0-360.
double bearingDeg(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = rad(lat1), p2 = rad(lat2), dl = rad(lon2 - lon1);
    double y = std::sin(dl) * std::cos(p2);
    double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    return wrap360(deg(std::atan2(y, x)));
}

// Signed cross-track distance of P from the great circle A->B (nm, positive
// right of track) and the along-track distance from A (nm, negative behind A).
void trackOffsets(double latA, double lonA, double latB, double lonB, double latP,
                  double lonP, double &crossNm, double &alongNm)
{
    double d13 = distanceNm(latA, lonA, latP, lonP) / kEarthRadiusNm;
    double b13 = rad(bearingDeg(latA, lonA, latP, lonP));
    double b12 = rad(bearingDeg(latA, lonA, latB, lonB));
    double xt = std::asin(std::sin(d13) * std::sin(b13 - b12));
    crossNm = xt * kEarthRadiusNm;
    double at = std::acos(std::cos(d13) / std::cos(xt));
    // acos loses the sign: behind A when the bearing to P is more than 90 deg
    // off the leg's own.
    if (std::fabs(angleDiff(deg(b13), deg(b12))) > 90.0) at = -at;
    alongNm = at * kEarthRadiusNm;
}

} // namespace

std::string GovernorDecision::stageWord() const
{
    switch (stage) {
    case GovernorStage::Off: return "OFF";
    case GovernorStage::NoPlan: return "NO PLAN";
    case GovernorStage::OffPlan: return "OFF PLAN";
    case GovernorStage::OnPlan: return "ON PLAN";
    case GovernorStage::Turn: return "TURN";
    }
    return "";
}

std::string GovernorDecision::stageSub() const
{
    if (stage == GovernorStage::OnPlan) {
        if (turnAngleDeg < 0.0) return nextFixIdent;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s %.0f\xC2\xB0", nextFixIdent.c_str(), turnAngleDeg);
        return buf;
    }
    if (stage == GovernorStage::Turn) {
        // "<fix> · plan" / "<fix> · sensed" / "sensed" (plan-less); U+00B7 as UTF-8.
        const char *src = source == WindowSource::Predicted ? "plan" : "sensed";
        if (nextFixIdent.empty()) return src;
        return nextFixIdent + " \xC2\xB7 " + src;
    }
    return "";
}

void TurnGovernor::setSettings(const GovernorSettings &s) { m_settings = s; }

void TurnGovernor::setPlan(const Plan &plan)
{
    m_plan = plan;
    if (m_windowOpen) m_pendingClose = "plan changed";
    m_windowOpen = false;
    m_source = WindowSource::None;
    m_windowFix = -1;
    m_activeFix = -1;
    m_onPlan = false;
    m_offPlanTicks = 0;
    m_candidateFix = -1;
    m_candidateTicks = 0;
    m_planComplete = false;
}

void TurnGovernor::clearPlan() { setPlan(Plan{}); }

GovernorDecision TurnGovernor::onTick(const GovernorInput &in)
{
    GovernorDecision d;
    if (m_pendingClose) {
        d.events.push_back(std::string("turn window closed: ") + m_pendingClose);
        m_pendingClose = nullptr;
    }
    d.airborne = !in.onGround && in.groundSpeedKt > m_config.airborneMinGsKt;

    // Closing a window for reasons other than a restore.
    auto closeWindow = [&](const char *why) {
        if (!m_windowOpen) return;
        m_windowOpen = false;
        m_source = WindowSource::None;
        m_windowFix = -1;
        d.events.push_back(std::string("turn window closed: ") + why);
    };
    if (!m_settings.enabled) {
        closeWindow("governor off");
        d.stage = GovernorStage::Off;
        return d;
    }
    if (!d.airborne) {
        // Live only airborne: on the ground the governor names nothing.
        closeWindow("on the ground");
        m_bankTicks = 0;
        m_haveHeading = false;
        // "Plan complete" is a property of a flight, not of a plan: it means
        // this one was flown out. Being on the ground ends that flight, so the
        // latch lifts here. Without it the flag outlives everything that could
        // clear it — only setPlan() does — and acquisition below stays gated
        // off for good: fly a route to its destination, lose the sim, reconnect
        // onto the same plan, and the governor never names another fix. It is
        // cleared on the ground rather than at the acquisition gate so that an
        // aircraft still airborne past its destination stays flown-out, and so
        // that no session-boundary code has to know the latch exists.
        m_planComplete = false;
        d.stage = m_plan.empty() ? GovernorStage::NoPlan : GovernorStage::OffPlan;
        return d;
    }

    // --- Position against the plan: acquire, hold, sequence ------------------
    const int n = static_cast<int>(m_plan.fixes.size());
    auto legStart = [&](int fix, double &lat, double &lon) -> bool {
        if (fix == 0) {
            if (!m_plan.haveOrigin) return false;
            lat = m_plan.origin.latDeg;
            lon = m_plan.origin.lonDeg;
            return true;
        }
        lat = m_plan.fixes[fix - 1].latDeg;
        lon = m_plan.fixes[fix - 1].lonDeg;
        return true;
    };
    // Cross/along-track of the aircraft against the leg ending at `fix`.
    auto legOffsets = [&](int fix, double &cross, double &along, double &length) -> bool {
        double la, lo;
        if (!legStart(fix, la, lo)) return false;
        const PlanFix &f = m_plan.fixes[fix];
        trackOffsets(la, lo, f.latDeg, f.lonDeg, in.latDeg, in.lonDeg, cross, along);
        length = distanceNm(la, lo, f.latDeg, f.lonDeg);
        return true;
    };
    // Sequenced when the aircraft is past the fix's turn bisector — the plane
    // through the fix perpendicular to the bisector of inbound and outbound
    // courses (the inbound alone for the destination).
    auto pastFix = [&](int fix) -> bool {
        double la, lo;
        if (!legStart(fix, la, lo)) return false;
        const PlanFix &f = m_plan.fixes[fix];
        double inbound = bearingDeg(la, lo, f.latDeg, f.lonDeg);
        double bisector = inbound;
        if (fix + 1 < n) {
            const PlanFix &g = m_plan.fixes[fix + 1];
            double outbound = bearingDeg(f.latDeg, f.lonDeg, g.latDeg, g.lonDeg);
            bisector = wrap360(inbound + angleDiff(outbound, inbound) / 2.0);
        }
        double toAircraft = bearingDeg(f.latDeg, f.lonDeg, in.latDeg, in.lonDeg);
        return std::fabs(angleDiff(toAircraft, bisector)) < 90.0;
    };
    // The first leg ahead of the aircraft that it is inside the corridor of,
    // searching from `from` on in plan order; -1 if none. Plan order rather
    // than nearest-by-cross-track breaks the ties a route that revisits a place
    // creates - a departure that returns to its own aerodrome sits inside the
    // corridor of its last leg as well as its first, and the first is the one
    // being flown. Ahead = not behind the leg's start and not past its fix.
    auto nearestLegAhead = [&](int from) -> int {
        for (int fix = from; fix < n; ++fix) {
            double cross, along, len;
            if (!legOffsets(fix, cross, along, len)) continue;
            if (along < -m_config.offPlanNm) continue;
            if (pastFix(fix)) continue;
            if (std::fabs(cross) < m_config.offPlanNm) return fix;
        }
        return -1;
    };

    if (m_onPlan) {
        // Sequence: monotonic advance past abeam.
        while (m_activeFix < n && pastFix(m_activeFix)) {
            d.events.push_back("sequenced " + m_plan.fixes[m_activeFix].ident);
            ++m_activeFix;
            m_legLevelSeen = false;
        }
        if (m_activeFix >= n) {
            // Past the destination: the plan is flown out.
            m_onPlan = false;
            m_activeFix = -1;
            m_planComplete = true;
            d.events.push_back("plan complete: destination sequenced");
        } else {
            double cross, along, len;
            legOffsets(m_activeFix, cross, along, len);
            if (std::fabs(cross) > m_config.offPlanNm) {
                if (++m_offPlanTicks >= m_config.offPlanTicks) {
                    m_onPlan = false;
                    m_candidateFix = -1;
                    m_candidateTicks = 0;
                    char buf[96];
                    std::snprintf(buf, sizeof buf, "off plan: %.1f nm from the leg to %s",
                                  std::fabs(cross), m_plan.fixes[m_activeFix].ident.c_str());
                    d.events.push_back(buf);
                }
            } else {
                m_offPlanTicks = 0;
            }
        }
    }
    if (!m_onPlan && !m_planComplete) {
        int cand = nearestLegAhead(m_activeFix < 0 ? 0 : m_activeFix);
        if (cand < 0 && m_activeFix > 0) cand = nearestLegAhead(0);
        if (cand >= 0 && cand == m_candidateFix) {
            ++m_candidateTicks;
        } else {
            m_candidateFix = cand;
            m_candidateTicks = cand >= 0 ? 1 : 0;
        }
        if (cand >= 0 && m_candidateTicks >= m_config.offPlanTicks) {
            m_onPlan = true;
            m_offPlanTicks = 0;
            m_activeFix = cand;
            m_legLevelSeen = false;
            d.events.push_back("on plan: next fix " + m_plan.fixes[cand].ident);
        }
    }

    // --- Name the fix ---------------------------------------------------------
    if (m_onPlan) {
        d.stage = GovernorStage::OnPlan;
        const PlanFix &f = m_plan.fixes[m_activeFix];
        d.nextFixIdent = f.ident;
        d.distanceToFixNm = distanceNm(in.latDeg, in.lonDeg, f.latDeg, f.lonDeg);
        if (m_activeFix + 1 < n) {
            const PlanFix &g = m_plan.fixes[m_activeFix + 1];
            double outbound = bearingDeg(f.latDeg, f.lonDeg, g.latDeg, g.lonDeg);
            d.turnAngleDeg = std::fabs(angleDiff(outbound, in.groundTrackDeg));
        }
    } else {
        d.stage = m_plan.empty() ? GovernorStage::NoPlan : GovernorStage::OffPlan;
    }

    // --- Lead: where the predicted window opens ------------------------------
    // R = V^2 / (g tan(bank)) at live GS; roll-in = R tan(theta/2) + measured
    // slack; then the human's margin and the ticks the rate compresses.
    if (m_onPlan && d.turnAngleDeg >= 0.0) {
        double v = in.groundSpeedKt * kKtToMps;
        double rNm = v * v / (kG * std::tan(rad(m_config.bankLimitDeg))) / kMetresPerNm;
        double rollIn = rNm * std::tan(rad(d.turnAngleDeg) / 2.0) + m_config.rollInSlackNm;
        double allowance = in.groundSpeedKt * in.simRate * m_config.tickAllowanceTicks / 3600.0;
        d.leadNm = rollIn + m_settings.marginNm + allowance;
    }

    // --- Sensed-turn signal (T37 thresholds), read every tick ------------------
    const double simS = in.dtWall * in.simRate;
    double hdgRate = 0.0;
    if (m_haveHeading && simS > 0.0)
        hdgRate = std::fabs(angleDiff(in.headingDeg, m_lastHeading)) / simS;
    m_lastHeading = in.headingDeg;
    m_haveHeading = true;
    m_bankTicks = std::fabs(in.bankDeg) > m_config.bankOnsetDeg ? m_bankTicks + 1 : 0;
    const bool turning = m_bankTicks >= m_config.bankOnsetTicks ||
                         hdgRate > m_config.headingRateOnsetDegPerS;
    const bool level = std::fabs(in.bankDeg) < m_config.wingsLevelDeg;
    // Arming early wants the aircraft *established* on the leg - tracking its
    // course, give or take - and a *fresh* onset: a level tick, established,
    // seen on this leg before the bank. A turn that starts from 50 deg off the
    // leg is the intercept onto it (T37, 22:43: a left turn 322->277 fourteen
    // miles out to rejoin 270, ahead of the real 59 deg left turn at the fix),
    // and a turn already under way when the leg was acquired is the recovery
    // from an overshoot; neither is the turn at the fix. Direction alone cannot
    // tell them apart - the intercept and the fix turn can go the same way. The
    // predicted window is untouched by either guard: a DIR TO still opens on
    // geometry, from the live track.
    bool established = false;
    if (m_onPlan) {
        double la, lo;
        if (legStart(m_activeFix, la, lo)) {
            const PlanFix &f = m_plan.fixes[m_activeFix];
            double course = bearingDeg(la, lo, f.latDeg, f.lonDeg);
            established = std::fabs(angleDiff(in.groundTrackDeg, course)) < m_config.establishedDeg;
        }
    }
    if (m_onPlan && established && !turning) m_legLevelSeen = true;

    // --- Opening a window -----------------------------------------------------
    auto open = [&](WindowSource source) {
        m_windowOpen = true;
        m_source = source;
        m_windowFix = m_onPlan ? m_activeFix : -1;
        m_sequenced = false;
        m_turnSeen = source != WindowSource::Predicted;
        m_levelSimS = 0.0;
        m_windowSimS = 0.0;
        char buf[160];
        if (source == WindowSource::Predicted)
            std::snprintf(buf, sizeof buf, "turn window open: %s %.0f\xC2\xB0 in %.1f nm (lead %.1f nm, plan)",
                          d.nextFixIdent.c_str(), d.turnAngleDeg, d.distanceToFixNm, d.leadNm);
        else if (source == WindowSource::SensedEarly)
            std::snprintf(buf, sizeof buf, "turn window open: %s %.0f\xC2\xB0 in %.1f nm (sensed early, lead was %.1f nm)",
                          d.nextFixIdent.c_str(), d.turnAngleDeg, d.distanceToFixNm, d.leadNm);
        else
            std::snprintf(buf, sizeof buf, "turn window open: sensed turn (%s)",
                          m_plan.empty() ? "no plan" : "off plan");
        d.events.push_back(buf);
    };
    if (!m_windowOpen && m_onPlan && d.turnAngleDeg >= m_settings.angleThresholdDeg) {
        if (d.distanceToFixNm <= d.leadNm) open(WindowSource::Predicted);
        // Arm early: the bank arriving before the predicted window. Bounded to
        // twice the lead so a mid-leg correction (which also bends the live
        // track, and with it the angle) cannot open a window on a fix an hour
        // away.
        else if (turning && m_legLevelSeen && established && d.distanceToFixNm <= 2.0 * d.leadNm) open(WindowSource::SensedEarly);
    } else if (!m_windowOpen && !m_onPlan && m_settings.sensedTurnSource && turning) {
        // Plan-less: no plan or off it, the sensed turn is the source when its
        // setting says so.
        open(WindowSource::Sensed);
    }

    // --- Restore ------------------------------------------------------------
    if (m_windowOpen) {
        m_windowSimS += simS;
        if (turning) m_turnSeen = true;
        m_levelSimS = level ? m_levelSimS + simS : 0.0;
        if (m_windowFix >= 0 && (m_activeFix != m_windowFix || pastFix(m_windowFix))) m_sequenced = true;
        const char *reason = nullptr;
        if (m_windowSimS >= m_config.windowCapSimSeconds) {
            reason = "window cap";
        } else if (m_levelSimS >= m_config.wingsLevelSimSeconds) {
            if (m_source == WindowSource::Sensed) reason = "wings level";
            else if (m_sequenced && m_turnSeen) reason = "sequenced, wings level";
        }
        if (reason) {
            m_windowOpen = false;
            m_source = WindowSource::None;
            m_windowFix = -1;
            d.events.push_back(std::string("restore: ") + reason);
        }
    }

    if (m_windowOpen) {
        d.stage = GovernorStage::Turn;
        d.windowOpen = true;
        d.source = m_source;
        d.haveCeiling = true;
        d.rateCeiling = m_settings.turnRate;
    }
    return d;
}

} // namespace maxwarp
