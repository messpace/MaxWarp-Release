#include "SessionLogger.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <share.h>

using maxwarp::CorrectorState;
using maxwarp::TankWrite;

namespace {

constexpr int kKeepLogs = 20;

std::string nowStamp(const char *fmt)
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

const char *stateWord(CorrectorState s)
{
    switch (s) {
        case CorrectorState::Off: return "OFF";
        case CorrectorState::Standby: return "STANDBY";
        case CorrectorState::Correcting: return "CORRECTING";
        case CorrectorState::Suspended: return "SUSPENDED";
    }
    return "?";
}

// Sanitize a variant label for a filename (e.g. "A330-300" -> "A330-300").
std::string safeName(const std::string &in)
{
    std::string out;
    for (char c : in)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    return out;
}

// Quote a free-text CSV cell. Only two cells carry text we did not author —
// the joined `event` string and `gov_fix`, which is a SimBrief ident copied
// verbatim out of the navlog JSON. Both are wrapped per RFC 4180 (always
// quoted, embedded `"` doubled) and, if the text opens with a spreadsheet
// formula lead-in, prefixed with `'` so Excel reads it as text — `-` matters
// there, since a lat/lon ident can plausibly begin with one. Every other `%s`
// in this writer prints from a closed set of literals and stays bare.
std::string csvCell(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 4);
    out += '"';
    if (!in.empty() && (in[0] == '=' || in[0] == '+' || in[0] == '-' || in[0] == '@'))
        out += '\'';
    for (char c : in) {
        if (c == '"')
            out += '"';
        out += c;
    }
    out += '"';
    return out;
}

} // namespace

SessionLogger::~SessionLogger()
{
    end();
}

std::string SessionLogger::logDirectory()
{
    const char *base = std::getenv("LOCALAPPDATA");
    std::filesystem::path dir =
        base ? std::filesystem::path(base) : std::filesystem::temp_directory_path();
    dir /= "MaxWarp";
    dir /= "logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

bool SessionLogger::begin(const std::string &variant, const std::vector<std::string> &tankLabels)
{
    end();
    m_tankLabels = tankLabels;

    const std::string dir = logDirectory();

    // Prune to the newest kKeepLogs before opening the new one.
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> logs;
    for (const auto &e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".csv")
            logs.push_back(e);
    }
    std::sort(logs.begin(), logs.end(), [](const auto &a, const auto &b) {
        return a.last_write_time() < b.last_write_time();
    });
    // Delete the oldest so that at most kKeepLogs-1 remain — the new file we are
    // about to open brings the directory to kKeepLogs.
    const int toRemove = static_cast<int>(logs.size()) - (kKeepLogs - 1);
    for (int i = 0; i < toRemove; ++i)
        std::filesystem::remove(logs[i].path(), ec);

    const std::string name =
        nowStamp("%Y-%m-%d_%H%M") + "_" + safeName(variant) + ".csv";
    m_path = (std::filesystem::path(dir) / name).string();
    m_file = _fsopen(m_path.c_str(), "w", _SH_DENYWR); // others may read while we log
    if (!m_file)
        return false;

    auto columnName = [](const std::string &label) {
        std::string col = label;
        for (auto &c : col)
            c = (c == ' ') ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return col;
    };

    std::fprintf(m_file, "iso_time,elapsed_s,dt_s,sim_rate,r_eff,state,rebaselined");
    // Cross-check quantities (gallons). The corrector no longer reads these and
    // never writes them, so there is no gallons `_cmd` column to pair them with.
    for (const auto &label : m_tankLabels)
        std::fprintf(m_file, ",%s_obs", columnName(label).c_str());
    // T19 (#21): `removed_kg` is gone, because it meant *commanded* and read as
    // realised — the exact misreading that let T13's log agree with a screen
    // claiming 531 gal removed while the tanks held ~all of it. Its replacements
    // sit side by side deliberately: read apart, `removed_cmd_kg` is the same
    // trap under a new name.
    std::fprintf(m_file,
                 ",ff1_pph,ff2_pph,total_gal,removed_cmd_kg,removed_realised_kg,"
                 "removed_measured_kg,burn_obs_kg,burn_eff_kg,event");
    // Bridge columns (T20). Core state in kg is the authority; the *_obs gallons
    // above are the cross-check, kept for the T12 offline pass.
    std::fprintf(m_file, ",bridge_up,bridge_state");
    for (const auto &label : m_tankLabels)
        std::fprintf(m_file, ",%s_kg", columnName(label).c_str());
    std::fprintf(m_file, ",core_fuel_used_kg,core_refueling");
    // T21 columns: what the corrector commanded on core state this tick, and the
    // density it converted capacities with (so a replay can rebuild them).
    std::fprintf(m_file, ",kg_per_gal");
    for (const auto &label : m_tankLabels)
        std::fprintf(m_file, ",%s_cmd_kg", columnName(label).c_str());
    // T24 (#34). `variant` makes the file self-describing: it is an observation
    // re-read every tick, so the filename — stamped once, at exactly the moment
    // the TITLE/capacity stream race is live — is mere convenience and the column
    // is the record. `session_active` says whether the fingerprint was locked on
    // this tick, which is the only thing that distinguishes a gap now that the log
    // records straight through one.
    std::fprintf(m_file, ",variant,session_active");
    // T25 (#37). Voided intervals were defined by T19 and counted by nothing, so
    // `removed_cmd_kg - removed_measured_kg` was a gap with no attribution — which
    // is how T12's acceptance run came to report "0 voided" while its one voided
    // interval held the entire 11.029 kg of it. Appended, on T24's precedent: a
    // pre-T25 capture must still parse.
    std::fprintf(m_file, ",voided_intervals,voided_kg");
    // T39 (#58): the governor's inputs and its decision, appended on T24/T25's
    // precedent (a pre-T39 capture must still parse). The inputs are everything
    // the core consumed, so a captured flight replays through TurnGovernor
    // offline — the T37/T35 diagnosis loop, without a new harness.
    std::fprintf(m_file, ",lat,lon,gs_kt,track_deg,bank_deg,hdg_true_deg,on_ground,"
                         "gov_stage,gov_fix,gov_angle_deg,gov_dist_nm,"
                         "gov_lead_nm,gov_ceiling,gov_cruise\n");
    std::fflush(m_file);
    return true;
}

