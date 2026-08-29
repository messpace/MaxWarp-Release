#include "SimConnectWorker.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <SimConnect.h>

#include <QDebug>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using maxwarp::CorrectorState;

// Read-only, all of them: T21 moved every write onto the bridge, so the worker
// no longer defines a writable simvar at all.
enum DefinitionId : DWORD {
    DEF_TICK = 1,
    DEF_TITLE = 2,
    DEF_RATE = 3,
};
enum RequestId : DWORD { REQ_TICK = 1, REQ_TITLE = 2, REQ_RATE = 3 };
enum EventId : DWORD { EVT_PAUSE = 1, EVT_RATE_INCR = 2, EVT_RATE_DECR = 3 };

// Field order must match the AddToDataDefinition order in setupDefinitions().
#pragma pack(push, 1)
struct TickPacket {
    double simRate;
    double totalGallons;
    double tankGallons[SimConnectWorker::kTankCount];
    double tankCaps[SimConnectWorker::kTankCount];
    double newFuelSystem;
    double ff1;
    double ff2;
    double lbsPerGal;
    double slew;
    // Turn-governor telemetry (T39 · #58) — the T33 model's inputs, sampled
    // atomically with the rest of the tick. All proven live by the T31/T37
    // probes; the GPS WP block stays out (dead on the ini A330).
    double lat;
    double lon;
    double groundTrack;
    double groundSpeed;
    double onGround;
    double bank;
    double headingTrue;
};
struct TitlePacket {
    char title[256];
};
#pragma pack(pop)

void CALLBACK dispatchThunk(SIMCONNECT_RECV *pData, DWORD cbData, void *context)
{
    (void)cbData;
    static_cast<SimConnectWorker *>(context)->handleRecv(pData);
}

const char *exceptionName(DWORD code)
{
    switch (code) {
        case SIMCONNECT_EXCEPTION_ERROR: return "ERROR";
        case SIMCONNECT_EXCEPTION_SIZE_MISMATCH: return "SIZE_MISMATCH";
        case SIMCONNECT_EXCEPTION_UNRECOGNIZED_ID: return "UNRECOGNIZED_ID";
        case SIMCONNECT_EXCEPTION_NAME_UNRECOGNIZED: return "NAME_UNRECOGNIZED";
        case SIMCONNECT_EXCEPTION_DATA_ERROR: return "DATA_ERROR";
        case SIMCONNECT_EXCEPTION_OUT_OF_BOUNDS: return "OUT_OF_BOUNDS";
        default: return "?";
    }
}

constexpr double kKgPerLb = 0.45359237;
constexpr double kFallbackLbsPerGal = 6.699; // Jet-A; mirrors SimService

// The core propagates its own kg state to the paired legacy simvar at a measured
// 3.0396 kg/gal (T17) — a conversion internal to the aircraft, and not the same
// number as FUEL WEIGHT PER GALLON x kg/lb. Comparing the two on anything else
// would report a permanent 0.04% offset as drift.
constexpr double kCoreKgPerGallon = 3.0396;

// Cross-check divergence (T21). The mirror can lag core state by a whole one of
// the core's 1 Hz propagation steps — and lags furthest exactly when we have
// just written — while EXTERNAL1 carries a phantom +/-1.14 kg square wave. So a
// single-tick difference proves nothing. The threshold sits well above both
// (~0.1% of a full main) and the gap must persist, because the failures worth an
// event here — a frozen mirror, a renamed L:var, a subscription writing into a
// void — diverge without bound and cross any threshold within seconds.
constexpr double kDivergenceAbsKg = 25.0;
constexpr double kDivergenceFraction = 0.01;
constexpr int kDivergenceTicks = 5;

QString stateWord(CorrectorState s)
{
    switch (s) {
        case CorrectorState::Off: return QStringLiteral("OFF");
        case CorrectorState::Standby: return QStringLiteral("STANDBY");
        case CorrectorState::Correcting: return QStringLiteral("CORRECTING");
        case CorrectorState::Suspended: return QStringLiteral("SUSPENDED");
    }
    return QStringLiteral("?");
}

} // namespace

// T1/T7 tank map: inner mains, outer aux, trim = EXTERNAL1; CENTER populated on
// the -200 only. Legacy FUEL TANK vars, Gallons only (T2).
const char *const SimConnectWorker::kTankSimvars[kTankCount] = {
    "LEFT MAIN", "RIGHT MAIN", "LEFT AUX", "RIGHT AUX", "CENTER", "EXTERNAL1",
};
const char *const SimConnectWorker::kTankLabels[kTankCount] = {
    "Inner L", "Inner R", "Outer L", "Outer R", "Center", "Trim",
};
const maxwarp::TankRole SimConnectWorker::kTankRoles[kTankCount] = {
    maxwarp::TankRole::Main, maxwarp::TankRole::Main, maxwarp::TankRole::Aux,
    maxwarp::TankRole::Aux,  maxwarp::TankRole::Center, maxwarp::TankRole::Trim,
};

