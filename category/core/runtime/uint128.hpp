// Copyright (C) 2025-26 Category Labs, Inc.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#pragma once

#include <category/core/assert.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace monad::vm::runtime
{

    struct uint128_t
    {
        uint64_t lo{};
        uint64_t hi{};

        constexpr uint128_t() noexcept = default;

        constexpr explicit uint128_t(uint64_t v) noexcept
            : lo(v)
            , hi(0)
        {
        }

        constexpr explicit operator uint64_t() const noexcept
        {
            return lo;
        }

        constexpr explicit operator unsigned __int128() const noexcept
        {
            return (static_cast<unsigned __int128>(hi) << 64) | lo;
        }

        [[nodiscard]] static constexpr uint128_t
        from_string(char const *const s)
        {
            MONAD_ASSERT(s != nullptr);
            uint128_t result{};
            char const *p = s;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                constexpr size_t max_hex_digits = sizeof(uint128_t) * 2;
                size_t num_digits = 0;
                if (*p == '\0') {
                    throw std::invalid_argument(s);
                }
                while (*p != '\0') {
                    uint8_t d;
                    if (*p >= '0' && *p <= '9') {
                        d = static_cast<uint8_t>(*p - '0');
                    }
                    else if (*p >= 'a' && *p <= 'f') {
                        d = static_cast<uint8_t>(*p - 'a' + 10);
                    }
                    else if (*p >= 'A' && *p <= 'F') {
                        d = static_cast<uint8_t>(*p - 'A' + 10);
                    }
                    else {
                        throw std::invalid_argument(s);
                    }
                    if (++num_digits > max_hex_digits) {
                        throw std::out_of_range(s);
                    }
                    result.hi = (result.hi << 4) | (result.lo >> 60);
                    result.lo = (result.lo << 4) | d;
                    ++p;
                }
            }
            else {
                // UINT128_MAX / 10; any result larger than this overflows when
                // multiplied by 10
                constexpr unsigned __int128 max_before_mul10 =
                    ((static_cast<unsigned __int128>(UINT64_MAX) << 64) |
                     UINT64_MAX) /
                    10;
                constexpr unsigned __int128 uint128_max =
                    (static_cast<unsigned __int128>(UINT64_MAX) << 64) |
                    UINT64_MAX;
                if (*p == '\0') {
                    throw std::invalid_argument(s);
                }
                while (*p != '\0') {
                    if (*p < '0' || *p > '9') {
                        throw std::invalid_argument(s);
                    }
                    auto const digit = static_cast<uint8_t>(*p - '0');
                    unsigned __int128 r =
                        (static_cast<unsigned __int128>(result.hi) << 64) |
                        result.lo;
                    if (r > max_before_mul10) {
                        throw std::out_of_range(s);
                    }
                    r *= 10;
                    if (r > uint128_max - digit) {
                        throw std::out_of_range(s);
                    }
                    r += digit;
                    result.lo = static_cast<uint64_t>(r);
                    result.hi = static_cast<uint64_t>(r >> 64);
                    ++p;
                }
            }
            return result;
        }
    };

    static_assert(sizeof(uint128_t) == 16);
    static_assert(alignof(uint128_t) == 8);
    static_assert(std::is_trivially_copyable_v<uint128_t>);

    [[nodiscard]] constexpr bool operator==(uint128_t a, uint128_t b) noexcept
    {
        return a.lo == b.lo && a.hi == b.hi;
    }

    [[nodiscard]] constexpr uint128_t
    operator*(uint128_t a, uint128_t b) noexcept
    {
        auto const r = static_cast<unsigned __int128>(a) *
                       static_cast<unsigned __int128>(b);
        uint128_t result;
        result.lo = static_cast<uint64_t>(r);
        result.hi = static_cast<uint64_t>(r >> 64);
        return result;
    }

    [[nodiscard]] constexpr uint128_t
    operator/(uint128_t a, uint64_t b) noexcept
    {
        MONAD_ASSERT(b != 0);
        auto const r = static_cast<unsigned __int128>(a) / b;
        uint128_t result;
        result.lo = static_cast<uint64_t>(r);
        result.hi = static_cast<uint64_t>(r >> 64);
        return result;
    }

    [[nodiscard]] constexpr uint128_t
    operator>>(uint128_t x, uint64_t shift) noexcept
    {
        MONAD_ASSERT(shift < 128);
        auto const r = static_cast<unsigned __int128>(x) >> shift;
        uint128_t result;
        result.lo = static_cast<uint64_t>(r);
        result.hi = static_cast<uint64_t>(r >> 64);
        return result;
    }

    [[nodiscard]] constexpr uint128_t byteswap(uint128_t x) noexcept
    {
        uint128_t result;
        result.lo = __builtin_bswap64(x.hi);
        result.hi = __builtin_bswap64(x.lo);
        return result;
    }

    [[nodiscard]] inline std::string to_string(uint128_t v, int base = 10)
    {
        MONAD_ASSERT(base >= 2 && base <= 16);
        static constexpr char digits[] = "0123456789abcdef";
        auto r = static_cast<unsigned __int128>(v);
        if (r == 0) {
            return "0";
        }
        std::string result;
        while (r != 0) {
            result += digits[r % static_cast<unsigned>(base)];
            r /= static_cast<unsigned>(base);
        }
        std::reverse(result.begin(), result.end());
        return result;
    }

    [[nodiscard]] consteval uint128_t operator""_u128(char const *s)
    {
        return uint128_t::from_string(s);
    }

} // namespace monad::vm::runtime

template <>
struct std::numeric_limits<monad::vm::runtime::uint128_t>
{
    static constexpr bool is_specialized = true;
    static constexpr bool is_integer = true;
    static constexpr bool is_signed = false;
    static constexpr int digits = 128;

    static constexpr monad::vm::runtime::uint128_t max() noexcept
    {
        monad::vm::runtime::uint128_t r;
        r.lo = UINT64_MAX;
        r.hi = UINT64_MAX;
        return r;
    }

    static constexpr monad::vm::runtime::uint128_t min() noexcept
    {
        return {};
    }
};
