#include "LVarBridge.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <SimConnect.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace maxwarp {

namespace {

// Module.cpp @ 1.0.1: MOBIFLIGHT_MESSAGE_SIZE = 1024; the well-known channels are
// "MobiFlight.Command" / ".Response"; a registered client gets "<Client>.Command",
// "<Client>.Response" and "<Client>.LVars", the last packing one float per
// subscribed variable at index*4 in registration order.
constexpr DWORD kMessageSize = 1024;
const char *const kWellKnownCommand = "MobiFlight.Command";
const char *const kWellKnownResponse = "MobiFlight.Response";

// Id blocks. Client-data areas and definitions have their own id space; request
// ids share one with the worker's sim-object requests, so they start well clear
// of REQ_TICK/REQ_TITLE/REQ_RATE.
constexpr DWORD kWellKnownArea = 100; // and 101 for the response side
constexpr DWORD kWellKnownReq = 900;
constexpr DWORD kOwnAreaBase = 110;   // + generation*3: command, response, lvars
constexpr DWORD kOwnReqBase = 910;    // + generation*2: response, lvars

// Handshake pacing. Generous: a module that is simply absent should read as
// absent quickly, but a sim mid-load should not be mistaken for one.
constexpr double kPingIntervalS = 3.0;
constexpr double kRegisterRetryS = 5.0;
constexpr double kResolveGraceS = 8.0;
constexpr double kStaleS = 5.0;
// The module publishes the subscription progressively — it resolves a bounded
// number of variables per frame — so the first block back carries only some of
// the names. Declaring the bridge up on it would report tanks as absent purely
// because they had not been published yet, and on the -300 the only genuinely
// absent name is CENTER. Wait for the whole set, or for this to elapse.
constexpr double kPublishSettleS = 2.0;

// Names beyond the tanks. INI_ENG*_FUEL_USED is the core's own cumulative burn
// integrator — T17 measured it against observed drain to 0.03%, so it is the
// max-burn clamp's correct reference (T16). INI_EFB_IS_REFUELING makes the
// conservation gate exact instead of inferred from a net fuel gain.
const char *const kEng1FuelUsed = "INI_ENG1_FUEL_USED";
const char *const kEng2FuelUsed = "INI_ENG2_FUEL_USED";
const char *const kIsRefueling = "INI_EFB_IS_REFUELING";

} // namespace

// Index-aligned to SimConnectWorker's slot order: inner mains, outer aux,
// center, trim. Trim quantity must come from here and never from EXTERNAL1,
// which carries a phantom +/-1.14 kg 1 Hz oscillation (T17).
const char *const LVarBridge::kTankLVars[kBridgeTankCount] = {
    "INI_FUEL_WEIGHT_LEFT_INNER",  "INI_FUEL_WEIGHT_RIGHT_INNER",
    "INI_FUEL_WEIGHT_LEFT_OUTER",  "INI_FUEL_WEIGHT_RIGHT_OUTER",
    "INI_FUEL_WEIGHT_CENTER",      "INI_FUEL_WEIGHT_TRIM",
};

const char *bridgeStateWord(BridgeState state)
{
    switch (state) {
        case BridgeState::Down: return "NO MODULE";
        case BridgeState::Linking: return "LINKING";
        case BridgeState::NoFuelVars: return "NO FUEL VARS";
        case BridgeState::Up: return "UP";
    }
    return "?";
}

void LVarBridge::addEvent(const std::string &text)
{
    m_events.push_back(text);
    if (m_events.size() > 16)
        m_events.erase(m_events.begin());
}

std::vector<std::string> LVarBridge::takeEvents()
{
    std::vector<std::string> out;
    out.swap(m_events);
    return out;
}

void LVarBridge::setState(BridgeState next, const std::string &why)
{
    if (next == m_state)
        return;
    m_state = next;
    m_stateSince = Clock::now();
    addEvent(why);
}