SimConnectWorker::SimConnectWorker(QObject *parent)
    : QThread(parent)
{
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

SimConnectWorker::~SimConnectWorker()
{
    stop();
    if (m_wakeEvent) {
        CloseHandle(static_cast<HANDLE>(m_wakeEvent));
        m_wakeEvent = nullptr;
    }
}

void SimConnectWorker::stop()
{
    if (!isRunning())
        return;
    requestInterruption();
    SetEvent(static_cast<HANDLE>(m_wakeEvent));
    wait(3000);
}

void SimConnectWorker::requestArm()
{
    m_masterRequest.store(1);
    SetEvent(static_cast<HANDLE>(m_wakeEvent));
}

void SimConnectWorker::requestDisengage()
{
    m_masterRequest.store(0);
    SetEvent(static_cast<HANDLE>(m_wakeEvent));
}

void SimConnectWorker::requestRateStep(bool up)
{
    (up ? m_pendingRateUp : m_pendingRateDown).fetch_add(1);
    // Mark the press for the governor's human-step test (T39) — authoritative
    // even when presses net out to the rate the driver already expected.
    m_uiStepped.store(true);
    SetEvent(static_cast<HANDLE>(m_wakeEvent));
}

void SimConnectWorker::updateGovernorSettings(const maxwarp::GovernorSettings &settings)
{
    QMutexLocker lock(&m_govMutex);
    m_pendingGovSettings = settings;
    m_govSettingsDirty = true;
}

void SimConnectWorker::updateGovernorPlan(const maxwarp::Plan &plan)
{
    QMutexLocker lock(&m_govMutex);
    m_pendingGovPlan = plan;
    m_govPlanDirty = true;
}

void SimConnectWorker::applyRateRequests()
{
    int up = m_pendingRateUp.exchange(0);
    int down = m_pendingRateDown.exchange(0);
    // The ceiling is a ceiling (T41 · #60). The QML `+` is disabled while a
    // window holds the rate down, but that binding only refreshes on
    // correctorUpdate, so it can be up to a tick stale at the moment a window
    // opens. Drop the press here rather than queue it: this runs on the worker
    // thread, the same thread as the driver, so `holding()` is the driver's
    // live state — and it is precisely the state that would step the press
    // back down on the next tick anyway. (Not m_govDecision.windowOpen: that
    // one latches across a pause.)
    if (m_rateDriver.holding())
        up = 0;
    transmitRateSteps(up, down);
}

void SimConnectWorker::transmitRateSteps(int up, int down)
{
    if ((!up && !down) || !m_hSim || !m_openReceived)
        return;
    HANDLE h = static_cast<HANDLE>(m_hSim);
    auto send = [&](DWORD evt, int n) {
        for (int i = 0; i < n; ++i)
            SimConnect_TransmitClientEvent(h, SIMCONNECT_OBJECT_ID_USER, evt, 0,
                                           SIMCONNECT_GROUP_PRIORITY_HIGHEST,
                                           SIMCONNECT_EVENT_FLAG_GROUPID_IS_PRIORITY);
    };
    send(EVT_RATE_INCR, up);
    send(EVT_RATE_DECR, down);
    // One-shot readback so the hero numeral answers ahead of the next 1 Hz packet.
    SimConnect_RequestDataOnSimObject(h, REQ_RATE, DEF_RATE, SIMCONNECT_OBJECT_ID_USER,
                                      SIMCONNECT_PERIOD_ONCE,
                                      SIMCONNECT_DATA_REQUEST_FLAG_DEFAULT, 0, 0, 0);
}

// The governor's tick (T39 · #58): runs on the packet, right before the
// corrector's, so its decision rides the same correctorUpdate to the UI. The
// order inside is the wiring contract — reconcile the rate *before* the core
// hears the tick (the driver's expectation must be current), drive the stepper *after* it (the
// ceiling is an output). T37 measured three back-to-back DECRs answered in one
// frame, so a whole descent goes out in one burst.
void SimConnectWorker::runGovernorTick(const maxwarp::GovernorInput &telemetry)
{
    m_govTickEvents.clear();

    // Settings and plan from the GUI thread, applied on this side of the tick.
    {
        QMutexLocker lock(&m_govMutex);
        if (m_govSettingsDirty) {
            m_governor.setSettings(m_pendingGovSettings);
            m_govSettingsDirty = false;
        }
        if (m_govPlanDirty) {
            if (m_pendingGovPlan.empty())
                m_governor.clearPlan();
            else
                m_governor.setPlan(m_pendingGovPlan);
            m_govPlanDirty = false;
        }
    }

    // The governor switch is the only escape from a turn window, and it takes
    // effect where it is thrown. Switching it off applies just above but does
    // not reach onTick while paused, so the driver would go on holding — and
    // the sim reports menus and loading screens as paused too, so "until the
    // next live tick" can be a long way off. Release the driver here instead:
    // the hold and the owed restore go, nothing is transmitted, and the held
    // rate becomes the cruise rate. This is what makes the `+` live again —
    // applyRateRequests() drops a press only while the driver is holding.
    if (!m_governor.settings().enabled && m_rateDriver.holding())
        m_rateDriver.release();

    // A paused sim streams frozen packets: dtWall keeps flowing while no
    // sim-second passes, so ticking the core would age its windows against
    // time the aircraft never flew. Everything stays latched — a window frozen
    // mid-turn really is still open, so the locked `+` and the held hero match
    // reality — and a rate the human changes from the pause menu is reconciled
    // on the first live tick.
    if (m_paused) {
        // One exception: the governor switch is the only escape from a window,
        // and it must work while paused too. Switching it off applies above but
        // never reaches onTick, so the core's closeWindow("governor off") is
        // owed until the sim runs again. Publish the stand-down now rather than
        // leave the `+` dead and the hero claiming a held state the human just
        // cancelled; the core closes its own window on its first live tick.
        if (!m_governor.settings().enabled && m_govDecision.windowOpen)
            m_govDecision = maxwarp::GovernorDecision{};
        return;
    }

    maxwarp::GovernorInput in = telemetry;
    // T41 (#60): the core no longer hears the human; reconcile keeps the
    // driver's expectation and cruise rate honest before the ceiling is applied.
    // Its bool — "the human moved the cruise rate" — is a test and diagnostic
    // signal only, deliberately discarded: no production code acts on it now
    // that the core no longer hears the human.
    m_rateDriver.reconcile(in.simRate, m_uiStepped.exchange(false));

    m_govInput = in;
    m_govDecision = m_governor.onTick(in);

    const maxwarp::RateSteps steps =
        m_rateDriver.drive(m_govDecision.haveCeiling, m_govDecision.rateCeiling);
    transmitRateSteps(steps.up, steps.down);

    for (const auto &e : m_govDecision.events) {
        const QString line = QStringLiteral("governor: ") + QString::fromStdString(e);
        m_govTickEvents.append(line);
        qInfo().noquote() << "event:" << line;
    }
}

void SimConnectWorker::applyMasterRequest()
{
    const int req = m_masterRequest.exchange(-1);
    if (req == 1)
        m_corrector.arm();
    else if (req == 0)
        m_corrector.disengage();
}

void SimConnectWorker::run()
{
    emit statusChanged(QStringLiteral("connecting"));

    bool stalled = false;
    while (!isInterruptionRequested()) {
        if (!m_hSim) {
            HANDLE h = nullptr;
            if (SUCCEEDED(SimConnect_Open(&h, "MaxWarp", nullptr, 0,
                                          static_cast<HANDLE>(m_wakeEvent), 0)) && h) {
                m_hSim = h;
                m_openReceived = false;
                m_quitReceived = false;
                m_haveTick = false;
                stalled = false;
                if (!setupDefinitions()) {
                    qWarning() << "SimConnect data definition setup failed - retrying";
                    closeSim();
                } else {
                    // The bridge shares the handle; its handshake starts here and
                    // runs independently of the telemetry stream (T20).
                    m_bridge.attach(m_hSim);
                    m_lastBridgeState = maxwarp::BridgeState::Down;
                    emit bridgeChanged(QString::fromLatin1(m_bridge.stateWord()), false);
                }
            }
            if (!m_hSim) {
                interruptibleSleep(2000);
                continue;
            }
        }

        const DWORD wait = WaitForSingleObject(static_cast<HANDLE>(m_wakeEvent), 1000);
        if (isInterruptionRequested())
            break;
        if (wait == WAIT_OBJECT_0) {
            if (FAILED(SimConnect_CallDispatch(static_cast<HANDLE>(m_hSim), dispatchThunk,
                                               this))) {
                m_quitReceived = true; // treat a broken dispatch like a sim quit
            }
        }

        if (m_quitReceived) {
            closeSim();
            emit statusChanged(QStringLiteral("reconnecting"));
            interruptibleSleep(2000);
            continue;
        }

        applyRateRequests();
        pumpBridge();

        if (m_openReceived && m_haveTick) {
            const bool nowStalled = Clock::now() - m_lastPacket > std::chrono::seconds(5);
            if (nowStalled != stalled) {
                stalled = nowStalled;
                emit statusChanged(stalled ? QStringLiteral("stalled")
                                           : QStringLiteral("connected"));
            }
        }
    }

    closeSim();
}

bool SimConnectWorker::setupDefinitions()
{
    HANDLE h = static_cast<HANDLE>(m_hSim);
    auto add = [&](DWORD def, const char *name, const char *units) {
        return SimConnect_AddToDataDefinition(h, def, name, units,
                                              SIMCONNECT_DATATYPE_FLOAT64);
    };

    // DEF_TICK — everything the corrector needs each tick, sampled atomically.
    if (FAILED(add(DEF_TICK, "SIMULATION RATE", nullptr)))
        return false;
    if (FAILED(add(DEF_TICK, "FUEL TOTAL QUANTITY", "Gallons")))
        return false;
    char name[64];
    for (int i = 0; i < kTankCount; ++i) {
        std::snprintf(name, sizeof(name), "FUEL TANK %s QUANTITY", kTankSimvars[i]);
        if (FAILED(add(DEF_TICK, name, "Gallons")))
            return false;
    }
    for (int i = 0; i < kTankCount; ++i) {
        std::snprintf(name, sizeof(name), "FUEL TANK %s CAPACITY", kTankSimvars[i]);
        if (FAILED(add(DEF_TICK, name, "Gallons")))
            return false;
    }
    if (FAILED(add(DEF_TICK, "NEW FUEL SYSTEM", "Bool")))
        return false;
    // Fuel flow rides in the same definition (T4). If the ini's custom engines
    // don't expose it these throw NAME_UNRECOGNIZED; the corrector's clamp then
    // self-disables (FF <= 0) rather than freezing.
    add(DEF_TICK, "ENG FUEL FLOW PPH:1", "Pounds per hour");
    add(DEF_TICK, "ENG FUEL FLOW PPH:2", "Pounds per hour");
    if (FAILED(add(DEF_TICK, "FUEL WEIGHT PER GALLON", "Pounds")))
        return false;
    if (FAILED(add(DEF_TICK, "IS SLEW ACTIVE", "Bool")))
        return false;
    // Turn-governor telemetry (T39): position, track, speed and attitude for
    // the T33 model. Standard simvars, accepted by SimConnect 12.2 (T31/T37).
    if (FAILED(add(DEF_TICK, "PLANE LATITUDE", "Degrees")))
        return false;
    if (FAILED(add(DEF_TICK, "PLANE LONGITUDE", "Degrees")))
        return false;
    if (FAILED(add(DEF_TICK, "GPS GROUND TRUE TRACK", "Degrees")))
        return false;
    if (FAILED(add(DEF_TICK, "GROUND VELOCITY", "Knots")))
        return false;
    if (FAILED(add(DEF_TICK, "SIM ON GROUND", "Bool")))
        return false;
    if (FAILED(add(DEF_TICK, "PLANE BANK DEGREES", "Degrees")))
        return false;
    if (FAILED(add(DEF_TICK, "PLANE HEADING DEGREES TRUE", "Degrees")))
        return false;

    // DEF_TITLE — the one string field, its own stream so DEF_TICK stays a flat
    // packed double array.
    if (FAILED(SimConnect_AddToDataDefinition(h, DEF_TITLE, "TITLE", nullptr,
                                              SIMCONNECT_DATATYPE_STRING256)))
        return false;

    // DEF_RATE — rate alone, for the one-shot readback after a stepper event.
    if (FAILED(add(DEF_RATE, "SIMULATION RATE", nullptr)))
        return false;
    SimConnect_MapClientEventToSimEvent(h, EVT_RATE_INCR, "SIM_RATE_INCR");
    SimConnect_MapClientEventToSimEvent(h, EVT_RATE_DECR, "SIM_RATE_DECR");

    // No per-tank write definitions: T2's simvar write path is deleted, not
    // disabled (T21). Corrections go to core state through the bridge, and T18
    // ruled out keeping an external flush path as a fallback — with no bridge,
    // MaxWarp declines to engage and says why.

    HRESULT hr = SimConnect_RequestDataOnSimObject(h, REQ_TICK, DEF_TICK,
                                                   SIMCONNECT_OBJECT_ID_USER,
                                                   SIMCONNECT_PERIOD_SECOND,
                                                   SIMCONNECT_DATA_REQUEST_FLAG_DEFAULT, 0, 0, 0);
    if (FAILED(hr))
        return false;
    SimConnect_RequestDataOnSimObject(h, REQ_TITLE, DEF_TITLE, SIMCONNECT_OBJECT_ID_USER,
                                      SIMCONNECT_PERIOD_SECOND,
                                      SIMCONNECT_DATA_REQUEST_FLAG_DEFAULT, 0, 0, 0);
    // Layered state detection (T3 GATE 2). Pause_EX1 is a bitmask; any non-zero
    // value is a pause of some kind.
    SimConnect_SubscribeToSystemEvent(h, EVT_PAUSE, "Pause_EX1");
    return true;
}

// Drive the bridge's handshake, retries and staleness detection, and republish
// its state when it moves. `simActive` gates the staleness check: the L:var
// subscription streams on change, so silence proves nothing while the sim is
// paused or the telemetry itself has stopped.
void SimConnectWorker::pumpBridge()
{
    if (!m_hSim || !m_openReceived)
        return;

    const bool simActive =
        m_haveTick && !m_paused && Clock::now() - m_lastPacket < std::chrono::seconds(5);
    m_bridge.poll(simActive);

    // Held until the next tick publishes it, so the last-event line advances once
    // per genuine transition rather than every tick the bridge stays down (T10).
    for (const auto &e : m_bridge.takeEvents()) {
        m_bridgeEvent = QString::fromStdString(e);
        qInfo().noquote() << "event:" << m_bridgeEvent;
    }

    const maxwarp::BridgeState now = m_bridge.state();
    if (now != m_lastBridgeState) {
        m_lastBridgeState = now;
        emit bridgeChanged(QString::fromLatin1(m_bridge.stateWord()), m_bridge.isReady());
    }
}

// Cross-check drift (T21). The legacy simvars mirror core state at a fixed
// 3.0396 kg/gal (T17); a gap that opens and stays open means one of the two has
// stopped tracking the aircraft, which is worth saying out loud even though
// nothing here feeds a correction.
void SimConnectWorker::checkCrossCheckDrift(const maxwarp::BridgeSample &bridge,
                                            const double *tankGallons)
{
    if (!bridge.up) {
        m_divergentTicks = 0;
        m_divergenceReported = false;
        return;
    }

    int worstSlot = -1;
    double worstGap = 0.0;
    for (int i = 0; i < kTankCount; ++i) {
        if (!bridge.resolved[i])
            continue;
        const double gap = std::fabs(bridge.tankKg[i] - tankGallons[i] * kCoreKgPerGallon);
        const double tolerance =
            std::max(kDivergenceAbsKg, kDivergenceFraction * std::fabs(bridge.tankKg[i]));
        if (gap > tolerance && gap > worstGap) {
            worstGap = gap;
            worstSlot = i;
        }
    }

    if (worstSlot < 0) {
        m_divergentTicks = 0;
        m_divergenceReported = false;
        return;
    }
    // One event per episode, not one per tick: this reports a condition, and it
    // clears itself above the moment the tanks agree again.
    if (++m_divergentTicks < kDivergenceTicks || m_divergenceReported)
        return;
    m_divergenceReported = true;
    m_crossCheckEvent = QStringLiteral("cross-check drift — %1 reads %2 kg, mirror says %3 kg")
                            .arg(QString::fromLatin1(kTankLabels[worstSlot]),
                                 QString::number(bridge.tankKg[worstSlot], 'f', 1),
                                 QString::number(tankGallons[worstSlot] * kCoreKgPerGallon,
                                                 'f', 1));
    qWarning().noquote() << "event:" << m_crossCheckEvent;
}

void SimConnectWorker::runCorrectorTick(double simRate, double totalGallons,
                                        const double *tankGallons, const double *tankCaps,
                                        double newFuelSystem, double ff1, double ff2,
                                        double wpg, bool slew, double dtWall)
{
    applyMasterRequest();

    // Core state as the bridge last saw it, latched for this tick (T18: the 1 Hz
    // SimConnect packet stays the tick, the L:vars ride into it). This is the
    // corrector's only fuel input — the legacy simvars below never reach it.
    const maxwarp::BridgeSample bridge = m_bridge.sample();

    const double kgPerGal = (wpg > 0.1 ? wpg : kFallbackLbsPerGal) * kKgPerLb;

    maxwarp::TickInput in;
    in.dtWall = dtWall;
    in.simRate = simRate;
    in.coreFuelUsedKg = bridge.fuelUsedKg;
    in.haveCoreFuelUsed = bridge.haveFuelUsed;
    in.coreRefueling = bridge.refueling;
    in.paused = m_paused;
    in.slew = slew;
    in.bridgeUp = m_bridge.isReady(); // fourth engage precondition (T20)
    in.aircraft.title = m_title.toStdString();
    in.aircraft.legacyFuelSystem = (newFuelSystem == 0.0);
    in.tanks.reserve(kTankCount);
    for (int i = 0; i < kTankCount; ++i) {
        // Quantity is core state and is converted from nothing. Capacity is a
        // structural figure only the simvars carry, so it is converted here — a
        // ceiling for the per-tank clamp, and the ground of both the fingerprint
        // and the variant. It is never a quantity, and since T24 (#34) it is
        // never gated on the bridge either: an airframe does not stop having a
        // tank because we briefly cannot read it.
        //
        // Fusing the two was the bug T23 (#31) diagnosed. A re-establish — which
        // T20 (#23) proved a flight load forces, catching two 5.1 s apart — zeroed
        // every capacity, so the fingerprint failed, the pairing ended, and the
        // re-lock wiped the session counters and voided T19's realised anchor,
        // mid-flight and on any variant. Whether we can *read* a slot now rides
        // on its own flag, and only the sites that ask that question read it.
        in.tanks.push_back({kTankRoles[i], bridge.tankKg[i], tankCaps[i] * kgPerGal,
                            bridge.resolved[i]});
    }

    const maxwarp::TickDecision decision = m_corrector.onTick(in);

    // The legacy simvars: still read, still logged, never consumed (CONTEXT.md,
    // "Cross-check"). A drift between them and core state means something is
    // wrong, and is worth an event.
    CrossCheck cross;
    for (int i = 0; i < kTankCount; ++i)
        cross.tankGallons[i] = tankGallons[i];
    cross.totalGallons = totalGallons;
    cross.ffPph1 = ff1;
    cross.ffPph2 = ff2;
    cross.kgPerGallon = kgPerGal;
    checkCrossCheckDrift(bridge, tankGallons);

    // Per-pairing session log (T10) — keyed on the pairing's identity since T24
    // (#34), not on the detection edge. The log's life is the *pairing's* life:
    // it rotates when a different airframe is paired with, records straight
    // through a gap in which the aircraft went unseen, and closes at shutdown
    // (`begin` closes the outgoing file, the destructor the last one).
    //
    // Both halves of the old rule were wrong, and T23's evidence caught each.
    // Opening on the `active` edge meant a variant flip — which never drops
    // `active` — kept writing into the previous pairing's file: 2026-07-28_2337
    // holds two `session started` events and did not rotate, so one file carries
    // two pairings under the wrong name. Closing on it meant the losing tick's
    // row was skipped for being inactive and *then* the file closed, which is why
    // `aircraft lost` appears in no capture ever taken.
    const std::string &pairing = decision.counters.title;
    if (!pairing.empty() && pairing != m_logPairing) {
        std::vector<std::string> labels;
        for (int i = 0; i < kTankCount; ++i)
            labels.push_back(kTankLabels[i]);
        m_logger.begin(decision.counters.variant, labels);
        m_logPairing = pairing;
    }
    if (m_logger.isOpen()) {
        std::string govEvents;
        for (const QString &e : m_govTickEvents) {
            if (!govEvents.empty())
                govEvents += " | ";
            govEvents += e.toStdString();
        }
        m_logger.writeRow(in, decision, bridge, cross, m_govInput, m_govDecision,
                          m_rateDriver.cruiseRate(), govEvents);
    }

    // Issue writes immediately, same tick/thread — signed kg deltas applied by
    // the bridge as one atomic in-sim read-modify-write (T17), so nothing out
    // here races the core's own 1 Hz integration step.
    for (const auto &w : decision.writes)
        m_bridge.writeTankDeltaKg(w.tankIndex, w.deltaKg);

    for (const auto &e : decision.events)
        qInfo().noquote() << "event:" << QString::fromStdString(e);

    QVariantMap m;
    m[QStringLiteral("state")] = stateWord(decision.state);
    m[QStringLiteral("variant")] = QString::fromStdString(decision.counters.variant);
    m[QStringLiteral("sessionActive")] = decision.counters.active;
    // The bridge's own words take the line when it has news: the corrector's
    // reason for a bridge stand-down is deliberately generic, and *which* way the
    // bridge is unavailable is the part with a different fix behind it (T18).
    // The governor outranks the corrector here (T39): at 1× the corrector's
    // stand-down/re-engage pair is the *consequence* of a turn window, and the
    // window is the event with the story in it.
    if (!m_bridgeEvent.isEmpty()) {
        m[QStringLiteral("lastEvent")] = m_bridgeEvent;
        m_bridgeEvent.clear();
    } else if (!m_crossCheckEvent.isEmpty()) {
        m[QStringLiteral("lastEvent")] = m_crossCheckEvent;
        m_crossCheckEvent.clear();
    } else if (!m_govTickEvents.isEmpty()) {
        m[QStringLiteral("lastEvent")] = m_govTickEvents.last();
    } else if (!decision.events.empty()) {
        m[QStringLiteral("lastEvent")] = QString::fromStdString(decision.events.back());
    }
    // Per-tank display rates (T9): the natural delta the engaged loop measured,
    // plus what each write added on top (clamps included). Both kg/tick now — the
    // UI's conversion from gallons went away with the corrector's.
    if (!decision.observedDeltaKg.empty()) {
        QVariantList obs, extra;
        std::vector<double> written(kTankCount, 0.0);
        for (const auto &w : decision.writes)
            written[w.tankIndex] = w.deltaKg;
        for (int i = 0; i < kTankCount; ++i) {
            obs.append(decision.observedDeltaKg[i]);
            extra.append(written[i]);
        }
        m[QStringLiteral("obsDeltaKg")] = obs;
        m[QStringLiteral("cmdExtraKg")] = extra;
    }
    // Per-tank quantities the corrector actually acted on. The cards showed the
    // simvar mirror converted at FUEL WEIGHT PER GALLON, which agrees to ~0.03%
    // — except on trim, where EXTERNAL1's phantom +/-1.14 kg oscillation (T17)
    // was visibly flickering the card's last digit. Core state has no such
    // artefact, and it is what the corrector is driving.
    if (bridge.up) {
        QVariantList coreKg;
        double totalKg = 0.0;
        for (int i = 0; i < kTankCount; ++i) {
            // Blank where the name never resolved, so the UI keeps the mirror
            // rather than showing a confident zero.
            coreKg.append(bridge.resolved[i] ? QVariant(bridge.tankKg[i]) : QVariant());
            if (bridge.resolved[i])
                totalKg += bridge.tankKg[i];
        }
        m[QStringLiteral("coreKg")] = coreKg;
        m[QStringLiteral("coreTotalKg")] = totalKg;
    }
    m[QStringLiteral("removedCommandedKg")] = decision.counters.removedCommandedKg;
    m[QStringLiteral("removedRealisedKg")] = decision.counters.removedRealisedKg;
    m[QStringLiteral("removedMeasuredKg")] = decision.counters.removedMeasuredKg;
    m[QStringLiteral("burnObservedKg")] = decision.counters.burnObservedKg;
    m[QStringLiteral("burnEffectiveKg")] = decision.counters.burnEffectiveKg;
    m[QStringLiteral("elapsedSec")] = decision.counters.elapsedSeconds;
    m[QStringLiteral("rEff")] = decision.rEff;
    m[QStringLiteral("writeCount")] = static_cast<int>(decision.writes.size());
    m[QStringLiteral("masterAuto")] =
        (m_corrector.masterSwitch() == maxwarp::MasterSwitch::Auto);
    // The governor stage and the hero's held state (T39): the rail's fixed
    // words, the named fix, and the cruise rate a restore returns to.
    m[QStringLiteral("govWord")] = QString::fromStdString(m_govDecision.stageWord());
    m[QStringLiteral("govSub")] = QString::fromStdString(m_govDecision.stageSub());
    m[QStringLiteral("govFix")] = QString::fromStdString(m_govDecision.nextFixIdent);
    m[QStringLiteral("govWindowOpen")] = m_govDecision.windowOpen;
    m[QStringLiteral("govCruise")] = m_rateDriver.cruiseRate();
    emit correctorUpdate(m);
}

void SimConnectWorker::handleRecv(void *recv)
{
    auto *pData = static_cast<SIMCONNECT_RECV *>(recv);
    switch (pData->dwID) {
        case SIMCONNECT_RECV_ID_OPEN: {
            auto *open = reinterpret_cast<SIMCONNECT_RECV_OPEN *>(pData);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s %u.%u build %u.%u (SimConnect %u.%u)",
                          open->szApplicationName,
                          open->dwApplicationVersionMajor, open->dwApplicationVersionMinor,
                          open->dwApplicationBuildMajor, open->dwApplicationBuildMinor,
                          open->dwSimConnectVersionMajor, open->dwSimConnectVersionMinor);
            m_openReceived = true;
            emit simInfoChanged(QString::fromLatin1(buf));
            emit statusChanged(QStringLiteral("connected"));
            break;
        }
        case SIMCONNECT_RECV_ID_EVENT: {
            auto *evt = reinterpret_cast<SIMCONNECT_RECV_EVENT *>(pData);
            if (evt->uEventID == EVT_PAUSE)
                m_paused = (evt->dwData != 0);
            break;
        }
        case SIMCONNECT_RECV_ID_SIMOBJECT_DATA: {
            auto *obj = reinterpret_cast<SIMCONNECT_RECV_SIMOBJECT_DATA *>(pData);
            if (obj->dwRequestID == REQ_TITLE) {
                TitlePacket title{};
                std::memcpy(&title, &obj->dwData, sizeof(TitlePacket));
                title.title[sizeof(title.title) - 1] = '\0';
                const QString next = QString::fromLatin1(title.title);
                // A changed TITLE means the flight or aircraft reloaded. A
                // subscription that spans a load is poisoned: its definitions are
                // invalidated when the load finishes and every write after that
                // lands nowhere, with no error at all (T17). Re-establish.
                if (!m_title.isEmpty() && next != m_title) {
                    m_bridge.noteAircraftReload();
                    // Same gap as a reconnect (f21), without the disconnect: the
                    // old flight's cruise rate and any owed restore would follow
                    // the human into a new aircraft and fire a burst of INCRs at
                    // the gate. Start the new flight at first-boot, adopting
                    // whatever rate it wakes at, and publish nothing held.
                    m_rateDriver.reset();
                    m_govDecision = maxwarp::GovernorDecision{};
                }
                m_title = next;
                break;
            }
            if (obj->dwRequestID == REQ_RATE) {
                double rate = 0.0;
                std::memcpy(&rate, &obj->dwData, sizeof(double));
                emit rateReadback(rate);
                break;
            }
            if (obj->dwRequestID != REQ_TICK)
                break;
            TickPacket packet{};
            std::memcpy(&packet, &obj->dwData, sizeof(TickPacket));
            const auto now = Clock::now();
            m_lastPacket = now;
            const double dt = m_haveTick
                ? std::chrono::duration<double>(now - m_lastTick).count()
                : 0.0;
            m_lastTick = now;
            m_haveTick = true;

            QList<double> tanks;
            tanks.reserve(kTankCount);
            for (double gallons : packet.tankGallons)
                tanks.append(gallons);
            QList<double> caps;
            caps.reserve(kTankCount);
            for (double cap : packet.tankCaps)
                caps.append(cap);
            emit tick(packet.simRate, packet.totalGallons, tanks, caps, packet.lbsPerGal,
                      dt);

            maxwarp::GovernorInput gov;
            gov.dtWall = dt;
            gov.simRate = packet.simRate;
            gov.onGround = packet.onGround != 0.0;
            gov.latDeg = packet.lat;
            gov.lonDeg = packet.lon;
            gov.groundTrackDeg = packet.groundTrack;
            gov.groundSpeedKt = packet.groundSpeed;
            gov.bankDeg = packet.bank;
            gov.headingDeg = packet.headingTrue;
            runGovernorTick(gov);

            runCorrectorTick(packet.simRate, packet.totalGallons, packet.tankGallons,
                             packet.tankCaps, packet.newFuelSystem, packet.ff1, packet.ff2,
                             packet.lbsPerGal, packet.slew != 0.0, dt);
            break;
        }
        case SIMCONNECT_RECV_ID_CLIENT_DATA:
            m_bridge.handleClientData(pData);
            break;
        case SIMCONNECT_RECV_ID_EXCEPTION: {
            auto *ex = reinterpret_cast<SIMCONNECT_RECV_EXCEPTION *>(pData);
            qWarning() << "SimConnect exception" << ex->dwException
                       << exceptionName(ex->dwException) << "index" << ex->dwIndex;
            m_bridge.noteException(ex->dwException, ex->dwSendID);
            break;
        }
        case SIMCONNECT_RECV_ID_QUIT:
            m_quitReceived = true;
            break;
        default:
            break;
    }
}

