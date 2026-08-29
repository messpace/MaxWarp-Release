#include "SimBriefClient.h"

#include <QNetworkReply>
#include <QNetworkRequest>

namespace {

// The fetcher answers in well under a second (T32 measured 0.03 s server-side,
// ~1 MB body); anything past this is the network, not SimBrief thinking.
constexpr int kTransferTimeoutMs = 15000;

} // namespace

SimBriefClient::SimBriefClient(QObject *parent)
    : QObject(parent)
{
}

void SimBriefClient::fetch(const QString &identity, const QString &resolvedUserid)
{
    if (m_reply)
        return; // one at a time; the answer in flight is the answer

    QNetworkRequest request(maxwarp::simbrief::requestUrl(identity, resolvedUserid));
    request.setTransferTimeout(kTransferTimeoutMs);
    m_reply = m_nam.get(request);
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->deleteLater();

        const QByteArray body = reply->readAll();
        maxwarp::simbrief::Result result = maxwarp::simbrief::parse(body);
        // The 400 error bodies parse into their own failure modes; any other
        // transport-level error with no recognisable body is the network's.
        if (reply->error() != QNetworkReply::NoError
            && result.status != maxwarp::simbrief::FetchStatus::UnknownUser
            && result.status != maxwarp::simbrief::FetchStatus::NoPlanFiled) {
            result = maxwarp::simbrief::Result();
            result.status = maxwarp::simbrief::FetchStatus::NetworkDown;
            result.detail = reply->errorString();
        }
        emit finished(result);
    });
}
