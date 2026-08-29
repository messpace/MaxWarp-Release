#pragma once

// MaxWarp SimBrief plan core (T38 · issue #57) — the pure, Network-free half of
// the SimBrief client: the T32 (#51) identity rule as a request URL, and the
// fetcher's `json=v2` body parsed into the plan the governor consumes and the
// words the settings page shows. Qt Core only, unit-tested headlessly; the
// QNetworkAccessManager adapter (`SimBriefClient`) stays thin.

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVector>

namespace maxwarp {
namespace simbrief {

// What one fetch resolved to. The governor's answer to every failure is the
// same — NO PLAN on the rail — so the *reason* lives here, one word per thing
// to fix (the NO MODULE / NO FUEL VARS precedent). NoIdentity never reaches the
// parser (there is nothing to send); it is the caller's state.
enum class FetchStatus {
    Success,
    UnknownUser,   // identity not found on simbrief.com
    NoPlanFiled,   // known account, no OFP on file
    NetworkDown,   // no body ever arrived: offline, DNS, timeout
    NoUsableFixes, // a body arrived but nothing the governor can fly
};

// One navlog fix the governor can fly. `isSidStar` rides along (T32 keeps it)
// even though the core's PlanFix drops it — every fix counts for turns.
struct Fix {
    QString ident;
    double latDeg = 0.0;
    double lonDeg = 0.0;
    bool isSidStar = false;
};

struct Plan {
    QString requestId;   // params.request_id — the plan's identity (T32 §4)
    QString generatedAt; // params.time_generated, ISO 8601 under json=v2
    QString originIcao;
    QString destinationIcao;
    // The origin aerodrome: never a fix, but the first leg starts there.
    bool haveOrigin = false;
    double originLatDeg = 0.0;
    double originLonDeg = 0.0;
    QVector<Fix> fixes; // TOC/TOD dropped, destination kept, in navlog order
    QString route() const // the settings page's headline: "EHAM → LFMN"
    {
        return originIcao + QStringLiteral(" → ") + destinationIcao;
    }
};

struct Result {
    FetchStatus status = FetchStatus::NetworkDown;
    QString detail;         // human words for the event line
    QString resolvedUserid; // fetch.userid whenever the server named one
    Plan plan;              // meaningful only when status == Success
};

// The T32 identity rule: trim; 1-7 digits -> `userid=`, anything else ->
// `username=`; never fall back across the two (all-digit aliases exist, and a
// fallback can quietly fetch a stranger's plan). A previously resolved numeric
// userid wins over whatever was typed.
QUrl requestUrl(const QString &identity, const QString &resolvedUserid = QString());

// Parse a fetcher response body (`json=v2`). Callers pass whatever arrived —
// success and the 400 error bodies alike; a body that parses as neither is
// NoUsableFixes with the reason in `detail`.
Result parse(const QByteArray &body);

} // namespace simbrief
} // namespace maxwarp
