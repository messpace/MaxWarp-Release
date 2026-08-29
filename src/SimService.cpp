#include "SimService.h"

#include "SimBriefClient.h"
#include "SimConnectWorker.h"

#include <QDebug>
#include <QSettings>
#include <QTime>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace {

// QSettings keys (T36). `theme/mode` is ThemeController's; these sit beside it.
constexpr const char *kKeyGovEnabled = "governor/enabled";
constexpr const char *kKeyTurnRate = "governor/turnRate";
constexpr const char *kKeyAngleThreshold = "governor/angleThresholdDeg";
constexpr const char *kKeyMargin = "governor/marginNm";
constexpr const char *kKeySensed = "governor/sensedTurnSource";
constexpr const char *kKeyIdentity = "simbrief/identity";
constexpr const char *kKeyUserid = "simbrief/userid"; // resolved fetch.userid (T32)

constexpr double kKgPerLb = 0.45359237;
constexpr double kFallbackLbsPerGal = 6.699; // Jet-A, mirrors CorrectorConfig
constexpr double kEmaAlpha = 0.35;           // delta-row smoothing, ~3-tick settle

double ema(double prev, double sample)
{
    return std::isnan(prev) ? sample : prev + kEmaAlpha * (sample - prev);
}

} // namespace

SimService::SimService(QObject *parent)
    : QObject(parent)
    , m_worker(new SimConnectWorker(this))
    , m_simbrief(new SimBriefClient(this))
{
    connect(m_simbrief, &SimBriefClient::finished, this, &SimService::onPlanFetched);
    connect(m_worker, &SimConnectWorker::statusChanged, this, &SimService::onStatus);
    connect(m_worker, &SimConnectWorker::simInfoChanged, this, &SimService::onSimInfo);
    connect(m_worker, &SimConnectWorker::bridgeChanged, this, &SimService::onBridge);
    connect(m_worker, &SimConnectWorker::tick, this, &SimService::onTick);
    connect(m_worker, &SimConnectWorker::rateReadback, this, &SimService::onRateReadback);
    connect(m_worker, &SimConnectWorker::correctorUpdate, this,
            &SimService::onCorrectorUpdate);

    const int n = SimConnectWorker::kTankCount;
    for (int i = 0; i < n; ++i) {
        QVariantMap tank;
        tank[QStringLiteral("label")] = QString::fromLatin1(SimConnectWorker::kTankLabels[i]);
        tank[QStringLiteral("simvar")] = QString::fromLatin1(SimConnectWorker::kTankSimvars[i]);
        tank[QStringLiteral("gallons")] = 0.0;
        tank[QStringLiteral("capacity")] = 0.0;
        tank[QStringLiteral("kg")] = 0.0;
        tank[QStringLiteral("pct")] = 0.0;
        tank[QStringLiteral("obsKgS")] = 0.0;
        tank[QStringLiteral("cmdKgS")] = 0.0;
        m_tanks.append(tank);
    }
    m_prevGallons.fill(0.0, n);
    m_telemKgS.fill(0.0, n);
    const double nan = std::nan("");
    m_obsEma.fill(nan, n);
    m_cmdEma.fill(nan, n);

    loadSettings();
}

// ---------- settings (T36 · #55) ----------

void SimService::loadSettings()
{
    QSettings st;
    const maxwarp::GovernorSettings defaults;
    m_gov.enabled = st.value(QLatin1String(kKeyGovEnabled), defaults.enabled).toBool();
    // Re-run each value through its setter's clamp so a hand-edited or stale
    // registry value can never put the core outside what it accepts.
    setTurnRate(st.value(QLatin1String(kKeyTurnRate), defaults.turnRate).toDouble());
    setAngleThresholdDeg(
        st.value(QLatin1String(kKeyAngleThreshold), defaults.angleThresholdDeg).toDouble());
    setMarginNm(st.value(QLatin1String(kKeyMargin), defaults.marginNm).toDouble());
    m_gov.sensedTurnSource =
        st.value(QLatin1String(kKeySensed), defaults.sensedTurnSource).toBool();
    m_simbriefIdentity = st.value(QLatin1String(kKeyIdentity)).toString().trimmed();
    m_resolvedUserid = st.value(QLatin1String(kKeyUserid)).toString().trimmed();
}

