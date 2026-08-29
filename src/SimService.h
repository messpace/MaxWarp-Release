#pragma once

#include <QObject>
#include <QVariantList>
#include <QVector>

#include "SimBriefPlan.h"
#include "TurnGovernor.h"

class SimBriefClient;
class SimConnectWorker;

// GUI-thread facade over the SimConnect worker: receives queued worker signals
// and exposes them as QML properties. The corrector pipeline (T3) lives on the
// worker side; this class stays presentation-only — unit conversion, per-tank
// display rates, and formatting inputs for the round-1 screen (T9).
class SimService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString simInfo READ simInfo NOTIFY simInfoChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY statusChanged)
    Q_PROPERTY(double simRate READ simRate NOTIFY tickReceived)
    Q_PROPERTY(double totalGallons READ totalGallons NOTIFY tickReceived)
    // Fuel on board in kg: core state while the bridge is up (the authority the
    // corrector drives), the converted simvar total otherwise.
    Q_PROPERTY(double totalKg READ totalKg NOTIFY tickReceived)
    Q_PROPERTY(double lastDt READ lastDt NOTIFY tickReceived)
    Q_PROPERTY(double kgPerGallon READ kgPerGallon NOTIFY tickReceived)
    // Per tank: label, simvar, gallons, capacity, kg, pct, obsKgS, cmdKgS.
    Q_PROPERTY(QVariantList tanks READ tanks NOTIFY tickReceived)

    // Corrector surface (T8/T10) — bound by the round-1 QML screen (T9).
    // Bridge stage (T20): the rail word and whether core state is reachable.
    Q_PROPERTY(QString bridgeWord READ bridgeWord NOTIFY bridgeChanged)
    Q_PROPERTY(bool bridgeUp READ bridgeUp NOTIFY bridgeChanged)

    Q_PROPERTY(QString correctorState READ correctorState NOTIFY correctorChanged)
    Q_PROPERTY(QString aircraftVariant READ aircraftVariant NOTIFY correctorChanged)
    Q_PROPERTY(bool sessionActive READ sessionActive NOTIFY correctorChanged)
    Q_PROPERTY(QString lastEvent READ lastEvent NOTIFY correctorChanged)
    Q_PROPERTY(QString lastEventTime READ lastEventTime NOTIFY correctorChanged)
    Q_PROPERTY(bool masterAuto READ masterAuto NOTIFY correctorChanged)
    Q_PROPERTY(double effectiveRate READ effectiveRate NOTIFY correctorChanged)
    // Session counters, in kg since the T21 port. T19 (#21) split removal into
    // realised and commanded and named both, so the screen can never again show
    // a commanded figure as fuel that has left the aircraft: `removedRealisedKg`
    // is the headline, with `removedCommandedKg` beside it and
    // `removedMeasuredKg` as the denominator that makes their ratio honest.
    // The burn figures remain commanded arithmetic and say so.
    Q_PROPERTY(double removedRealisedKg READ removedRealisedKg NOTIFY correctorChanged)
    Q_PROPERTY(double removedCommandedKg READ removedCommandedKg NOTIFY correctorChanged)
    Q_PROPERTY(double removedMeasuredKg READ removedMeasuredKg NOTIFY correctorChanged)
    Q_PROPERTY(double burnObservedKg READ burnObservedKg NOTIFY correctorChanged)
    Q_PROPERTY(double burnEffectiveKg READ burnEffectiveKg NOTIFY correctorChanged)
    Q_PROPERTY(double elapsedSeconds READ elapsedSeconds NOTIFY correctorChanged)

    // Turn-governor settings (T36 · #55): the human's knobs from the T34 page,
    // persisted in QSettings beside `theme/mode` (ThemeController's pattern).
    // Held as the same `GovernorSettings` the core takes, so T39 has one struct
    // to hand the worker and T38 one place to find the identity. Nothing
    // downstream reads them yet.
    Q_PROPERTY(bool governorEnabled READ governorEnabled WRITE setGovernorEnabled NOTIFY settingsChanged)
    Q_PROPERTY(double turnRate READ turnRate WRITE setTurnRate NOTIFY settingsChanged)
    Q_PROPERTY(double angleThresholdDeg READ angleThresholdDeg WRITE setAngleThresholdDeg NOTIFY settingsChanged)
    Q_PROPERTY(double marginNm READ marginNm WRITE setMarginNm NOTIFY settingsChanged)
    Q_PROPERTY(bool sensedTurnSource READ sensedTurnSource WRITE setSensedTurnSource NOTIFY settingsChanged)
    Q_PROPERTY(QString simbriefIdentity READ simbriefIdentity WRITE setSimbriefIdentity NOTIFY settingsChanged)
    // The page's flight-plan box (T38 · #57). `planWord` is the box's state —
    // NO IDENTITY · NOT FETCHED · FETCHING · PLAN, or one failure word per
    // thing to fix (the NO MODULE / NO FUEL VARS precedent): NO SUCH USER ·
    // NO PLAN FILED · NO NETWORK · NO FIXES. `planDetail` is the box's second
    // line — the count-and-time of a held plan, or what would change the word.
    Q_PROPERTY(QString planWord READ planWord NOTIFY planChanged)
    Q_PROPERTY(QString planDetail READ planDetail NOTIFY planChanged)
    Q_PROPERTY(QString planRoute READ planRoute NOTIFY planChanged)
    Q_PROPERTY(int planFixCount READ planFixCount NOTIFY planChanged)
    Q_PROPERTY(QString planFetchedAt READ planFetchedAt NOTIFY planChanged)

    // The governor on the main screen (T39 · #58, the T34 variant A binding):
    // the rail's fifth stage carries the core's fixed word and sub, the hero's
    // held state keys on the open window — accent numeral, ghost pip at the
    // cruise rate, "HELD FOR <fix> · returns to N×".
    Q_PROPERTY(QString govWord READ govWord NOTIFY governorChanged)
    Q_PROPERTY(QString govSub READ govSub NOTIFY governorChanged)
    Q_PROPERTY(QString govFix READ govFix NOTIFY governorChanged)
    Q_PROPERTY(bool govWindowOpen READ govWindowOpen NOTIFY governorChanged)
    Q_PROPERTY(double govCruiseRate READ govCruiseRate NOTIFY governorChanged)