void LVarBridge::attach(void *hSim, const char *clientBaseName)
{
    m_hSim = hSim;
    m_clientBaseName = clientBaseName && *clientBaseName ? clientBaseName : "MaxWarp";
    m_generation = 0;
    m_state = BridgeState::Down;
    m_stateSince = m_attachedAt = Clock::now();
    m_haveBlock = false;
    m_subscribed = false;
    m_pendingReestablish = false;
    m_wellKnownMapped = false;
    m_moduleVersion.clear();
    m_names.clear();
    m_values.clear();
    m_resolved.clear();
    m_ourSends.clear();
    beginHandshake();
}

void LVarBridge::detach()
{
    m_hSim = nullptr;
    m_state = BridgeState::Down;
    m_subscribed = false;
    m_haveBlock = false;
    m_wellKnownMapped = false;
    m_names.clear();
    m_values.clear();
    m_resolved.clear();
    m_ourSends.clear();
}

bool LVarBridge::mapArea(const char *name, unsigned area, unsigned def, unsigned size)
{
    HANDLE h = static_cast<HANDLE>(m_hSim);
    if (FAILED(SimConnect_MapClientDataNameToID(h, name, area)))
        return false;
    return SUCCEEDED(SimConnect_AddToClientDataDefinition(h, def, 0, size, 0, 0));
}

void LVarBridge::send(unsigned area, unsigned def, const std::string &command)
{
    if (!m_hSim)
        return;
    char buf[kMessageSize];
    std::memset(buf, 0, sizeof(buf));
    std::strncpy(buf, command.c_str(), sizeof(buf) - 1);
    HANDLE h = static_cast<HANDLE>(m_hSim);
    SimConnect_SetClientData(h, area, def, SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT, 0,
                             kMessageSize, buf);
    // Remember the packet id so an exception can be attributed to us rather than
    // to the worker's own definitions.
    DWORD sendId = 0;
    if (SUCCEEDED(SimConnect_GetLastSentPacketID(h, &sendId))) {
        m_ourSends.push_back(sendId);
        if (m_ourSends.size() > 64)
            m_ourSends.erase(m_ourSends.begin());
    }
}

void LVarBridge::sendWellKnown(const std::string &command)
{
    send(kWellKnownArea, kWellKnownArea, command);
}

void LVarBridge::sendPrivate(const std::string &command)
{
    const unsigned base = kOwnAreaBase + m_generation * 3;
    send(base, base, command);
}

void LVarBridge::beginHandshake()
{
    if (!m_hSim)
        return;
    HANDLE h = static_cast<HANDLE>(m_hSim);
    if (!mapArea(kWellKnownCommand, kWellKnownArea, kWellKnownArea, kMessageSize) ||
        !mapArea(kWellKnownResponse, kWellKnownArea + 1, kWellKnownArea + 1, kMessageSize)) {
        m_wellKnownMapped = false;
        m_lastPing = Clock::now();
        setState(BridgeState::Down, "bridge down — cannot reach the module's channels");
        return;
    }
    m_wellKnownMapped = true;
    SimConnect_RequestClientData(h, kWellKnownArea + 1, kWellKnownReq, kWellKnownArea + 1,
                                 SIMCONNECT_CLIENT_DATA_PERIOD_ON_SET,
                                 SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_DEFAULT, 0, 0, 0);
    // The module drops the first command after a client attaches; the second ping
    // is the one that answers.
    sendWellKnown("MF.Ping");
    sendWellKnown("MF.Ping");
    m_lastPing = Clock::now();
}