void SimService::persist(const char *key, const QVariant &value)
{
    QSettings().setValue(QLatin1String(key), value);
    emit settingsChanged();
}

// The worker's governor hears every knob the moment it settles (T39): the
// setters push after persisting, and start() pushes the loaded state once.
void SimService::pushGovernorSettings()
{
    m_worker->updateGovernorSettings(m_gov);
}

// Convert the held SimBrief plan into the core's shape and hand it over. An
// empty held plan clears the governor's — NO PLAN on the rail, honestly.
void SimService::pushGovernorPlan()
{
    maxwarp::Plan plan;
    plan.haveOrigin = m_plan.haveOrigin;
    plan.origin = {m_plan.originIcao.toStdString(), m_plan.originLatDeg,
                   m_plan.originLonDeg};
    plan.fixes.reserve(m_plan.fixes.size());
    for (const auto &f : m_plan.fixes)
        plan.fixes.push_back({f.ident.toStdString(), f.latDeg, f.lonDeg});
    m_worker->updateGovernorPlan(plan);
}

void SimService::setGovernorEnabled(bool on)
{
    if (m_gov.enabled == on)
        return;
    m_gov.enabled = on;
    persist(kKeyGovEnabled, on);
    pushGovernorSettings();
}

void SimService::setTurnRate(double rate)
{
    // The stepper moves in powers of two and the page offers 1× / 2× / 4×;
    // anything else snaps to the nearest of those.
    const double snapped = rate >= 3.0 ? 4.0 : rate >= 1.5 ? 2.0 : 1.0;
    if (m_gov.turnRate == snapped)
        return;
    m_gov.turnRate = snapped;
    persist(kKeyTurnRate, snapped);
    pushGovernorSettings();
}

void SimService::setAngleThresholdDeg(double deg)
{
    const double clamped = std::clamp(std::round(deg), 0.0, 180.0);
    if (m_gov.angleThresholdDeg == clamped)
        return;
    m_gov.angleThresholdDeg = clamped;
    persist(kKeyAngleThreshold, clamped);
    pushGovernorSettings();
}

void SimService::setMarginNm(double nm)
{
    const double clamped = std::clamp(std::round(nm * 2.0) / 2.0, 0.0, 10.0);
    if (m_gov.marginNm == clamped)
        return;
    m_gov.marginNm = clamped;
    persist(kKeyMargin, clamped);
    pushGovernorSettings();
}

void SimService::setSensedTurnSource(bool on)
{
    if (m_gov.sensedTurnSource == on)
        return;
    m_gov.sensedTurnSource = on;
    persist(kKeySensed, on);
    pushGovernorSettings();
}

void SimService::setSimbriefIdentity(const QString &identity)
{
    const QString trimmed = identity.trimmed();
    if (m_simbriefIdentity == trimmed)
        return;
    m_simbriefIdentity = trimmed;
    persist(kKeyIdentity, trimmed);
    // A new identity voids everything the old one resolved: the canonical
    // userid, the held plan, the last failure. The box returns to NOT FETCHED.
    m_resolvedUserid.clear();
    QSettings().remove(QLatin1String(kKeyUserid));
    m_plan = maxwarp::simbrief::Plan();
    m_planFetchedAt.clear();
    m_planFailWord.clear();
    pushGovernorPlan();
    emit planChanged();
}

// ---------- the SimBrief client (T38 · #57) ----------

QString SimService::planWord() const
{
    if (m_simbriefIdentity.isEmpty())
        return QStringLiteral("NO IDENTITY");
    if (m_planFetching)
        return QStringLiteral("FETCHING");
    if (!m_planFailWord.isEmpty())
        return m_planFailWord;
    if (!m_plan.fixes.isEmpty())
        return QStringLiteral("PLAN");
    return QStringLiteral("NOT FETCHED");
}

