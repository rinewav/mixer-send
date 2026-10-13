// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mixersend
{

/** A random, non-zero 64-bit id (instance ids, bus ids, test sender ids). */
std::uint64_t randomNonZeroId() noexcept;

/** 16 lowercase hex digits, the form the companion and the saved state use. */
std::string idToHex (std::uint64_t id);

/** Parses exactly 16 hex digits (either case) into a non-zero id. */
std::optional<std::uint64_t> idFromHex (std::string_view text) noexcept;

} // namespace mixersend
