#pragma once

#include "ByteView.h"
#include <limits>
#include <variant>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QTextCodec>
#else
#include <QtCore5Compat/QTextCodec>
#endif

namespace rose::sourceEncoding {
enum class Error { Unsupported, Invalid };
template<class T> using Result = std::variant<T, Error>;

inline Result<QString> decode(ByteView bytes, const QByteArray &encoding) {
    if (bytes.size() > std::numeric_limits<int>::max()) return Error::Invalid;
    auto *codec = QTextCodec::codecForName(encoding);
    if (!codec) return Error::Unsupported;
    QTextCodec::ConverterState state(QTextCodec::ConvertInvalidToNull);
    auto text = codec->toUnicode(bytes.data(), int(bytes.size()), &state);
    if (state.invalidChars || state.remainingChars) return Error::Invalid;
    return text;
}

inline Result<QByteArray> encode(const QString &text, const QByteArray &encoding) {
    if (text.size() > std::numeric_limits<int>::max()) return Error::Invalid;
    auto *codec = QTextCodec::codecForName(encoding);
    if (!codec) return Error::Unsupported;
    QTextCodec::ConverterState state(QTextCodec::ConvertInvalidToNull | QTextCodec::IgnoreHeader);
    auto bytes = codec->fromUnicode(text.constData(), int(text.size()), &state);
    if (state.invalidChars || state.remainingChars) return Error::Invalid;
    // Some legacy codecs substitute silently; reject any lossy round trip,
    // including malformed UTF-16, instead of changing user source text.
    const auto decoded = decode(bytes, encoding);
    const auto *roundTrip = std::get_if<QString>(&decoded);
    if (!roundTrip || *roundTrip != text) return Error::Invalid;
    return bytes;
}
} // namespace rose::sourceEncoding