QString SimService::planDetail() const
{
    // A held plan's count-and-time stays up even through a failed refresh —
    // losing the network never loses the plan, only says so.
    if (!m_plan.fixes.isEmpty()) {
        const QString held = QStringLiteral("%1 fixes · fetched %2")
                                 .arg(m_plan.fixes.size())
                                 .arg(m_planFetchedAt);
        if (m_planFetching || m_planFailWord.isEmpty())
            return held;
        return QStringLiteral("%1 · refresh failed — %2").arg(held, m_planFailWord);
    }
    if (m_simbriefIdentity.isEmpty())
        return QStringLiteral("set an identity above");
    if (m_planFetching)
        return QStringLiteral("asking simbrief.com…");
    if (m_planFailWord == QLatin1String("NO SUCH USER"))
        return QStringLiteral("SimBrief knows no such user — check the identity");
    if (m_planFailWord == QLatin1String("NO PLAN FILED"))
        return QStringLiteral("generate a flight on simbrief.com, then refresh");
    if (m_planFailWord == QLatin1String("NO NETWORK"))
        return QStringLiteral("simbrief.com unreachable — check the connection");
    if (m_planFailWord == QLatin1String("NO FIXES"))
        return QStringLiteral("the OFP held no usable fixes — regenerate it");
    return QStringLiteral("nothing fetched yet");
}

void SimService::postEvent(const QString &text)
{
    m_lastEvent = text;
    m_lastEventTime = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    qInfo().noquote() << text;
    emit correctorChanged();
}

void SimService::startFetch()
{
    if (m_simbrief->busy())
        return; // the answer in flight is the answer
    m_planFetching = true;
    m_simbrief->fetch(m_simbriefIdentity, m_resolvedUserid);
    emit planChanged();
}

void SimService::refreshPlan()
{
    if (m_simbriefIdentity.isEmpty()) {
        postEvent(QStringLiteral("SimBrief refresh — no identity set"));
        return;
    }
    startFetch();
}

void SimService::onPlanFetched(const maxwarp::simbrief::Result &result)
{
    using maxwarp::simbrief::FetchStatus;
    m_planFetching = false;
    m_planFailWord.clear();

    switch (result.status) {
    case FetchStatus::Success: {
        // Canonicalise on the resolved Pilot ID: immune to alias renames, and
        // never re-ambiguous between the two identity namespaces (T32).
        if (!result.resolvedUserid.isEmpty() && result.resolvedUserid != m_resolvedUserid) {
            m_resolvedUserid = result.resolvedUserid;
            QSettings().setValue(QLatin1String(kKeyUserid), m_resolvedUserid);
        }
        const QString pair = result.plan.originIcao + QStringLiteral("→")
            + result.plan.destinationIcao;
        const bool unchanged =
            !m_plan.requestId.isEmpty() && result.plan.requestId == m_plan.requestId;
        m_plan = result.plan;
        m_planFetchedAt = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
        pushGovernorPlan();
        postEvent(unchanged
                      ? QStringLiteral("Plan unchanged — %1, %2 fixes")
                            .arg(pair)
                            .arg(m_plan.fixes.size())
                      : QStringLiteral("Plan fetched — %1, %2 fixes")
                            .arg(pair)
                            .arg(m_plan.fixes.size()));
        break;
    }
    case FetchStatus::UnknownUser:
        m_planFailWord = QStringLiteral("NO SUCH USER");
        // Whatever userid we thought was canonical did not resolve; let the
        // next fetch go back to what the human typed.
        m_resolvedUserid.clear();
        QSettings().remove(QLatin1String(kKeyUserid));
        postEvent(QStringLiteral("SimBrief — no user ‘%1’").arg(m_simbriefIdentity));
        break;
    case FetchStatus::NoPlanFiled:
        // The error body still names the account (T32) — keep the canonical id.
        if (!result.resolvedUserid.isEmpty() && result.resolvedUserid != m_resolvedUserid) {
            m_resolvedUserid = result.resolvedUserid;
            QSettings().setValue(QLatin1String(kKeyUserid), m_resolvedUserid);
        }
        m_planFailWord = QStringLiteral("NO PLAN FILED");
        postEvent(QStringLiteral("SimBrief — no plan filed for ‘%1’")
                      .arg(m_simbriefIdentity));
        break;
    case FetchStatus::NetworkDown:
        m_planFailWord = QStringLiteral("NO NETWORK");
        postEvent(QStringLiteral("SimBrief — network down or timed out (%1)")
                      .arg(result.detail));
        break;
    case FetchStatus::NoUsableFixes:
        m_planFailWord = QStringLiteral("NO FIXES");
        postEvent(QStringLiteral("SimBrief — %1").arg(result.detail));
        break;
    }
    emit planChanged();
}

