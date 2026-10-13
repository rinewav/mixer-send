// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#if defined(_WIN32) && ! defined(_CRT_RAND_S)
 #define _CRT_RAND_S // rand_s (RtlGenRandom, kernel-seeded); must precede the first <stdlib.h>
#endif

#include "core/InstanceId.h"

#include <cstdio>

#if defined(__APPLE__)
 #include <stdlib.h> // arc4random_buf
#elif defined(_WIN32)
 #include <stdlib.h> // rand_s
#else
 #include <random>
#endif

namespace mixersend
{

std::uint64_t randomNonZeroId() noexcept
{
    std::uint64_t id = 0;

    while (id == 0)
    {
       #if defined(__APPLE__)
        // Kernel-seeded, never blocks, no state to share between threads.
        arc4random_buf (&id, sizeof (id));
       #elif defined(_WIN32)
        unsigned int high = 0, low = 0;
        if (rand_s (&high) != 0 || rand_s (&low) != 0)
            continue;
        id = (static_cast<std::uint64_t> (high) << 32) | low;
       #else
        static thread_local std::mt19937_64 engine { std::random_device {}() };
        id = engine();
       #endif
    }

    return id;
}

std::string idToHex (std::uint64_t id)
{
    char text[17] {};
    std::snprintf (text, sizeof (text), "%016llx", static_cast<unsigned long long> (id));
    return text;
}

std::optional<std::uint64_t> idFromHex (std::string_view text) noexcept
{
    if (text.size() != 16)
        return std::nullopt;

    std::uint64_t value = 0;

    for (const char c : text)
    {
        int digit = -1;

        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;

        if (digit < 0)
            return std::nullopt;

        value = (value << 4) | static_cast<std::uint64_t> (digit);
    }

    if (value == 0)
        return std::nullopt;

    return value;
}

} // namespace mixersend
