#include "AgentTypes.h"
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QImageReader>
#include <QStringView>

namespace rose::agent {
namespace {
constexpr qsizetype maximumImageBytes = 8 * 1024 * 1024;
constexpr qint64 maximumImagePixels = 16 * 1024 * 1024;
constexpr int maximumImageDimension = 8192;
std::variant<QString, AgentError> imageMime(const QByteArray &bytes) {
    if (bytes.isEmpty() || bytes.size() > maximumImageBytes)
        return AgentError{AgentErrorCode::InvalidArguments, "An image must contain between 1 byte and 8 MiB."};
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    const auto format = reader.format().toLower();
    QString mime;
    if (format == "png") mime = "image/png";
    else if (format == "jpeg" || format == "jpg") mime = "image/jpeg";
    else if (format == "webp") mime = "image/webp";
    else return AgentError{AgentErrorCode::InvalidArguments, "Only real PNG, JPEG, and WebP images are supported by the installed image decoders."};
    const auto size = reader.size();
    if (!size.isValid() || size.width() > maximumImageDimension || size.height() > maximumImageDimension
        || qint64(size.width()) * size.height() > maximumImagePixels)
        return AgentError{AgentErrorCode::InvalidArguments, "Image dimensions exceed 8192 per side or 16 megapixels, or are invalid."};
    if (reader.read().isNull())
        return AgentError{AgentErrorCode::InvalidArguments, "The image could not be decoded completely."};
    return mime;
}
}
std::variant<AgentImage, AgentError> loadImage(const QString &path) {
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly))
        return AgentError{AgentErrorCode::InvalidArguments, "Cannot read the selected local image file."};
    if (file.size() < 1 || file.size() > maximumImageBytes)
        return AgentError{AgentErrorCode::InvalidArguments, "An image file must contain between 1 byte and 8 MiB."};
    auto bytes = file.read(maximumImageBytes + 1);
    if (file.error() != QFileDevice::NoError)
        return AgentError{AgentErrorCode::InvalidArguments, "Reading the selected image failed."};
    auto mime = imageMime(bytes);
    if (auto error = std::get_if<AgentError>(&mime)) return *error;
    return AgentImage{std::get<QString>(std::move(mime)), std::move(bytes)};
}
std::optional<AgentError> validateInput(const AgentInput &input, int maxContextBytes) {
    if (QStringView(input.prompt).trimmed().isEmpty() && input.images.isEmpty())
        return AgentError{AgentErrorCode::InvalidArguments, "Provide a request or an image before starting the agent."};
    if (input.images.size() > 4)
        return AgentError{AgentErrorCode::InvalidArguments, "At most four images may be included in one request."};
    qint64 encodedBytes = input.prompt.toUtf8().size();
    for (const auto &image : input.images) {
        if (image.bytes.isEmpty() || image.bytes.size() > maximumImageBytes)
            return AgentError{AgentErrorCode::InvalidArguments, "An image must contain between 1 byte and 8 MiB."};
        encodedBytes += 4 * ((qint64(image.bytes.size()) + 2) / 3) + image.mimeType.size() + 13;
    }
    if (encodedBytes > maxContextBytes)
        return AgentError{AgentErrorCode::ContextBudgetExceeded, "Prompt and encoded images exceed the context byte budget. No image was discarded or reduced."};
    for (const auto &image : input.images) {
        auto mime = imageMime(image.bytes);
        if (auto error = std::get_if<AgentError>(&mime)) return *error;
        if (image.mimeType != std::get<QString>(mime))
            return AgentError{AgentErrorCode::InvalidArguments, "Image MIME type does not match its decoded PNG, JPEG, or WebP content."};
    }
    return std::nullopt;
}
QJsonObject ProviderResponse::assistantMessage() const {
    QJsonObject result{{"role", "assistant"}, {"content", text.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(text)}};
    if (!calls.isEmpty()) {
        QJsonArray array;
        for (const auto &call : calls)
            array.append(QJsonObject{{"id", call.id}, {"type", "function"},
                {"function", QJsonObject{{"name", call.name}, {"arguments", QString::fromUtf8(call.arguments)}}}});
        result.insert("tool_calls", array);
    }
    return result;
}
std::optional<AgentError> validateConfig(const ProviderConfig &config) {
    auto invalid = [](const QString &message) { return std::optional<AgentError>{{AgentErrorCode::Configuration, message}}; };
    if (!config.endpoint.isValid() || config.endpoint.host().isEmpty() || !config.endpoint.userInfo().isEmpty()
        || config.endpoint.hasFragment() || config.endpoint.hasQuery())
        return invalid("Set an absolute endpoint without URL credentials, query, or fragment.");
    if (config.kind == ProviderKind::Hosted && config.endpoint.scheme() != "https")
        return invalid("Hosted providers require HTTPS.");
    if (config.kind == ProviderKind::Local) {
        const auto host = config.endpoint.host();
        if (host != "localhost" && !QHostAddress(host).isLoopback())
            return invalid("Local providers must use a literal loopback address or localhost.");
        if (config.endpoint.scheme() != "http" && config.endpoint.scheme() != "https")
            return invalid("Local providers require HTTP or HTTPS on loopback.");
    }
    if (config.model.trimmed().isEmpty() || config.endpoint.path().isEmpty())
        return invalid("Set the exact chat-completions URL and model identifier.");
    if (config.contextWindowTokens < 0)
        return invalid("Provider-advertised context window cannot be negative.");
    if (config.timeoutMs < 100 || config.timeoutMs > 3600000 || config.maxRounds < 1 || config.maxRounds > 256
        || config.maxToolCalls < 1 || config.maxToolCalls > 1024 || config.maxContextBytes < 1024
        || config.maxContextBytes > 64 * 1024 * 1024 || config.maxResponseBytes < 1024 || config.maxResponseBytes > 64 * 1024 * 1024)
        return invalid("Provider time, call, or byte budgets are outside supported bounds.");
    return std::nullopt;
}
QString errorName(AgentErrorCode code) {
    switch (code) {
    case AgentErrorCode::Configuration: return "Configuration";
    case AgentErrorCode::Credentials: return "Credentials";
    case AgentErrorCode::Authentication: return "Authentication";
    case AgentErrorCode::Network: return "Network";
    case AgentErrorCode::Timeout: return "Timeout";
    case AgentErrorCode::Protocol: return "Protocol";
    case AgentErrorCode::PermissionDenied: return "PermissionDenied";
    case AgentErrorCode::InvalidArguments: return "InvalidArguments";
    case AgentErrorCode::ContextBudgetExceeded: return "ContextBudgetExceeded";
    case AgentErrorCode::CallBudgetExceeded: return "CallBudgetExceeded";
    case AgentErrorCode::StaleRevision: return "StaleRevision";
    case AgentErrorCode::WorkspaceUnavailable: return "WorkspaceUnavailable";
    }
    return "AgentError";
}
QString statusName(RunStatus status) {
    switch (status) {
    case RunStatus::Planning: return "Planning";
    case RunStatus::Reading: return "Reading";
    case RunStatus::Proposing: return "Proposing";
    case RunStatus::AwaitingReview: return "Awaiting review";
    case RunStatus::Applying: return "Applying";
    case RunStatus::Completed: return "Completed";
    case RunStatus::Cancelled: return "Cancelled";
    case RunStatus::Failed: return "Failed";
    }
    return {};
}
} // namespace rose::agent