SimService::~SimService()
{
    m_worker->stop();
}

void SimService::start()
{
    pushGovernorSettings(); // the persisted knobs, before the first tick
    m_worker->start();
}

void SimService::arm()
{
    m_worker->requestArm();
}

void SimService::disengage()
{
    m_worker->requestDisengage();
}

void SimService::rateUp()
{
    m_worker->requestRateStep(true);
}

void SimService::rateDown()
{
    m_worker->requestRateStep(false);
}

void SimService::onRateReadback(double simRate)
{
    m_simRate = simRate;
    emit tickReceived();
}

void SimService::onCorrectorUpdate(const QVariantMap &state)
{
    m_correctorState = state.value(QStringLiteral("state")).toString();
    m_masterAuto = state.value(QStringLiteral("masterAuto")).toBool();
    m_sessionActive = state.value(QStringLiteral("sessionActive")).toBool();
    m_effectiveRate = state.value(QStringLiteral("rEff")).toDouble();
    m_removedCommandedKg = state.value(QStringLiteral("removedCommandedKg")).toDouble();
    m_removedRealisedKg = state.value(QStringLiteral("removedRealisedKg")).toDouble();
    m_removedMeasuredKg = state.value(QStringLiteral("removedMeasuredKg")).toDouble();
    m_burnObservedKg = state.value(QStringLiteral("burnObservedKg")).toDouble();
    m_burnEffectiveKg = state.value(QStringLiteral("burnEffectiveKg")).toDouble();
    m_elapsedSeconds = state.value(QStringLiteral("elapsedSec")).toDouble();

    // The governor stage and held state (T39) — emitted only on movement, so
    // the rail and the hero repaint on transitions, not on every tick.
    const QString govWord = state.value(QStringLiteral("govWord")).toString();
    const QString govSub = state.value(QStringLiteral("govSub")).toString();
    const QString govFix = state.value(QStringLiteral("govFix")).toString();
    const bool govOpen = state.value(QStringLiteral("govWindowOpen")).toBool();
    const double govCruise = state.value(QStringLiteral("govCruise")).toDouble();
    if (govWord != m_govWord || govSub != m_govSub || govFix != m_govFix
        || govOpen != m_govWindowOpen || govCruise != m_govCruiseRate) {
        m_govWord = govWord;
        m_govSub = govSub;
        m_govFix = govFix;
        m_govWindowOpen = govOpen;
        m_govCruiseRate = govCruise;
        emit governorChanged();
    }

    const QString variant = state.value(QStringLiteral("variant")).toString();
    if (!variant.isEmpty())
        m_aircraftVariant = variant;
    // Fetch at pairing lock (T38): the session going active is the one moment
    // MaxWarp asks SimBrief unprompted — that plus the Refresh button is the
    // whole fetch surface (T32's user-initiated-only rule).
    if (m_sessionActive && !m_wasSessionActive && !m_simbriefIdentity.isEmpty())
        startFetch();
    m_wasSessionActive = m_sessionActive;
    // Only the ticks that carry a fresh event advance the last-event line (T10).
    if (state.contains(QStringLiteral("lastEvent"))) {
        m_lastEvent = state.value(QStringLiteral("lastEvent")).toString();
        m_lastEventTime = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    }

    // Per-tank quantities: core state whenever the bridge resolved the name (it
    // is what the corrector reads and writes, and it is free of EXTERNAL1's
    // phantom trim oscillation), otherwise the converted simvar mirror set in
    // onTick. Arrives per tick, so onCorrectorUpdate overwriting onTick's value
    // is the intended order.
    const QVariantList coreKg = state.value(QStringLiteral("coreKg")).toList();
    m_haveCoreTotal = state.contains(QStringLiteral("coreTotalKg"));
    if (m_haveCoreTotal)
        m_coreTotalKg = state.value(QStringLiteral("coreTotalKg")).toDouble();

    // Delta rows (T9): while the engaged loop measures, its natural delta is the
    // obs column and delta + write is the cmd column; otherwise fall back to raw
    // telemetry rates (no writes are landing, so telemetry *is* the natural burn).
    // Both are already kg since the T21 port — nothing to convert.
    const QVariantList obsDelta = state.value(QStringLiteral("obsDeltaKg")).toList();
    const QVariantList cmdExtra = state.value(QStringLiteral("cmdExtraKg")).toList();
    const bool engagedRates = obsDelta.size() == m_tanks.size() && m_lastDt > 0.05;
    for (int i = 0; i < m_tanks.size(); ++i) {
        double obsK = m_telemKgS[i];
        double cmdK = obsK;
        if (engagedRates) {
            obsK = obsDelta[i].toDouble() / m_lastDt;
            cmdK = obsK + cmdExtra[i].toDouble() / m_lastDt;
        }
        m_obsEma[i] = ema(m_obsEma[i], obsK);
        m_cmdEma[i] = ema(m_cmdEma[i], cmdK);
        QVariantMap tank = m_tanks[i].toMap();
        tank[QStringLiteral("obsKgS")] = m_obsEma[i];
        tank[QStringLiteral("cmdKgS")] = m_cmdEma[i];
        if (i < coreKg.size() && coreKg[i].isValid()) {
            const double kg = coreKg[i].toDouble();
            const double cap = tank[QStringLiteral("capacity")].toDouble() * m_kgPerGal;
            tank[QStringLiteral("kg")] = kg;
            tank[QStringLiteral("pct")] = cap > 0.0 ? kg / cap : 0.0;
        }
        m_tanks[i] = tank;
    }

    emit tickReceived();
    emit correctorChanged();
}

