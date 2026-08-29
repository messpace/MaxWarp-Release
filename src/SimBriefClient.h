#pragma once

// Thin Qt Network adapter over the SimBrief plan core (T38 · issue #57): one
// GET at a time against xml.fetcher.php, a transfer timeout so "network down"
// and "timed out" collapse into the same answer, and `finished` carrying the
// parsed Result. Fetches are user-initiated only — pairing lock and the
// Refresh button; never polled (T32's operative rule).

#include "SimBriefPlan.h"

#include <QNetworkAccessManager>
#include <QObject>

class QNetworkReply;

class SimBriefClient : public QObject
{
    Q_OBJECT
public:
    explicit SimBriefClient(QObject *parent = nullptr);

    bool busy() const { return m_reply != nullptr; }

    // Start a fetch for the typed identity (the resolved userid, when known,
    // wins — SimBriefPlan's rule). A fetch already in flight is left alone.
    void fetch(const QString &identity, const QString &resolvedUserid = QString());

signals:
    void finished(const maxwarp::simbrief::Result &result);

private:
    QNetworkAccessManager m_nam;
    QNetworkReply *m_reply = nullptr;
};