public:
    explicit SimService(QObject *parent = nullptr);
    ~SimService() override;

    void start();

    QString status() const { return m_status; }
    QString simInfo() const { return m_simInfo; }
    bool connected() const;
    double simRate() const { return m_simRate; }
    double totalGallons() const { return m_totalGallons; }
    double totalKg() const {
        return m_haveCoreTotal ? m_coreTotalKg : m_totalGallons * m_kgPerGal;
    }
    double lastDt() const { return m_lastDt; }
    double kgPerGallon() const { return m_kgPerGal; }
    QVariantList tanks() const { return m_tanks; }

    QString bridgeWord() const { return m_bridgeWord; }
    bool bridgeUp() const { return m_bridgeUp; }
    QString correctorState() const { return m_correctorState; }
    QString aircraftVariant() const { return m_aircraftVariant; }
    bool sessionActive() const { return m_sessionActive; }
    QString lastEvent() const { return m_lastEvent; }
    QString lastEventTime() const { return m_lastEventTime; }
    bool masterAuto() const { return m_masterAuto; }
    double effectiveRate() const { return m_effectiveRate; }
    double removedRealisedKg() const { return m_removedRealisedKg; }
    double removedCommandedKg() const { return m_removedCommandedKg; }
    double removedMeasuredKg() const { return m_removedMeasuredKg; }
    double burnObservedKg() const { return m_burnObservedKg; }
    double burnEffectiveKg() const { return m_burnEffectiveKg; }
    double elapsedSeconds() const { return m_elapsedSeconds; }

    // Master switch (T10) — the AUTO chip / DISENGAGE control call these.
    Q_INVOKABLE void arm();
    Q_INVOKABLE void disengage();

    // Sim-rate steppers (T9): halve / double via SIM_RATE_INCR/DECR + readback.
    Q_INVOKABLE void rateUp();
    Q_INVOKABLE void rateDown();

    // Settings (T36). Setters clamp to what the core accepts, then persist.
    bool governorEnabled() const { return m_gov.enabled; }
    void setGovernorEnabled(bool on);
    double turnRate() const { return m_gov.turnRate; }
    void setTurnRate(double rate); // 1, 2 or 4
    double angleThresholdDeg() const { return m_gov.angleThresholdDeg; }
    void setAngleThresholdDeg(double deg); // 0–180
    double marginNm() const { return m_gov.marginNm; }
    void setMarginNm(double nm); // 0–10
    bool sensedTurnSource() const { return m_gov.sensedTurnSource; }
    void setSensedTurnSource(bool on);
    QString simbriefIdentity() const { return m_simbriefIdentity; }
    void setSimbriefIdentity(const QString &identity);
    const maxwarp::GovernorSettings &governorSettings() const { return m_gov; }

    QString govWord() const { return m_govWord; }
    QString govSub() const { return m_govSub; }
    QString govFix() const { return m_govFix; }
    bool govWindowOpen() const { return m_govWindowOpen; }
    double govCruiseRate() const { return m_govCruiseRate; }

    QString planWord() const;
    QString planDetail() const;
    QString planRoute() const
    {
        return m_plan.fixes.isEmpty() ? QString() : m_plan.route();
    }
    int planFixCount() const { return m_plan.fixes.size(); }
    QString planFetchedAt() const { return m_planFetchedAt; }
    Q_INVOKABLE void refreshPlan();
    // The held plan, for the wiring ticket (T39) to hand the governor.
    const maxwarp::simbrief::Plan &plan() const { return m_plan; }