void SimConnectWorker::closeSim()
{
    m_bridge.detach();
    if (m_lastBridgeState != maxwarp::BridgeState::Down) {
        m_lastBridgeState = maxwarp::BridgeState::Down;
        emit bridgeChanged(QString::fromLatin1(bridgeStateWord(m_lastBridgeState)), false);
    }
    m_bridgeEvent.clear();
    m_crossCheckEvent.clear();
    m_govTickEvents.clear();
    m_divergentTicks = 0;
    m_divergenceReported = false;
    if (m_hSim) {
        SimConnect_Close(static_cast<HANDLE>(m_hSim));
        m_hSim = nullptr;
    }
    m_openReceived = false;
    m_haveTick = false;
    m_paused = false;
    m_title.clear();
    // The worker object outlives the connection — run() loops and reopens on the
    // same instance — so the driver's cross-session state would ride a reconnect
    // into a flight that never asked for it: a window open at 8× cruise would
    // leave the driver holding with a restore owed, and the first tick of a
    // fresh session at the gate would fire it, stepping an untouched flight up
    // by itself. reset() puts the driver back to first-boot, so the next
    // session adopts whatever rate the sim wakes at. The published decision goes
    // with it, so nothing shows a held state across the gap (f19/f21). The plan
    // deliberately stays: SimService only re-pushes it on a settings/plan change
    // or start(), so clearing it here would strand the reconnect at NO PLAN —
    // and the core's own on-ground branch closes the window on the first packet.
    m_rateDriver.reset();
    m_govDecision = maxwarp::GovernorDecision{};
    // Losing the sim itself is not a detection gap — there is no stream left to
    // record through — so the file closes here and a reconnect opens a fresh one.
    // Clearing the key is what makes it reopen: without it the reconnected worker
    // would match the pairing it already had and never call begin() again, and
    // the log would go silent for the rest of the flight. The counters are the
    // corrector's and are untouched, so a reconnect onto the same airframe
    // resumes the pairing across two files — as it already did before T24.
    m_logger.end();
    m_logPairing.clear();
}

void SimConnectWorker::interruptibleSleep(int ms)
{
    for (int slept = 0; slept < ms && !isInterruptionRequested(); slept += 100)
        msleep(100);
}