void LVarBridge::attachOwnChannels()
{
    HANDLE h = static_cast<HANDLE>(m_hSim);
    const unsigned base = kOwnAreaBase + m_generation * 3;
    const std::string cmd = m_clientName + ".Command";
    const std::string resp = m_clientName + ".Response";
    const std::string lvars = m_clientName + ".LVars";

    if (!mapArea(cmd.c_str(), base, base, kMessageSize) ||
        !mapArea(resp.c_str(), base + 1, base + 1, kMessageSize)) {
        setState(BridgeState::Down, "bridge down — private channel refused");
        return;
    }
    SimConnect_RequestClientData(h, base + 1, kOwnReqBase + m_generation * 2, base + 1,
                                 SIMCONNECT_CLIENT_DATA_PERIOD_ON_SET,
                                 SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_DEFAULT, 0, 0, 0);
    SimConnect_MapClientDataNameToID(h, lvars.c_str(), base + 2);

    sendPrivate("MF.Version.Get");
    sendPrivate("MF.Config.MAX_VARS_PER_FRAME.Set.100");
    subscribe();
}

void LVarBridge::subscribe()
{
    HANDLE h = static_cast<HANDLE>(m_hSim);
    const unsigned base = kOwnAreaBase + m_generation * 3;

    m_names.clear();
    for (int i = 0; i < kBridgeTankCount; ++i) {
        m_tankSlotIndex[i] = static_cast<int>(m_names.size());
        m_names.push_back(kTankLVars[i]);
    }
    m_eng1Index = static_cast<int>(m_names.size());
    m_names.push_back(kEng1FuelUsed);
    m_eng2Index = static_cast<int>(m_names.size());
    m_names.push_back(kEng2FuelUsed);
    m_refuelIndex = static_cast<int>(m_names.size());
    m_names.push_back(kIsRefueling);

    m_values.assign(m_names.size(), 0.0f);
    m_resolved.assign(m_names.size(), false);

    // Resolve every name unconditionally. MF.LVars.List is never consulted: it
    // walks ids 0..999 and stops, and INI_FUEL_WEIGHT_* are not in it at all, so
    // gating on the listing would conclude the fuel L:vars do not exist. Add
    // resolves through the sim's own calculator instead, which has no such cap;
    // a name that genuinely does not exist simply reads a flat 0.
    sendPrivate("MF.SimVars.Clear");
    for (const auto &n : m_names)
        sendPrivate("MF.SimVars.Add.(L:" + n + ")");

    // One definition spanning every subscribed float, so each packet is a whole
    // snapshot on one clock rather than N interleaved single-variable packets.
    const DWORD bytes = static_cast<DWORD>(m_names.size() * sizeof(float));
    SimConnect_AddToClientDataDefinition(h, base + 2, 0, bytes, 0, 0);
    SimConnect_RequestClientData(h, base + 2, kOwnReqBase + m_generation * 2 + 1, base + 2,
                                 SIMCONNECT_CLIENT_DATA_PERIOD_ON_SET,
                                 SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_CHANGED, 0, 0, 0);

    m_subscribed = true;
    m_haveBlock = false;
    m_stateSince = Clock::now();
}

void LVarBridge::reestablish(const char *why)
{
    if (!m_hSim)
        return;
    // Never rebuild a definition in place — that took the sim down mid-session in
    // T17. A new generation means a new client name and a fresh id block, so the
    // poisoned set is simply abandoned. Cancelling its data requests is safe (it
    // touches no definition) and stops the old stream talking over the new one.
    HANDLE h = static_cast<HANDLE>(m_hSim);
    const unsigned oldBase = kOwnAreaBase + m_generation * 3;
    const unsigned oldReq = kOwnReqBase + m_generation * 2;
    SimConnect_RequestClientData(h, oldBase + 1, oldReq, oldBase + 1,
                                 SIMCONNECT_CLIENT_DATA_PERIOD_NEVER,
                                 SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_DEFAULT, 0, 0, 0);
    SimConnect_RequestClientData(h, oldBase + 2, oldReq + 1, oldBase + 2,
                                 SIMCONNECT_CLIENT_DATA_PERIOD_NEVER,
                                 SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_DEFAULT, 0, 0, 0);
    ++m_generation;
    m_subscribed = false;
    m_haveBlock = false;
    m_names.clear();
    m_values.clear();
    m_resolved.clear();
    setState(BridgeState::Linking, std::string("bridge re-establishing — ") + why);
    m_clientName = m_clientBaseName + std::to_string(m_generation);
    sendWellKnown(std::string("MF.Clients.Add.") + m_clientName);
    m_lastPing = Clock::now();
}

