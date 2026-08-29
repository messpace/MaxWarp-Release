#pragma once

// Per-pairing CSV session log (T10 · issue #11). One file per A330 pairing,
// timestamped name + variant, in %LOCALAPPDATA%/MaxWarp/logs; the last ~20 are
// kept, older ones pruned at launch. Opened shared-read so a harness can tail it
// live during a bench check (the T2 pattern). This is the durable record; the
// UI's lingering stats only summarize.
//
// T24 (#34): the file's life is the *pairing's* life, not detection's — the
// caller rotates it when the paired airframe changes, and it records straight
// through a gap in which the aircraft went unseen. That is what makes an
// `aircraft lost` event loggable at all, and it is why the row carries a
// `session_active` column: with the gap recorded rather than cut out, something
// has to say the fingerprint was not locked. The `variant` column joins it,
// because a variant is re-read every tick while the filename is stamped once.

#include <cstdio>
#include <string>
#include <vector>

#include "Corrector.h"
#include "LVarBridge.h"
#include "TurnGovernor.h"

// The legacy fuel simvars (CONTEXT.md, "Cross-check"): still read and still
// logged, never an input to a correction. T21 (#24) took them out of the
// corrector's hands entirely, so they reach the log on their own rather than
// riding in on a TickInput that no longer carries them.
struct CrossCheck {
    double tankGallons[maxwarp::kBridgeTankCount] = {};
    double totalGallons = 0.0;
    double ffPph1 = 0.0;   // ENG FUEL FLOW PPH:1 — no longer the clamp's reference
    double ffPph2 = 0.0;   //   (T17: it under-reports by a non-constant ratio)
    double kgPerGallon = 0.0; // FUEL WEIGHT PER GALLON, converted
};

class SessionLogger {
public:
    SessionLogger() = default;
    ~SessionLogger();

    // Prune old logs, then open a fresh CSV for a pairing. tankLabels name the
    // per-tank columns in fixed slot order.
    bool begin(const std::string &variant, const std::vector<std::string> &tankLabels);

    // Append one tick. `in` is the core state that produced `decision`; `bridge`
    // is the bridge's own view of it, latched for the same tick (T20); `cross` is
    // the legacy simvar reading, logged but never consumed.
    //
    // Column *names* are the contract — MaxWarpReplay looks every one of them up
    // by name, so a T20-era capture still replays through the ported corrector.
    // T21 renamed each column whose unit changed rather than reusing the name,
    // so nothing can be read as gallons when it is kg: the counters are now
    // `*_kg`, and per-tank commands are `<tank>_cmd_kg` beside the observed
    // `<tank>_kg`. The gallons `<tank>_cmd` column is gone — the corrector
    // commands nothing on the simvar path any more.
    //
    // T19 (#21) applied the same rule to a column whose *meaning* was wrong
    // rather than its unit: `removed_kg` counted commanded correction and was
    // read as fuel gone, so it is now `removed_cmd_kg`, beside the new
    // `removed_realised_kg` and `removed_measured_kg`.
    // T39 (#58) appended the governor's inputs, its decision and the cruise rate
    // (blank-vs-zero rules in the .cpp), plus its events merged into the same
    // `event` cell — so a captured flight replays through TurnGovernor offline.
    void writeRow(const maxwarp::TickInput &in, const maxwarp::TickDecision &decision,
                  const maxwarp::BridgeSample &bridge, const CrossCheck &cross,
                  const maxwarp::GovernorInput &govIn,
                  const maxwarp::GovernorDecision &govDec, double cruiseRate,
                  const std::string &govEvents);

    void end();
    bool isOpen() const { return m_file != nullptr; }

    // Directory the logs live in (%LOCALAPPDATA%/MaxWarp/logs), created if needed.
    static std::string logDirectory();

private:
    std::FILE *m_file = nullptr;
    std::string m_path;
    std::vector<std::string> m_tankLabels;
};
