#pragma once

#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariantMap>

#include <atomic>
#include <chrono>

#include "Corrector.h"
#include "LVarBridge.h"
#include "RateDriver.h"
#include "SessionLogger.h"
#include "TurnGovernor.h"

// Dedicated SimConnect worker thread per the T4 timing spec (issue #5):
// event-handle wait with a 1 s timeout; each 1 Hz SIMCONNECT_PERIOD_SECOND
// packet (rate + tanks + fuel flow + capacities, atomic) is the tick; dt is
// steady_clock between actual arrivals. Reconnect only on sim quit or a failed
// dispatch, retrying every 2 s. Telemetry silence beyond 5 s is reported as
// "stalled" — status only, never a reconnect.
//
// T8 (issue #9): the full T3 correction pipeline runs here, synchronously on the
// tick. The Corrector core is fed a TickInput built from each packet and its
// returned writes are issued immediately (same tick/thread). Session logging and
// the master switch live here too; the QML side stays presentation-only.
//
// T20 (issue #23): the MobiFlight L:var bridge rides on the same handle. Its
// values are latched into each tick alongside the telemetry (T18 keeps T4's 1 Hz
// packet as the tick) and its health is the corrector's fourth engage
// precondition.
//
// T21 (issue #24): core state is now the whole fuel data path. The bridge's kg
// quantities are the corrector's only fuel input and its signed kg deltas are
// the only writes; the simvar write path is deleted rather than disabled, so
// there is no writable simvar definition here at all. What still comes off the
// SimConnect packet is everything core state does not carry — rate, pause, slew,
// aircraft identity, tank capacities — plus the legacy quantities, which are
// logged as a cross-check and watched for drift, never consumed.
class SimConnectWorker : public QThread
{
    Q_OBJECT

public:
    static constexpr int kTankCount = 6;
    static const char *const kTankSimvars[kTankCount];
    static const char *const kTankLabels[kTankCount];
    static const maxwarp::TankRole kTankRoles[kTankCount];

    explicit SimConnectWorker(QObject *parent = nullptr);
    ~SimConnectWorker() override;

    void stop();

    // Master switch (T10). Thread-safe — the request is applied on the worker
    // thread at the next tick. A human toggles these; nothing else re-arms.
    void requestArm();
    void requestDisengage();

    // Sim-rate stepper (T9): each call transmits one SIM_RATE_INCR/DECR client
    // event — the T2-proven path — followed by a one-shot rate readback so the
    // UI updates ahead of the next 1 Hz packet. Thread-safe.
    void requestRateStep(bool up);

    // Turn governor (T39 · #58): the human's knobs and the SimBrief plan, from
    // the GUI thread. Latched under a mutex and applied on the worker thread at
    // the next tick — the m_masterRequest pattern, with payloads.
    void updateGovernorSettings(const maxwarp::GovernorSettings &settings);
    void updateGovernorPlan(const maxwarp::Plan &plan);

    // SimConnect dispatch entry; only the dispatch thunk in the .cpp calls this.
    void handleRecv(void *recv);

signals:
    void statusChanged(const QString &status); // connecting|connected|stalled|reconnecting
    void simInfoChanged(const QString &info);
    // Bridge stage of the status rail (T20): the fixed word, plus whether core
    // state is reachable right now. Emitted only when the state changes.
    void bridgeChanged(const QString &word, bool up);
    void tick(double simRate, double totalGallons, const QList<double> &tankGallons,
              const QList<double> &tankCaps, double lbsPerGal, double dtWallSeconds);
    void rateReadback(double simRate); // one-shot answer to a rate step
    // Corrector state for the UI (T10): state word, variant, last-event line,
    // session counters, and the master-switch position. Emitted every tick.
    void correctorUpdate(const QVariantMap &state);

protected:
    void run() override;

private:
    bool setupDefinitions();
    void closeSim();
    void interruptibleSleep(int ms);
    void applyMasterRequest();
    void applyRateRequests();
    void transmitRateSteps(int up, int down);
    void runGovernorTick(const maxwarp::GovernorInput &in);
    void runCorrectorTick(double simRate, double totalGallons,
                          const double *tankGallons, const double *tankCaps,
                          double newFuelSystem, double ff1, double ff2, double wpg,
                          bool slew, double dtWall);
    void pumpBridge();
    void checkCrossCheckDrift(const maxwarp::BridgeSample &bridge, const double *tankGallons);

    void *m_wakeEvent = nullptr; // HANDLE; created in the ctor so stop() can signal it
    void *m_hSim = nullptr;      // SimConnect HANDLE; owned by the worker loop
    bool m_openReceived = false;
    bool m_quitReceived = false;
    bool m_haveTick = false;
    bool m_paused = false; // driven by the SimConnect "Pause" system event
    QString m_title;       // latest TITLE (own PERIOD_SECOND stream)
    std::chrono::steady_clock::time_point m_lastPacket;
    std::chrono::steady_clock::time_point m_lastTick;

    maxwarp::Corrector m_corrector;
    maxwarp::LVarBridge m_bridge;
    maxwarp::BridgeState m_lastBridgeState = maxwarp::BridgeState::Down;
    QString m_bridgeEvent; // the bridge's own last words, for the last-event line
    QString m_crossCheckEvent; // core state vs the legacy mirror have drifted apart
    int m_divergentTicks = 0;
    bool m_divergenceReported = false;
    SessionLogger m_logger;
    // The pairing the open log file belongs to (TITLE). The log rotates when this
    // changes and at no other time (T24 · #34) — never on the detection edge,
    // which is neither the start nor the end of a pairing.
    std::string m_logPairing;
    std::atomic<int> m_masterRequest{-1}; // -1 none, 0 disengage, 1 arm
    std::atomic<int> m_pendingRateUp{0};
    std::atomic<int> m_pendingRateDown{0};

    // Turn governor (T39 · #58). The core and the rate driver live on the
    // worker thread; settings and plan arrive from the GUI thread through the
    // pending slots below. m_uiStepped marks a stepper press since the last
    // tick — the authoritative half of the human-step test (the other half is
    // the driver's expectation arithmetic).
    maxwarp::TurnGovernor m_governor;
    maxwarp::RateDriver m_rateDriver;
    maxwarp::GovernorInput m_govInput;      // last tick's adapted telemetry, for the log
    maxwarp::GovernorDecision m_govDecision; // last decision, for the map + log
    QStringList m_govTickEvents;            // this tick's governor events
    std::atomic<bool> m_uiStepped{false};
    QMutex m_govMutex;
    maxwarp::GovernorSettings m_pendingGovSettings;
    maxwarp::Plan m_pendingGovPlan;
    bool m_govSettingsDirty = false;
    bool m_govPlanDirty = false;
};
