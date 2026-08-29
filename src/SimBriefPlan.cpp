#include "SimBriefPlan.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QUrlQuery>

namespace maxwarp {
namespace simbrief {

namespace {

bool isPilotId(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("^\\d{1,7}$"));
    return re.match(s).hasMatch();
}

} // namespace

QUrl requestUrl(const QString &identity, const QString &resolvedUserid)
{
    const QString typed = identity.trimmed();
    const QString resolved = resolvedUserid.trimmed();
    QUrlQuery query;
    if (!resolved.isEmpty())
        query.addQueryItem(QStringLiteral("userid"), resolved);
    else if (isPilotId(typed))
        query.addQueryItem(QStringLiteral("userid"), typed);
    else
        query.addQueryItem(QStringLiteral("username"), typed);
    query.addQueryItem(QStringLiteral("json"), QStringLiteral("v2"));
    QUrl url(QStringLiteral("https://www.simbrief.com/api/xml.fetcher.php"));
    url.setQuery(query);
    return url;
}

Result parse(const QByteArray &body)
{
    Result r;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (!doc.isObject()) {
        r.status = FetchStatus::NoUsableFixes;
        r.detail = QStringLiteral("response was not a plan (%1)")
                       .arg(err.error == QJsonParseError::NoError
                                ? QStringLiteral("no JSON object")
                                : err.errorString());
        return r;
    }
    const QJsonObject root = doc.object();
    const QJsonObject fetch = root.value(QLatin1String("fetch")).toObject();
    // fetch.userid names the resolved Pilot ID even on the no-plan error (T32).
    r.resolvedUserid = fetch.value(QLatin1String("userid")).toString().trimmed();

    const QString status = fetch.value(QLatin1String("status")).toString();
    if (status != QLatin1String("Success")) {
        const QString reason = QString(status).remove(QLatin1String("Error: "));
        if (status.contains(QLatin1String("Unknown UserID"))) {
            r.status = FetchStatus::UnknownUser;
            r.resolvedUserid.clear(); // nothing was resolved
        } else if (status.contains(QLatin1String("No flight plan on file"))) {
            r.status = FetchStatus::NoPlanFiled;
        } else {
            r.status = FetchStatus::NoUsableFixes;
        }
        r.detail = reason.isEmpty() ? QStringLiteral("empty fetch status") : reason;
        return r;
    }

    const QJsonObject params = root.value(QLatin1String("params")).toObject();
    r.plan.requestId = params.value(QLatin1String("request_id")).toString();
    r.plan.generatedAt = params.value(QLatin1String("time_generated")).toString();

    const QJsonObject origin = root.value(QLatin1String("origin")).toObject();
    const QJsonObject dest = root.value(QLatin1String("destination")).toObject();
    r.plan.originIcao = origin.value(QLatin1String("icao_code")).toString();
    r.plan.destinationIcao = dest.value(QLatin1String("icao_code")).toString();
    const QString oLat = origin.value(QLatin1String("pos_lat")).toString();
    const QString oLon = origin.value(QLatin1String("pos_long")).toString();
    bool okLat = false, okLon = false;
    const double oLatD = oLat.toDouble(&okLat);
    const double oLonD = oLon.toDouble(&okLon);
    if (okLat && okLon) {
        r.plan.haveOrigin = true;
        r.plan.originLatDeg = oLatD;
        r.plan.originLonDeg = oLonD;
    }

    // The navlog: TOC/TOD dropped on ident (never on type — NAT lat/lon fixes
    // are "ltlg" too), destination kept, every scalar a string (T32).
    const QJsonArray navlog = root.value(QLatin1String("navlog")).toArray();
    for (const QJsonValue &v : navlog) {
        const QJsonObject o = v.toObject();
        Fix f;
        f.ident = o.value(QLatin1String("ident")).toString();
        if (f.ident == QLatin1String("TOC") || f.ident == QLatin1String("TOD"))
            continue;
        bool okA = false, okB = false;
        f.latDeg = o.value(QLatin1String("pos_lat")).toString().toDouble(&okA);
        f.lonDeg = o.value(QLatin1String("pos_long")).toString().toDouble(&okB);
        f.isSidStar = o.value(QLatin1String("is_sid_star")).toString() == QLatin1String("1");
        if (f.ident.isEmpty() || !okA || !okB)
            continue; // a fix the governor cannot place is no fix
        r.plan.fixes.append(f);
    }
    if (r.plan.fixes.isEmpty()) {
        r.status = FetchStatus::NoUsableFixes;
        r.detail = QStringLiteral("plan %1 held no usable fixes").arg(r.plan.route());
        return r;
    }
    r.status = FetchStatus::Success;
    return r;
}

} // namespace simbrief
} // namespace maxwarp
