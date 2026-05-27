/*
 * Copyright (C) 2025 toytoi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace gdf
{

constexpr std::array<std::string_view, 3> default_dict_filenames{
	"JPDB_v2.2_Frequency_2024-10-13.zip",
	"H_Freq.zip",
	"vn_freq.zip",
};
constexpr std::string_view fallback_data_dir{"/usr/share/gd-frequency"};
constexpr std::string_view default_cache_filename{"dict.bin"};
constexpr std::string_view builder_binary_name{"gd-frequency-build-cache"};

constexpr std::uint32_t cache_version = 3;
constexpr std::uint32_t header_size = 32;
constexpr std::uint32_t dictionary_descriptor_size = 88;
#ifndef GDF_DEFAULT_BLOCK_SIZE
#define GDF_DEFAULT_BLOCK_SIZE 64
#endif
constexpr std::uint32_t default_block_size = GDF_DEFAULT_BLOCK_SIZE;
constexpr std::array<char, 8> cache_magic{
	'G', 'D', 'F', 'R', 'Q', 'C', '4', '\0'};

enum class frequency_kind : std::uint8_t {
	integer = 0,
	text = 1,
	decimal1 = 2,
	range = 3,
};

// ---------------------------------------------------------------------------
// Byte I/O (little-endian on disk).
// ---------------------------------------------------------------------------

template <typename T>
[[nodiscard]] inline auto
read_le(const std::uint8_t *data) noexcept -> T
{
	static_assert(std::is_trivially_copyable_v<T>);
	T value;
	std::memcpy(&value, data, sizeof(T));
	if constexpr (std::endian::native == std::endian::big) {
		value = std::byteswap(value);
	}
	return value;
}

template <typename T>
inline void
append_le(std::vector<char> &out, T value)
{
	static_assert(std::is_trivially_copyable_v<T>);
	if constexpr (std::endian::native == std::endian::big) {
		value = std::byteswap(value);
	}
	const auto *bytes = reinterpret_cast<const char *>(&value);
	out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
inline void
write_le_at(std::span<char> out, std::size_t offset, T value)
{
	static_assert(std::is_trivially_copyable_v<T>);
	if constexpr (std::endian::native == std::endian::big) {
		value = std::byteswap(value);
	}
	std::memcpy(out.data() + offset, &value, sizeof(T));
}

inline void
append_varint(std::vector<char> &out, std::uint64_t value)
{
	while (value >= 0x80U) {
		out.push_back(static_cast<char>((value & 0x7fU) | 0x80U));
		value >>= 7U;
	}
	out.push_back(static_cast<char>(value));
}

[[nodiscard]] inline auto
read_varint(const std::uint8_t *&cursor,
	const std::uint8_t *end,
	std::uint64_t &value) noexcept -> bool
{
	value = 0;
	std::uint32_t shift = 0;
	while (cursor < end && shift <= 63U) {
		const std::uint8_t byte = *cursor++;
		value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
		if ((byte & 0x80U) == 0) {
			return true;
		}
		shift += 7U;
	}
	return false;
}

[[nodiscard]] inline auto
checked_u32(std::uint64_t value, std::string_view field) -> std::uint32_t
{
	if (value > std::numeric_limits<std::uint32_t>::max()) {
		throw std::runtime_error(
			std::string{field} + " exceeds cache format limit");
	}
	return static_cast<std::uint32_t>(value);
}

[[nodiscard]] inline auto
range_in_buffer(std::uint64_t offset,
	std::uint64_t size,
	std::size_t total) noexcept -> bool
{
	return offset <= total && size <= total - offset;
}

[[nodiscard]] inline auto
source_mtime_stamp(const std::string &path) -> std::uint64_t
{
	return static_cast<std::uint64_t>(std::filesystem::last_write_time(path)
						 .time_since_epoch()
						 .count());
}

[[nodiscard]] inline auto
runtime_data_dir() -> std::filesystem::path
{
	std::error_code error;
	const std::filesystem::path executable =
		std::filesystem::read_symlink("/proc/self/exe", error);
	if (!error) {
		const std::filesystem::path candidate =
			executable.parent_path().parent_path() / "share" /
			"gd-frequency";
		if (std::filesystem::exists(candidate, error)) {
			return candidate;
		}
	}
	return std::filesystem::path{fallback_data_dir};
}

} // namespace gdf
