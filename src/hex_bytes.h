#pragma once

// HexBytes — a binary blob that glaze deserializes directly from a JSON hex
// string, decoding straight from the response buffer during the parse (no
// intermediate hex std::string, no separate decode pass). Used for the raw
// getblock results in the all-raw batch path.

#include <array>
#include <cstdint>
#include <string>

#include <glaze/glaze.hpp>

namespace lyghtd {
struct HexBytes {
    std::string bytes;
};
}  // namespace lyghtd

template <>
struct glz::from<glz::JSON, lyghtd::HexBytes> {
    static constexpr std::array<uint8_t, 256> kLut = [] {
        std::array<uint8_t, 256> t{};
        for (auto& v : t) v = 0xff;
        for (int i = 0; i < 10; ++i) t[static_cast<size_t>('0' + i)] =
            static_cast<uint8_t>(i);
        for (int i = 0; i < 6; ++i) {
            t[static_cast<size_t>('a' + i)] = static_cast<uint8_t>(10 + i);
            t[static_cast<size_t>('A' + i)] = static_cast<uint8_t>(10 + i);
        }
        return t;
    }();

    template <auto Options>
    static void op(lyghtd::HexBytes& value, is_context auto&& ctx, auto&& it,
                   auto end) {
        constexpr auto Opts = ws_handled_off<Options>();
        if constexpr (!check_ws_handled(Options)) {
            if (skip_ws<Opts>(ctx, it, end)) return;
        }
        if (it == end || *it != '"') {
            ctx.error = error_code::expected_quote;
            return;
        }
        ++it;  // opening quote
        std::string& out = value.bytes;
        out.clear();
        for (;;) {
            if (it == end) {
                ctx.error = error_code::unexpected_end;
                return;
            }
            if (*it == '"') {
                ++it;  // closing quote
                break;
            }
            uint8_t hi = kLut[static_cast<uint8_t>(*it)];
            ++it;
            if (it == end) {
                ctx.error = error_code::unexpected_end;
                return;
            }
            uint8_t lo = kLut[static_cast<uint8_t>(*it)];
            ++it;
            if ((hi | lo) & 0x80) {  // 0xff (non-hex) sets the high bit
                ctx.error = error_code::syntax_error;
                return;
            }
            out.push_back(static_cast<char>((hi << 4) | lo));
        }
    }
};
