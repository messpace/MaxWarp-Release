#pragma once

// MobiFlight L:var bridge (T20 · issue #23) — the in-sim path by which MaxWarp
// reaches the ini A330's core fuel state at all.
//
// T18 (#20) chose option C: the in-sim module is the whole fuel data path, that
// module is MobiFlight's, and MaxWarp ships no in-sim component of its own. An
// external SimConnect client cannot read *or* write an L:var unaided, so with no
// module there is no correcting — bridge health is an engage precondition, not a
// feature (see CONTEXT.md, "Bridge").
//
// Everything here is transport. The class is Qt-free and knows nothing about the
// corrector; it speaks MobiFlight's client-data protocol on a SimConnect handle
// the caller owns, latches the subscribed values, and offers the T17 write
// primitive. Wiring the corrector's writes onto it is T21's job.
//
// Protocol facts below are measured in T17 (prototypes/t17-lvar-probe), not read
// from documentation, and three of them are load-bearing:
//
//   * MF.LVars.List walks ids 0..999 and stops, and INI_FUEL_WEIGHT_* are absent
//     from it entirely. Names are therefore resolved unconditionally, never
//     gated on the listing — a probe that trusted it would conclude the fuel
//     L:vars do not exist.
//   * Byte-identical consecutive payloads are silently dropped by the command
//     channel. Every write carries a sequence tag; a corrector holding a steady
//     target would otherwise do nothing while looking like a sim fault.
//   * Rebuilding a client-data definition in place took the sim down mid-session.
//     The subscription set is changed only by re-establishing under a fresh
//     client name with fresh definition ids.

#include <chrono>
#include <string>
#include <vector>

namespace maxwarp {

// Bridge health, in the order the handshake walks it. Only `Up` permits
// correcting; the two failure states are kept apart deliberately, because
// "nothing answered" and "answered, but the fuel quantities were not there" are
// different problems with different fixes (T18).
enum class BridgeState {
    Down,        // no module has answered
    Linking,     // pinged / registering / attaching our private channel
    NoFuelVars,  // module is alive, but INI_FUEL_WEIGHT_* never resolved
    Up,          // core state readable now
};

// Tank slots, index-aligned to SimConnectWorker's fixed slot order so a slot
// index means the same thing on the simvar and L:var sides.
constexpr int kBridgeTankCount = 6;

// Fixed rail vocabulary (CONTEXT.md): NO MODULE / LINKING / NO FUEL VARS / UP.
const char *bridgeStateWord(BridgeState state);

// One tick's latched view of core state, for the session log.
struct BridgeSample {
    BridgeState state = BridgeState::Down;
    bool up = false;
    bool resolved[kBridgeTankCount] = {};   // this tank's L:var read non-zero at least once
    double tankKg[kBridgeTankCount] = {};
    double fuelUsedKg = 0.0;  // INI_ENG1_FUEL_USED + INI_ENG2_FUEL_USED (cumulative)
    bool haveFuelUsed = false;
    bool refueling = false;   // INI_EFB_IS_REFUELING
};

class LVarBridge {
public:
    // L:var names for each tank slot. CENTER is populated on the -200 and absent
    // on the -300; that one failing to resolve is normal, not a bridge fault.
    static const char *const kTankLVars[kBridgeTankCount];

    LVarBridge() = default;

    // Bind to a freshly opened SimConnect handle and start the handshake. The
    // caller keeps ownership of the handle.
    //
    // `clientBaseName` is the MobiFlight client to register as, and it must be
    // unique per process: the module keys its private channels by name, so two
    // processes claiming the same one end up sharing a command channel and
    // writing over each other. The app keeps the default; anything else that
    // links this class — the bench check — passes its own.
    void attach(void *hSim, const char *clientBaseName = "MaxWarp");
    void detach();

    // Drive handshake timeouts, retries and staleness detection. Called from the
    // worker loop, at whatever cadence it happens to run. `simActive` means
    // telemetry is currently flowing and the sim is not paused — the subscription
    // only streams on change, so silence is evidence of a poisoned handle just
    // when the values ought to be moving, and means nothing when they are not.
    void poll(bool simActive);