bool SimService::connected() const
{
    return m_status == QLatin1String("connected") || m_status == QLatin1String("stalled");
}

void SimService::onStatus(const QString &status)
{
    if (m_status == status)
        return;
    m_status = status;
    qInfo().noquote() << "status:" << status;
    emit statusChanged();
}

void SimService::onBridge(const QString &word, bool up)
{
    if (m_bridgeWord == word && m_bridgeUp == up)
        return;
    m_bridgeWord = word;
    m_bridgeUp = up;
    qInfo().noquote() << "bridge:" << word;
    emit bridgeChanged();
}

void SimService::onSimInfo(const QString &info)
{
    m_simInfo = info;
    qInfo().noquote() << "sim:" << info;
    emit simInfoChanged();
}

void SimService::onTick(double simRate, double totalGallons,
                        const QList<double> &tankGallons, const QList<double> &tankCaps,
                        double lbsPerGal, double dtWallSeconds)
{
    m_simRate = simRate;
    m_totalGallons = totalGallons;
    m_lastDt = dtWallSeconds;
    m_kgPerGal = (lbsPerGal > 0.1 ? lbsPerGal : kFallbackLbsPerGal) * kKgPerLb;

    QString logLine;
    for (int i = 0; i < m_tanks.size() && i < tankGallons.size(); ++i) {
        const double gallons = tankGallons[i];
        const double cap = i < tankCaps.size() ? tankCaps[i] : 0.0;
        // Telemetry fallback rate; a dt burst or first tick keeps the last value.
        if (m_havePrev && dtWallSeconds > 0.05) {
            const double raw = (gallons - m_prevGallons[i]) / dtWallSeconds * m_kgPerGal;
            if (std::fabs(raw) < 100.0) // teleport/refuel spikes stay off the display
                m_telemKgS[i] = raw;
        }
        m_prevGallons[i] = gallons;

        QVariantMap tank = m_tanks[i].toMap();
        tank[QStringLiteral("gallons")] = gallons;
        tank[QStringLiteral("capacity")] = cap;
        tank[QStringLiteral("kg")] = gallons * m_kgPerGal;
        tank[QStringLiteral("pct")] = cap > 0.0 ? gallons / cap : 0.0;
        m_tanks[i] = tank;
        logLine += QStringLiteral(" %1=%2")
                       .arg(tank[QStringLiteral("label")].toString(),
                            QString::number(gallons, 'f', 1));
    }
    m_havePrev = true;
    qInfo().noquote() << QStringLiteral("tick rate=%1 dt=%2s total=%3gal |%4")
                             .arg(QString::number(simRate, 'f', 2),
                                  QString::number(dtWallSeconds, 'f', 2),
                                  QString::number(totalGallons, 'f', 1), logLine);
    emit tickReceived();
}
