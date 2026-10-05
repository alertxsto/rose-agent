#include "StdinReader.h"
#include <QMetaObject>
#include <cstdio>
#include <mutex>
#include <thread>

namespace rose::cli {
struct StdinReader::State {
    std::mutex mutex;
    QObject *receiver;
    std::function<void(QByteArray)> line;
    std::function<void(QString)> finished;
};
StdinReader::StdinReader(QObject *receiver, std::function<void(QByteArray)> line,
                         std::function<void(QString)> finished)
    : state_(std::make_shared<State>()) {
    state_->receiver = receiver;
    state_->line = std::move(line);
    state_->finished = std::move(finished);
    std::thread([state = state_] {
        QByteArray pending;
        while (true) {
            const int character = std::fgetc(stdin);
            if (character != EOF) pending.append(char(character));
            if (character == '\n' || (character == EOF && !pending.isEmpty())) {
                std::lock_guard lock(state->mutex);
                if (!state->receiver) return;
                QMetaObject::invokeMethod(state->receiver,
                    [state, bytes = std::move(pending)]() mutable { state->line(std::move(bytes)); }, Qt::QueuedConnection);
                pending.clear();
            }
            if (character == EOF) {
                const auto error = std::ferror(stdin) ? QStringLiteral("Cannot read stdin") : QString{};
                std::lock_guard lock(state->mutex);
                if (state->receiver) QMetaObject::invokeMethod(state->receiver,
                    [state, error] { state->finished(error); }, Qt::QueuedConnection);
                return;
            }
        }
    }).detach();
}
StdinReader::~StdinReader() {
    std::lock_guard lock(state_->mutex);
    state_->receiver = nullptr;
}
} // namespace rose::cli
