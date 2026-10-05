#pragma once
#include <QObject>
#include <QByteArray>
#include <QString>
#include <functional>
#include <memory>

namespace rose::cli {
// Stdio stays on a detached reader thread; only queued callbacks touch Qt state.
// Destruction disconnects the receiver even when a terminal is still blocking.
class StdinReader final {
public:
    StdinReader(QObject *receiver, std::function<void(QByteArray)> line,
                std::function<void(QString)> finished);
    ~StdinReader();
    StdinReader(const StdinReader &) = delete;
    StdinReader &operator=(const StdinReader &) = delete;
private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace rose::cli