void SessionLogger::writeRow(const maxwarp::TickInput &in, const maxwarp::TickDecision &decision,
                             const maxwarp::BridgeSample &bridge, const CrossCheck &cross,
                             const maxwarp::GovernorInput &govIn,
                             const maxwarp::GovernorDecision &govDec, double cruiseRate,
                             const std::string &govEvents)
{
    if (!m_file)
        return;

    std::fprintf(m_file, "%s,%.1f,%.3f,%g,%g,%s,%d",
                 nowStamp("%Y-%m-%dT%H:%M:%S").c_str(), decision.counters.elapsedSeconds,
                 in.dtWall, in.simRate, decision.rEff, stateWord(decision.state),
                 decision.reBaselined ? 1 : 0);

    for (size_t i = 0; i < m_tankLabels.size(); ++i)
        std::fprintf(m_file, ",%.3f",
                     i < maxwarp::kBridgeTankCount ? cross.tankGallons[i] : 0.0);

    // The joined event text for this tick, if any (kept on one CSV cell). The
    // governor's events ride in the same cell (T39), already prefixed.
    std::string events;
    for (const auto &e : decision.events) {
        if (!events.empty())
            events += " | ";
        events += e;
    }
    if (!govEvents.empty()) {
        if (!events.empty())
            events += " | ";
        events += govEvents;
    }

    const std::string eventsCell = csvCell(events);
    std::fprintf(m_file, ",%.1f,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%s", cross.ffPph1,
                 cross.ffPph2, cross.totalGallons, decision.counters.removedCommandedKg,
                 decision.counters.removedRealisedKg, decision.counters.removedMeasuredKg,
                 decision.counters.burnObservedKg, decision.counters.burnEffectiveKg,
                 eventsCell.c_str());

    std::fprintf(m_file, ",%d,%s", bridge.up ? 1 : 0, maxwarp::bridgeStateWord(bridge.state));
    for (size_t i = 0; i < m_tankLabels.size(); ++i) {
        // An unresolved name and an empty tank both read a flat 0 (T17), so the
        // column is left blank rather than claiming a quantity of zero.
        if (i < maxwarp::kBridgeTankCount && bridge.resolved[i])
            std::fprintf(m_file, ",%.3f", bridge.tankKg[i]);
        else
            std::fprintf(m_file, ",");
    }
    if (bridge.haveFuelUsed)
        std::fprintf(m_file, ",%.3f", bridge.fuelUsedKg);
    else
        std::fprintf(m_file, ",");
    std::fprintf(m_file, ",%d", bridge.refueling ? 1 : 0);

    std::fprintf(m_file, ",%.5f", cross.kgPerGallon);
    for (size_t i = 0; i < m_tankLabels.size(); ++i) {
        // Blank where nothing was commanded, so a commanded value and "we left
        // this tank alone" never look alike.
        double commanded = std::nan("");
        for (const TankWrite &w : decision.writes)
            if (static_cast<size_t>(w.tankIndex) == i)
                commanded = w.targetKg;
        if (std::isnan(commanded))
            std::fprintf(m_file, ",");
        else
            std::fprintf(m_file, ",%.3f", commanded);
    }
    std::fprintf(m_file, ",%s,%d", decision.counters.variant.c_str(),
                 decision.counters.active ? 1 : 0);
    std::fprintf(m_file, ",%ld,%.3f", decision.counters.voidedIntervals,
                 decision.counters.voidedKg);
    // Governor block (T39). Ceiling is blank when the governor asks nothing of
    // the rate, so "no ceiling" and "ceiling 0" never look alike; angle,
    // distance and lead are blank while unnamed (the core's -1 sentinels).
    const std::string fixCell = csvCell(govDec.nextFixIdent);
    std::fprintf(m_file, ",%.6f,%.6f,%.1f,%.2f,%.2f,%.2f,%d,%s,%s", govIn.latDeg,
                 govIn.lonDeg, govIn.groundSpeedKt, govIn.groundTrackDeg, govIn.bankDeg,
                 govIn.headingDeg, govIn.onGround ? 1 : 0,
                 govDec.stageWord().c_str(), fixCell.c_str());
    if (govDec.turnAngleDeg >= 0.0)
        std::fprintf(m_file, ",%.1f", govDec.turnAngleDeg);
    else
        std::fprintf(m_file, ",");
    if (govDec.distanceToFixNm >= 0.0)
        std::fprintf(m_file, ",%.2f", govDec.distanceToFixNm);
    else
        std::fprintf(m_file, ",");
    if (govDec.leadNm >= 0.0)
        std::fprintf(m_file, ",%.2f", govDec.leadNm);
    else
        std::fprintf(m_file, ",");
    if (govDec.haveCeiling)
        std::fprintf(m_file, ",%g", govDec.rateCeiling);
    else
        std::fprintf(m_file, ",");
    std::fprintf(m_file, ",%g\n", cruiseRate);
    std::fflush(m_file);
}

void SessionLogger::end()
{
    if (m_file) {
        std::fclose(m_file);
        m_file = nullptr;
    }
}
