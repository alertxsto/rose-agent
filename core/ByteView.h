#pragma once

#include <QByteArray>
#include <QString>
#include <cstring>

namespace rose {
// Borrows immutable bytes. Views and parser index keys remain valid only while
// their source QByteArray storage is alive and unchanged; slicing never copies.
class ByteView final {
public:
    constexpr ByteView() noexcept = default;
    constexpr ByteView(const char *data, qsizetype size) noexcept : data_(data), size_(size) {}
    ByteView(const char *text) noexcept : ByteView(text, qsizetype(std::strlen(text))) {}
    ByteView(const QByteArray &bytes) noexcept : ByteView(bytes.constData(), bytes.size()) {}
    constexpr const char *data() const noexcept { return data_; }
    constexpr qsizetype size() const noexcept { return size_; }
    constexpr bool isEmpty() const noexcept { return size_ == 0; }
    constexpr const char *begin() const noexcept { return data_; }
    constexpr const char *end() const noexcept { return size_ ? data_ + size_ : data_; }
    constexpr char operator[](qsizetype index) const noexcept { return data_[index]; }
    constexpr char front() const noexcept { return data_[0]; }
    constexpr char back() const noexcept { return data_[size_ - 1]; }
    constexpr ByteView sliced(qsizetype offset, qsizetype length) const noexcept {
        return {offset ? data_ + offset : data_, length};
    }
    QByteArray toByteArray() const { return QByteArray(data_, size_); }
    QString toLatin1String() const { return QString::fromLatin1(data_, size_); }
    friend bool operator==(ByteView a, ByteView b) noexcept {
        return a.size_ == b.size_ && (!a.size_ || std::memcmp(a.data_, b.data_, size_t(a.size_)) == 0);
    }
    friend bool operator==(ByteView a, const QByteArray &b) noexcept {
        return a == ByteView(b);
    }
    friend bool operator==(ByteView a, const char *b) noexcept {
        return a == ByteView(b);
    }
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    friend uint qHash(ByteView bytes, uint seed = 0) noexcept {
#else
    friend size_t qHash(ByteView bytes, size_t seed = 0) noexcept {
#endif
        // Hash directly over borrowed storage, including embedded NUL bytes.
        auto hash = seed;
        for (char byte : bytes) hash = (hash ^ static_cast<unsigned char>(byte)) * 16777619u;
        return hash;
    }
private:
    const char *data_ = nullptr;
    qsizetype size_ = 0;
};
} // namespace rose