    // Route a SIMCONNECT_RECV_ID_CLIENT_DATA packet. Returns true if it was ours.
    bool handleClientData(const void *recv);

    // A SimConnect exception the caller saw. Only acted on when it names one of
    // our own sends — an invalidated definition is how a poisoned subscription
    // announces itself, and after that writes vanish silently (T17).
    void noteException(unsigned code, unsigned sendId);

    // The aircraft or flight reloaded. Connecting across a load poisons the
    // subscription: the definitions are invalidated when the load finishes and
    // subsequent writes hit a stale handle with *no error at all* (T17). The
    // only safe response is to re-establish.
    void noteAircraftReload();

    BridgeState state() const { return m_state; }
    bool isReady() const { return m_state == BridgeState::Up; }

    const char *stateWord() const { return bridgeStateWord(m_state); }

    // Latched core state. Values are whatever the last block carried; `resolved`
    // says whether the name ever produced a reading.
    BridgeSample sample() const;
    double tankKg(int slot) const;
    bool tankResolved(int slot) const;

    // The T17 write primitive — an atomic in-sim read-modify-write, so there is
    // no client-side race against the core's own 1 Hz integration step:
    //
    //   <seq> (>L:MAXWARP_SEQ) (L:<tank>) <|delta|> -|+ (>L:<tank>)
    //
    // `deltaKg` is signed and applied to core state: negative removes fuel. The
    // magnitude is emitted with the matching operator so no negative literal ever
    // enters the RPN. Returns false if the bridge is not up or the slot's name
    // never resolved. Fire-and-forget: the module acknowledges nothing, so the
    // proof a write landed is the next block's value.
    bool writeTankDeltaKg(int slot, double deltaKg);

    // Human-readable transitions, drained by the caller each tick and shown on
    // the last-event line (T10). Never accumulates unboundedly.
    std::vector<std::string> takeEvents();

private:
    using Clock = std::chrono::steady_clock;

    void setState(BridgeState next, const std::string &why);
    void addEvent(const std::string &text);

    bool mapArea(const char *name, unsigned area, unsigned def, unsigned size);
    void send(unsigned area, unsigned def, const std::string &command);
    void sendWellKnown(const std::string &command); // registration only
    void sendPrivate(const std::string &command);   // everything else

    void beginHandshake();      // map the well-known channels and ping
    void attachOwnChannels();   // our private Command/Response/LVars set
    void subscribe();           // resolve every name unconditionally
    void reestablish(const char *why);
    void onResponse(const char *text);
    void onValues(const float *values, int count);

    double secondsSince(Clock::time_point t) const {
        return std::chrono::duration<double>(Clock::now() - t).count();
    }

    void *m_hSim = nullptr;
    BridgeState m_state = BridgeState::Down;

    // Each re-establish takes a fresh client name and a fresh id block, because
    // rebuilding a definition in place crashes the sim (T17).
    int m_generation = 0;
    std::string m_clientBaseName;
    std::string m_clientName;

    Clock::time_point m_attachedAt{};
    Clock::time_point m_stateSince{};
    Clock::time_point m_lastPing{};
    Clock::time_point m_lastBlock{};
    bool m_haveBlock = false;
    bool m_subscribed = false;
    bool m_pendingReestablish = false;
    bool m_wellKnownMapped = false;

    // Subscription set, in the order the module packs it into the LVars area.
    std::vector<std::string> m_names;
    std::vector<float> m_values;
    std::vector<bool> m_resolved;
    int m_tankSlotIndex[kBridgeTankCount] = {}; // slot -> index into m_names
    int m_eng1Index = -1, m_eng2Index = -1, m_refuelIndex = -1;

    unsigned m_seq = 0;
    std::vector<unsigned> m_ourSends; // recent send ids, for exception attribution
    std::vector<std::string> m_events;
    std::string m_moduleVersion;
};

} // namespace maxwarp