signals:
    void statusChanged();
    void simInfoChanged();
    void tickReceived();
    void correctorChanged();
    void bridgeChanged();
    void settingsChanged();
    void planChanged();
    void governorChanged();

private slots:
    void onStatus(const QString &status);
    void onSimInfo(const QString &info);
    void onBridge(const QString &word, bool up);
    void onTick(double simRate, double totalGallons, const QList<double> &tankGallons,
                const QList<double> &tankCaps, double lbsPerGal, double dtWallSeconds);
    void onRateReadback(double simRate);
    void onCorrectorUpdate(const QVariantMap &state);

private slots:
    void onPlanFetched(const maxwarp::simbrief::Result &result);

private:
    void loadSettings();
    void persist(const char *key, const QVariant &value);
    void postEvent(const QString &text);
    void startFetch();
    void pushGovernorSettings();
    void pushGovernorPlan();

    SimConnectWorker *m_worker = nullptr;
    SimBriefClient *m_simbrief = nullptr;
    maxwarp::GovernorSettings m_gov; // core defaults until QSettings says otherwise
    QString m_simbriefIdentity;
    QString m_resolvedUserid; // fetch.userid once SimBrief has named it (T32)
    maxwarp::simbrief::Plan m_plan;
    QString m_planFailWord; // the last fetch's failure word; empty when none
    bool m_planFetching = false;
    bool m_wasSessionActive = false; // pairing-lock edge for the fetch trigger
    QString m_planFetchedAt;
    QString m_status; // empty until the worker reports, so the first status always logs
    QString m_simInfo;
    double m_simRate = 0.0;
    double m_totalGallons = 0.0;
    double m_lastDt = 0.0;
    double m_kgPerGal = 3.039; // Jet-A fallback until the first packet arrives
    QVariantList m_tanks;

    // No module until one answers — the honest starting position, since MaxWarp
    // ships no in-sim component of its own (T18).
    QString m_bridgeWord = QStringLiteral("NO MODULE");
    bool m_bridgeUp = false;

    // Armed-but-waiting until the first corrector tick reports otherwise (T10).
    QString m_correctorState = QStringLiteral("STANDBY");
    QString m_aircraftVariant;
    bool m_sessionActive = false;
    QString m_lastEvent;
    QString m_lastEventTime;
    bool m_masterAuto = true;
    double m_effectiveRate = 0.0;
    double m_removedRealisedKg = 0.0;
    double m_removedCommandedKg = 0.0;
    double m_removedMeasuredKg = 0.0;
    double m_burnObservedKg = 0.0;
    double m_burnEffectiveKg = 0.0;
    double m_elapsedSeconds = 0.0;
    double m_coreTotalKg = 0.0;
    bool m_haveCoreTotal = false;

    // Governor screen state (T39): OFF until the worker's first tick says
    // otherwise — the switch defaults off, so this is also the honest start.
    QString m_govWord = QStringLiteral("OFF");
    QString m_govSub;
    QString m_govFix;
    bool m_govWindowOpen = false;
    double m_govCruiseRate = 1.0;

    // Per-tank display-rate state: raw telemetry rates as the pre-engagement
    // fallback, EMA smoothing over what's shown so the delta rows read steady.
    QVector<double> m_prevGallons;
    QVector<double> m_telemKgS;
    QVector<double> m_obsEma;
    QVector<double> m_cmdEma;
    bool m_havePrev = false;
};
