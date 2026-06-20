#pragma once

// A cryptobyte-inspired byte reader, ported from the Go lightwalletd
// parser/internal/bytestring. Non-owning view over a buffer; each Read*/Skip
// advances the cursor and reports success. Little-endian integer decoders and
// Bitcoin CompactSize match the Go semantics exactly (the parsed bytes must
// align identically or the produced CompactBlock won't match the Go cache).

#include <cstdint>
#include <string>
#include <string_view>

namespace lyghtd {

class ByteString {
public:
    explicit ByteString(std::string_view data) : data_(data) {}

    bool empty() const { return data_.empty(); }
    size_t size() const { return data_.size(); }
    std::string_view view() const { return data_; }

    // Advance n bytes; false if fewer remain.
    bool Skip(size_t n) {
        if (data_.size() < n) return false;
        data_.remove_prefix(n);
        return true;
    }

    bool ReadByte(uint8_t* out) {
        if (data_.empty()) return false;
        *out = static_cast<uint8_t>(data_[0]);
        data_.remove_prefix(1);
        return true;
    }

    // Copy n bytes into out (replacing its contents).
    bool ReadBytes(std::string* out, size_t n) {
        if (data_.size() < n) return false;
        out->assign(data_.data(), n);
        data_.remove_prefix(n);
        return true;
    }

    bool ReadUint16(uint16_t* out) {
        if (data_.size() < 2) return false;
        uint16_t v = 0;
        for (int i = 1; i >= 0; --i)
            v = static_cast<uint16_t>((v << 8) | static_cast<uint8_t>(data_[i]));
        data_.remove_prefix(2);
        *out = v;
        return true;
    }

    bool ReadUint32(uint32_t* out) {
        if (data_.size() < 4) return false;
        uint32_t v = 0;
        for (int i = 3; i >= 0; --i)
            v = (v << 8) | static_cast<uint8_t>(data_[i]);
        data_.remove_prefix(4);
        *out = v;
        return true;
    }

    bool ReadUint64(uint64_t* out) {
        if (data_.size() < 8) return false;
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i)
            v = (v << 8) | static_cast<uint8_t>(data_[i]);
        data_.remove_prefix(8);
        *out = v;
        return true;
    }

    bool ReadInt32(int32_t* out) {
        uint32_t tmp;
        if (!ReadUint32(&tmp)) return false;
        *out = static_cast<int32_t>(tmp);
        return true;
    }

    // Bitcoin CompactSize, with the same canonical-range checks as Go
    // (rejects non-minimal encodings and values above 0x02000000).
    bool ReadCompactSize(uint64_t* size) {
        *size = 0;
        uint8_t lenByte;
        if (!ReadByte(&lenByte)) return false;

        int lenLen = 0;
        uint64_t length = 0, minSize = 0;
        if (lenByte < 253) {
            length = lenByte;
        } else if (lenByte == 253) {
            lenLen = 2;
            minSize = 253;
        } else if (lenByte == 254) {
            lenLen = 4;
            minSize = 0x10000;
        } else {  // 255: beyond maxCompactSize, unusable
            return false;
        }
        if (lenLen > 0) {
            if (data_.size() < static_cast<size_t>(lenLen)) return false;
            for (int i = lenLen - 1; i >= 0; --i)
                length = (length << 8) | static_cast<uint8_t>(data_[i]);
            data_.remove_prefix(lenLen);
        }
        constexpr uint64_t kMaxCompactSize = 0x02000000;
        if (length > kMaxCompactSize || length < minSize) return false;
        *size = length;
        return true;
    }

    // Read a CompactSize length then that many bytes into out.
    bool ReadCompactLengthPrefixed(std::string* out) {
        uint64_t length;
        if (!ReadCompactSize(&length)) return false;
        return ReadBytes(out, static_cast<size_t>(length));
    }

private:
    std::string_view data_;
};

}  // namespace lyghtd