void LVarBridge::noteAircraftReload()
{
    if (!m_hSim || m_state == BridgeState::Down)
        return;
    m_pendingReestablish = true;
}

void LVarBridge::noteException(unsigned code, unsigned sendId)
{
    if (!m_hSim || !m_subscribed)
        return;
    bool ours = false;
    for (unsigned id : m_ourSends)
        if (id == sendId)
            ours = true;
    if (!ours)
        return;
    // UNRECOGNIZED_ID on one of our sends means the definitions were invalidated
    // under us — the flight-load failure mode. Everything after this would be
    // written into a void with no further error, so re-establish now.
    if (code == SIMCONNECT_EXCEPTION_UNRECOGNIZED_ID)
        m_pendingReestablish = true;
}

void LVarBridge::poll(bool simActive)
{
    if (!m_hSim)
        return;

    if (m_pendingReestablish) {
        m_pendingReestablish = false;
        reestablish("flight or aircraft reloaded");
        return;
    }

    switch (m_state) {
        case BridgeState::Down:
            // Keep knocking: the module may be loading, or the user may only now
            // have started a flight that has it.
            if (secondsSince(m_lastPing) <= kPingIntervalS)
                break;
            if (!m_wellKnownMapped) {
                beginHandshake();
            } else {
                sendWellKnown("MF.Ping");
                m_lastPing = Clock::now();
            }
            break;

        case BridgeState::Linking:
            if (!m_subscribed) {
                if (secondsSince(m_lastPing) > kRegisterRetryS) {
                    sendWellKnown(std::string("MF.Clients.Add.") + m_clientName);
                    m_lastPing = Clock::now();
                }
            } else if (simActive && secondsSince(m_stateSince) > kResolveGraceS) {
                // Subscribed, but no fuel quantity has ever read non-zero. Note we
                // cannot distinguish an unresolved name from a genuinely empty
                // aircraft — T17 established that a name which does not exist also
                // reads a flat 0 — but the two are the same answer here: there is
                // no usable core state, so there is nothing to correct.
                setState(BridgeState::NoFuelVars,
                         "bridge down — fuel L:vars did not resolve");
            }
            break;

        case BridgeState::NoFuelVars:
            // Self-healing: a value arriving later promotes straight to Up (see
            // onValues). Nothing to do but wait.
            break;

        case BridgeState::Up:
            // The subscription streams on change only, so silence is evidence of a
            // stale handle exactly when the values ought to be moving. This is the
            // detection T18 demanded: silent write loss must never be silent.
            if (simActive && m_haveBlock && secondsSince(m_lastBlock) > kStaleS)
                reestablish("core state stopped arriving");
            break;
    }
}

bool LVarBridge::handleClientData(const void *recv)
{
    if (!m_hSim)
        return false;
    auto *d = static_cast<const SIMCONNECT_RECV_CLIENT_DATA *>(recv);
    const DWORD respReq = kOwnReqBase + m_generation * 2;
    const DWORD lvarReq = respReq + 1;

    if (d->dwRequestID == kWellKnownReq || d->dwRequestID == respReq) {
        char text[kMessageSize + 1];
        std::memcpy(text, &d->dwData, kMessageSize);
        text[kMessageSize] = '\0';
        onResponse(text);
        return true;
    }
    if (d->dwRequestID == lvarReq) {
        onValues(reinterpret_cast<const float *>(&d->dwData),
                 static_cast<int>(m_values.size()));
        return true;
    }
    return false;
}

