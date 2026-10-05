#pragma once
#include "AgentTypes.h"
#include <QNetworkAccessManager>
#include <QPointer>
#include <map>
#include <memory>

namespace rose::agent {
// Each instance and its network manager stay on their QObject owning thread.
class Provider final : public QObject {
    Q_OBJECT
public:
    explicit Provider(QObject *parent = nullptr);
    ~Provider() override;
    RequestId complete(const ProviderConfig &, const QJsonArray &messages, const QJsonArray &tools, const QByteArray &key);
    RequestId complete(const ProviderConfig &config, const QJsonObject &message, const QJsonArray &tools, const QByteArray &key) {
        return complete(config, QJsonArray{message}, tools, key);
    }
    void cancel(RequestId);
    void cancelAll();
signals:
    void completed(rose::agent::RequestId, rose::agent::ProviderResult);
private:
    struct Pending;
    void consume(RequestId);
    void finish(RequestId);
    void fail(RequestId, AgentError);
    bool parseEvent(Pending &, const QByteArray &);
    bool parseJson(Pending &, const QJsonObject &, bool streaming);
    QNetworkAccessManager network_;
    std::map<RequestId, std::unique_ptr<Pending>> pending_;
    RequestId sequence_ = 0;
};
} // namespace rose::agent