void LVarBridge::onResponse(const char *text)
{
    const std::string s(text);
    if (s.empty())
        return;

    if (s == "MF.Pong") {
        if (m_state == BridgeState::Down) {
            m_clientName = m_clientBaseName;
            setState(BridgeState::Linking, "bridge linking — module responded");
            sendWellKnown(std::string("MF.Clients.Add.") + m_clientName);
            m_lastPing = Clock::now();
        }
        return;
    }
    if (s.rfind("MF.Version.", 0) == 0) {
        m_moduleVersion = s.substr(std::strlen("MF.Version."));
        return;
    }
    if (!m_clientName.empty() && s == "MF.Clients.Add." + m_clientName + ".Finished") {
        if (!m_subscribed)
            attachOwnChannels();
        return;
    }
}

void LVarBridge::onValues(const float *values, int count)
{
    m_lastBlock = Clock::now();
    m_haveBlock = true;
    if (m_values.empty())
        return;
    for (int i = 0; i < count && i < static_cast<int>(m_values.size()); ++i) {
        m_values[i] = values[i];
        if (values[i] != 0.0f && std::isfinite(values[i]))
            m_resolved[i] = true;
    }

    int tanksResolved = 0;
    for (int slot = 0; slot < kBridgeTankCount; ++slot)
        if (m_resolved[m_tankSlotIndex[slot]])
            ++tanksResolved;

    // Up as soon as the whole tank set has reported, or once the publish settle
    // has passed and at least one has — the -300 has no CENTER, so waiting for
    // all six would never complete there. Resolution is sticky, so this only ever
    // climbs, including out of NoFuelVars: a bridge that came up before the fuel
    // did heals itself without a restart.
    const bool settled = tanksResolved == kBridgeTankCount ||
                         secondsSince(m_stateSince) > kPublishSettleS;
    if (tanksResolved > 0 && settled && m_state != BridgeState::Up)
        setState(BridgeState::Up, "bridge up — core state readable");
}

double LVarBridge::tankKg(int slot) const
{
    if (slot < 0 || slot >= kBridgeTankCount || m_values.empty())
        return 0.0;
    return m_values[m_tankSlotIndex[slot]];
}

bool LVarBridge::tankResolved(int slot) const
{
    if (slot < 0 || slot >= kBridgeTankCount || m_resolved.empty())
        return false;
    return m_resolved[m_tankSlotIndex[slot]];
}

BridgeSample LVarBridge::sample() const
{
    BridgeSample s;
    s.state = m_state;
    s.up = isReady();
    for (int slot = 0; slot < kBridgeTankCount; ++slot) {
        s.tankKg[slot] = tankKg(slot);
        s.resolved[slot] = tankResolved(slot);
    }
    if (m_eng1Index >= 0 && m_eng2Index >= 0 && !m_values.empty()) {
        s.fuelUsedKg = m_values[m_eng1Index] + m_values[m_eng2Index];
        s.haveFuelUsed = m_resolved[m_eng1Index] || m_resolved[m_eng2Index];
    }
    if (m_refuelIndex >= 0 && !m_values.empty())
        s.refueling = m_values[m_refuelIndex] != 0.0f;
    return s;
}

bool LVarBridge::writeTankDeltaKg(int slot, double deltaKg)
{
    if (!isReady() || slot < 0 || slot >= kBridgeTankCount)
        return false;
    if (!tankResolved(slot))
        return false;
    if (!std::isfinite(deltaKg) || deltaKg == 0.0)
        return false;

    const char *name = kTankLVars[slot];
    // The magnitude carries the sign via the operator, so no negative literal ever
    // enters the RPN. The leading sequence tag guarantees the payload differs from
    // the last one: byte-identical consecutive commands are silently dropped, and
    // a corrector holding a steady target would otherwise write nothing at all
    // while looking, from the outside, like a sim fault (T17).
    const char op = deltaKg < 0.0 ? '-' : '+';
    char code[320];
    std::snprintf(code, sizeof(code),
                  "MF.SimVars.Set.%u (>L:MAXWARP_SEQ) (L:%s) %.4f %c (>L:%s)",
                  ++m_seq, name, std::fabs(deltaKg), op, name);
    sendPrivate(code);
    return true;
}

} // namespace maxwarp
