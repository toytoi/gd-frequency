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

#include "common.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <libdeflate.h>
#include <numeric>
#include <optional>
#include <simdjson.h>
#include <thread>
#include <unordered_map>
#include <utility>

using namespace gdf;

namespace
{

constexpr std::string_view help_text =
	R"EOF(usage: gd-frequency-build-cache [OPTIONS]

Rebuild the gd-frequency cache from Yomitan frequency dictionaries.

OPTIONS
   --dict-path /path/to/dict.zip  Yomitan frequency dictionary zip
                                  (provide once per dictionary)
   --bin-path /path/to/bin        path to compact cache data
   --build-cache                  redundant; accepted for compatibility
   -h, --help                     show this help
)EOF";

void
write_stderr_line(std::string_view text) noexcept
{
	std::fwrite(text.data(), 1, text.size(), stderr);
	std::fputc('\n', stderr);
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

struct builder_params {
	std::vector<std::string> dict_paths;
	std::string path_to_bin;
	bool show_help = false;

	void
	assign(int argc, char *argv[])
	{
		for (int i = 1; i < argc; ++i) {
			const std::string_view key{argv[i]};
			if (key == "-h" || key == "--help") {
				show_help = true;
				continue;
			}
			if (key == "--build-cache") {
				continue;
			}
			auto require_value = [&]() -> std::string_view {
				if (i + 1 >= argc) {
					throw std::invalid_argument(
						"Missing value for argument: " +
						std::string{key});
				}
				return argv[++i];
			};
			if (key == "--dict-path") {
				dict_paths.emplace_back(require_value());
			} else if (key == "--bin-path") {
				path_to_bin = require_value();
			} else {
				throw std::invalid_argument(
					"Unknown argument: " + std::string{key});
			}
		}
	}
};

void
apply_default_paths(builder_params &params)
{
	const auto data_dir = runtime_data_dir();
	if (params.dict_paths.empty()) {
		for (const std::string_view filename : default_dict_filenames) {
			const auto candidate = data_dir / filename;
			std::error_code error;
			if (std::filesystem::exists(candidate, error)) {
				params.dict_paths.push_back(candidate.string());
			}
		}
	}
	if (params.path_to_bin.empty()) {
		params.path_to_bin =
			(data_dir / default_cache_filename).string();
	}
}

// ---------------------------------------------------------------------------
// Frequency representation (build-side mutable form).
// ---------------------------------------------------------------------------

struct frequency_value {
	frequency_kind kind = frequency_kind::integer;
	std::uint64_t first = 0;
	std::uint64_t second = 0;
	std::string text;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

[[nodiscard]] auto
common_prefix_size(std::string_view lhs, std::string_view rhs) noexcept
	-> std::size_t
{
	const std::size_t limit = std::min(lhs.size(), rhs.size());
	std::size_t index = 0;
	while (index < limit && lhs[index] == rhs[index]) {
		++index;
	}
	return index;
}

[[nodiscard]] auto
parse_unsigned(std::string_view text, std::uint64_t &value) noexcept -> bool
{
	if (text.empty()) {
		return false;
	}
	const auto *first = text.data();
	const auto *last = first + text.size();
	auto [ptr, ec] = std::from_chars(first, last, value);
	return ec == std::errc{} && ptr == last;
}

[[nodiscard]] auto
parse_range_display(std::string_view text, frequency_value &value) noexcept
	-> bool
{
	const std::size_t separator = text.find('-');
	if (separator == std::string_view::npos) {
		return false;
	}
	std::uint64_t first = 0;
	std::uint64_t second = 0;
	if (!parse_unsigned(text.substr(0, separator), first) ||
		!parse_unsigned(text.substr(separator + 1), second)) {
		return false;
	}
	value = {.kind = frequency_kind::range,
		.first = first,
		.second = second,
		.text = {}};
	return true;
}

[[nodiscard]] auto
parse_decimal1_display(std::string_view text, frequency_value &value) noexcept
	-> bool
{
	const std::size_t separator = text.find('.');
	if (separator == std::string_view::npos ||
		text.size() != separator + 2) {
		return false;
	}
	if (text[separator + 1] < '0' || text[separator + 1] > '9') {
		return false;
	}
	std::uint64_t whole = 0;
	if (!parse_unsigned(text.substr(0, separator), whole)) {
		return false;
	}
	if (whole > (std::numeric_limits<std::uint64_t>::max() - 9U) / 10U) {
		return false;
	}
	value = {.kind = frequency_kind::decimal1,
		.first = whole * 10U +
			static_cast<std::uint64_t>(text[separator + 1] - '0'),
		.second = 0,
		.text = {}};
	return true;
}

[[nodiscard]] auto
dictionary_name_from_path(const std::string &path) -> std::string
{
	const std::filesystem::path fs_path{path};
	std::string name = fs_path.stem().string();
	if (name.empty()) {
		name = fs_path.filename().string();
	}
	if (name.empty()) {
		name = path;
	}
	return name;
}

[[nodiscard]] auto
term_meta_bank_number(std::string_view name) noexcept -> std::uint32_t
{
	constexpr std::string_view prefix{"term_meta_bank_"};
	constexpr std::string_view suffix{".json"};
	if (!name.starts_with(prefix) || !name.ends_with(suffix)) {
		return std::numeric_limits<std::uint32_t>::max();
	}
	std::uint32_t value = 0;
	const std::string_view digits = name.substr(
		prefix.size(), name.size() - prefix.size() - suffix.size());
	if (digits.empty()) {
		return std::numeric_limits<std::uint32_t>::max();
	}
	if (std::from_chars(
		    digits.data(), digits.data() + digits.size(), value)
			.ec != std::errc{}) {
		return std::numeric_limits<std::uint32_t>::max();
	}
	return value;
}

// ---------------------------------------------------------------------------
// Zip archive (deflate / stored only).
// ---------------------------------------------------------------------------

struct zip_entry {
	std::string name;
	std::uint16_t flags = 0;
	std::uint16_t method = 0;
	std::uint32_t compressed_size = 0;
	std::uint32_t uncompressed_size = 0;
	std::uint32_t local_header_offset = 0;
};

class zip_archive
{
      public:
	explicit zip_archive(const std::string &path)
	{
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input.is_open()) {
			throw std::runtime_error(
				"Failed to open zip dictionary: " + path);
		}
		const std::streamoff size = input.tellg();
		if (size < 0) {
			throw std::runtime_error(
				"Failed to read zip dictionary size: " + path);
		}
		bytes_.resize(static_cast<std::size_t>(size));
		input.seekg(0);
		input.read(reinterpret_cast<char *>(bytes_.data()), size);
		if (!input) {
			throw std::runtime_error(
				"Failed to read zip dictionary: " + path);
		}
		parse_central_directory(path);
	}

	[[nodiscard]] auto
	entries() const noexcept -> const std::vector<zip_entry> &
	{
		return entries_;
	}

	[[nodiscard]] auto
	read_text_file(std::string_view name) const -> std::string
	{
		const auto it = index_.find(name);
		if (it == index_.end()) {
			throw std::runtime_error(
				"Missing zip entry: " + std::string{name});
		}
		return extract(entries_[it->second]);
	}

      private:
	void
	parse_central_directory(const std::string &path)
	{
		if (bytes_.size() < 22) {
			throw std::runtime_error(
				"Invalid zip dictionary: " + path);
		}

		const std::size_t search_start = bytes_.size() > 22 + 65535
			? bytes_.size() - (22 + 65535)
			: 0;
		std::optional<std::size_t> eocd_offset;
		for (std::size_t pos = bytes_.size() - 22;; --pos) {
			if (read_le<std::uint32_t>(bytes_.data() + pos) ==
				0x06054b50U) {
				eocd_offset = pos;
				break;
			}
			if (pos == search_start) {
				break;
			}
		}
		if (!eocd_offset) {
			throw std::runtime_error(
				"Missing zip central directory: " + path);
		}

		const auto *eocd = bytes_.data() + *eocd_offset;
		const auto disk_number = read_le<std::uint16_t>(eocd + 4);
		const auto cdisk = read_le<std::uint16_t>(eocd + 6);
		const auto entry_count = read_le<std::uint16_t>(eocd + 10);
		const auto cd_size = read_le<std::uint32_t>(eocd + 12);
		const auto cd_offset = read_le<std::uint32_t>(eocd + 16);

		if (disk_number != 0 || cdisk != 0 ||
			entry_count == 0xffffU || cd_size == 0xffffffffU ||
			cd_offset == 0xffffffffU ||
			!range_in_buffer(cd_offset, cd_size, bytes_.size())) {
			throw std::runtime_error(
				"Unsupported zip dictionary: " + path);
		}

		std::uint64_t cursor = cd_offset;
		const std::uint64_t directory_end =
			static_cast<std::uint64_t>(cd_offset) + cd_size;
		entries_.reserve(entry_count);
		index_.reserve(entry_count);

		for (std::uint16_t index = 0; index < entry_count; ++index) {
			if (!range_in_buffer(cursor, 46, bytes_.size()) ||
				cursor >= directory_end) {
				throw std::runtime_error(
					"Corrupt zip central directory: " +
					path);
			}

			const auto *header = bytes_.data() + cursor;
			if (read_le<std::uint32_t>(header) != 0x02014b50U) {
				throw std::runtime_error("Corrupt zip central "
							 "directory entry: " +
					path);
			}

			const auto flags = read_le<std::uint16_t>(header + 8);
			const auto method = read_le<std::uint16_t>(header + 10);
			const auto compressed_size =
				read_le<std::uint32_t>(header + 20);
			const auto uncompressed_size =
				read_le<std::uint32_t>(header + 24);
			const auto name_size =
				read_le<std::uint16_t>(header + 28);
			const auto extra_size =
				read_le<std::uint16_t>(header + 30);
			const auto comment_size =
				read_le<std::uint16_t>(header + 32);
			const auto local_header_offset =
				read_le<std::uint32_t>(header + 42);
			const std::uint64_t entry_size =
				46ULL + name_size + extra_size + comment_size;

			if (compressed_size == 0xffffffffU ||
				uncompressed_size == 0xffffffffU ||
				local_header_offset == 0xffffffffU ||
				!range_in_buffer(
					cursor, entry_size, bytes_.size())) {
				throw std::runtime_error(
					"Unsupported zip64 dictionary entry: " +
					path);
			}

			entries_.push_back(
				{std::string{reinterpret_cast<const char *>(
						     header + 46),
					 name_size},
					flags,
					method,
					compressed_size,
					uncompressed_size,
					local_header_offset});
			index_.emplace(entries_.back().name, entries_.size() - 1);
			cursor += entry_size;
		}
	}

	[[nodiscard]] auto
	extract(const zip_entry &entry) const -> std::string
	{
		if ((entry.flags & 1U) != 0) {
			throw std::runtime_error(
				"Encrypted zip entries are not supported: " +
				entry.name);
		}
		if (!range_in_buffer(
			    entry.local_header_offset, 30, bytes_.size())) {
			throw std::runtime_error(
				"Corrupt zip local header: " + entry.name);
		}

		const auto *local = bytes_.data() + entry.local_header_offset;
		if (read_le<std::uint32_t>(local) != 0x04034b50U) {
			throw std::runtime_error(
				"Corrupt zip local header signature: " +
				entry.name);
		}

		const auto local_name_size = read_le<std::uint16_t>(local + 26);
		const auto local_extra_size = read_le<std::uint16_t>(local + 28);
		const std::uint64_t data_offset =
			static_cast<std::uint64_t>(entry.local_header_offset) +
			30 + local_name_size + local_extra_size;

		if (!range_in_buffer(data_offset,
			    entry.compressed_size,
			    bytes_.size())) {
			throw std::runtime_error(
				"Corrupt zip entry data: " + entry.name);
		}

		const auto *compressed = bytes_.data() + data_offset;
		if (entry.method == 0) {
			if (entry.compressed_size != entry.uncompressed_size) {
				throw std::runtime_error(
					"Corrupt stored zip entry: " +
					entry.name);
			}
			return {reinterpret_cast<const char *>(compressed),
				entry.uncompressed_size};
		}
		if (entry.method != 8) {
			throw std::runtime_error("Unsupported zip compression "
						 "method in entry: " +
				entry.name);
		}

		std::string output;
		output.resize(entry.uncompressed_size);

		auto *decompressor = libdeflate_alloc_decompressor();
		if (decompressor == nullptr) {
			throw std::runtime_error(
				"Failed to allocate libdeflate decompressor");
		}
		std::size_t produced = 0;
		const auto result = libdeflate_deflate_decompress(decompressor,
			compressed,
			entry.compressed_size,
			output.data(),
			output.size(),
			&produced);
		libdeflate_free_decompressor(decompressor);
		if (result != LIBDEFLATE_SUCCESS ||
			produced != output.size()) {
			throw std::runtime_error(
				"Failed to decompress zip entry: " + entry.name);
		}
		return output;
	}

	std::vector<std::uint8_t> bytes_;
	std::vector<zip_entry> entries_;
	std::unordered_map<std::string_view, std::size_t> index_;
};

// ---------------------------------------------------------------------------
// Yomitan frequency extraction via simdjson.
// ---------------------------------------------------------------------------

[[nodiscard]] auto
freq_from_simdjson_value(simdjson::dom::element value,
	std::optional<std::string_view> display) -> std::optional<frequency_value>
{
	if (display) {
		frequency_value compact;
		if (parse_range_display(*display, compact) ||
			parse_decimal1_display(*display, compact)) {
			return compact;
		}
		std::uint64_t integer = 0;
		if (value.get(integer) == simdjson::SUCCESS) {
			std::array<char, 24> buffer{};
			auto [end, ec] = std::to_chars(buffer.data(),
				buffer.data() + buffer.size(),
				integer);
			const std::string_view rendered{buffer.data(),
				static_cast<std::size_t>(end - buffer.data())};
			if (*display == rendered) {
				return frequency_value{
					.kind = frequency_kind::integer,
					.first = integer,
					.second = 0,
					.text = {}};
			}
		}
		return frequency_value{.kind = frequency_kind::text,
			.first = 0,
			.second = 0,
			.text = std::string{*display}};
	}

	std::uint64_t u_int = 0;
	if (value.get(u_int) == simdjson::SUCCESS) {
		return frequency_value{.kind = frequency_kind::integer,
			.first = u_int,
			.second = 0,
			.text = {}};
	}
	std::int64_t s_int = 0;
	if (value.get(s_int) == simdjson::SUCCESS) {
		if (s_int >= 0) {
			return frequency_value{.kind = frequency_kind::integer,
				.first = static_cast<std::uint64_t>(s_int),
				.second = 0,
				.text = {}};
		}
		return frequency_value{.kind = frequency_kind::text,
			.first = 0,
			.second = 0,
			.text = std::to_string(s_int)};
	}
	std::string_view sv;
	if (value.get(sv) == simdjson::SUCCESS) {
		frequency_value compact;
		if (parse_decimal1_display(sv, compact)) {
			return compact;
		}
		return frequency_value{.kind = frequency_kind::text,
			.first = 0,
			.second = 0,
			.text = std::string{sv}};
	}
	double dbl = 0.0;
	if (value.get(dbl) == simdjson::SUCCESS) {
		std::array<char, 32> buffer{};
		auto [end, ec] = std::to_chars(buffer.data(),
			buffer.data() + buffer.size(),
			dbl);
		std::string text{buffer.data(),
			static_cast<std::size_t>(end - buffer.data())};
		if (text.find('.') == std::string::npos &&
			text.find('e') == std::string::npos) {
			text.append(".0");
		}
		frequency_value compact;
		if (parse_decimal1_display(text, compact)) {
			return compact;
		}
		return frequency_value{.kind = frequency_kind::text,
			.first = 0,
			.second = 0,
			.text = std::move(text)};
	}
	return std::nullopt;
}

[[nodiscard]] auto
read_freq_simdjson(simdjson::dom::element entry_meta)
	-> std::optional<frequency_value>
{
	simdjson::dom::object meta;
	if (entry_meta.get(meta) != simdjson::SUCCESS) {
		return std::nullopt;
	}

	auto extract_display = [](simdjson::dom::object &obj)
		-> std::optional<std::string_view> {
		std::string_view text;
		if (obj["displayValue"].get(text) == simdjson::SUCCESS) {
			return text;
		}
		return std::nullopt;
	};

	simdjson::dom::element direct_value;
	if (meta["value"].get(direct_value) == simdjson::SUCCESS) {
		return freq_from_simdjson_value(
			direct_value, extract_display(meta));
	}

	simdjson::dom::element frequency_elem;
	if (meta["frequency"].get(frequency_elem) != simdjson::SUCCESS) {
		return std::nullopt;
	}

	simdjson::dom::object frequency_obj;
	if (frequency_elem.get(frequency_obj) == simdjson::SUCCESS) {
		simdjson::dom::element inner_value;
		if (frequency_obj["value"].get(inner_value) ==
			simdjson::SUCCESS) {
			return freq_from_simdjson_value(
				inner_value, extract_display(frequency_obj));
		}
	}
	return freq_from_simdjson_value(frequency_elem, std::nullopt);
}

// ---------------------------------------------------------------------------
// Build arena (SoA columnar storage).
// ---------------------------------------------------------------------------

struct build_arena {
	std::vector<char> word_arena;
	std::vector<char> text_arena;
	std::vector<std::uint32_t> word_offset;
	std::vector<std::uint32_t> word_size;
	std::vector<std::uint8_t> kind;
	std::vector<std::uint64_t> first;
	std::vector<std::uint64_t> second;
	std::vector<std::uint32_t> text_offset;
	std::vector<std::uint32_t> text_size;

	void
	reserve(std::size_t entries, std::size_t word_bytes, std::size_t text_bytes)
	{
		word_arena.reserve(word_bytes);
		text_arena.reserve(text_bytes);
		word_offset.reserve(entries);
		word_size.reserve(entries);
		kind.reserve(entries);
		first.reserve(entries);
		second.reserve(entries);
		text_offset.reserve(entries);
		text_size.reserve(entries);
	}

	[[nodiscard]] auto
	size() const noexcept -> std::size_t
	{
		return word_offset.size();
	}

	void
	push(std::string_view word, const frequency_value &value)
	{
		word_offset.push_back(
			checked_u32(word_arena.size(), "word arena offset"));
		word_size.push_back(checked_u32(word.size(), "word size"));
		word_arena.insert(word_arena.end(), word.begin(), word.end());

		kind.push_back(static_cast<std::uint8_t>(value.kind));
		first.push_back(value.first);
		second.push_back(value.second);
		text_offset.push_back(
			checked_u32(text_arena.size(), "text arena offset"));
		text_size.push_back(checked_u32(value.text.size(), "text size"));
		text_arena.insert(
			text_arena.end(), value.text.begin(), value.text.end());
	}

	[[nodiscard]] auto
	word(std::uint32_t i) const noexcept -> std::string_view
	{
		return {word_arena.data() + word_offset[i], word_size[i]};
	}

	[[nodiscard]] auto
	text(std::uint32_t i) const noexcept -> std::string_view
	{
		return {text_arena.data() + text_offset[i], text_size[i]};
	}
};

void
append_frequency_indexed(std::vector<char> &out,
	const build_arena &arena,
	std::uint32_t idx)
{
	constexpr std::uint64_t shift_limit =
		std::numeric_limits<std::uint64_t>::max() >> 2U;
	const auto k = static_cast<frequency_kind>(arena.kind[idx]);
	const auto tag = static_cast<std::uint64_t>(k);

	switch (k) {
	case frequency_kind::integer:
	case frequency_kind::decimal1:
	case frequency_kind::range:
		if (arena.first[idx] > shift_limit) {
			throw std::runtime_error(
				"frequency value exceeds cache format limit");
		}
		append_varint(out, (arena.first[idx] << 2U) | tag);
		if (k == frequency_kind::range) {
			append_varint(out, arena.second[idx]);
		}
		return;
	case frequency_kind::text: {
		const auto text = arena.text(idx);
		append_varint(out,
			(static_cast<std::uint64_t>(text.size()) << 2U) | tag);
		out.insert(out.end(), text.begin(), text.end());
		return;
	}
	}
}

// ---------------------------------------------------------------------------
// Parallel stable sort over indices via std::async + inplace_merge.
// ---------------------------------------------------------------------------

template <typename Cmp>
void
parallel_stable_sort(std::vector<std::uint32_t> &order, Cmp cmp)
{
	constexpr std::size_t serial_threshold = 16384;
	const std::size_t hw = std::max<std::size_t>(
		1, std::thread::hardware_concurrency());
	std::size_t chunks = 1;
	while (chunks * 2U <= hw) {
		chunks *= 2U;
	}
	if (order.size() < serial_threshold || chunks == 1) {
		std::stable_sort(order.begin(), order.end(), cmp);
		return;
	}
	chunks = std::min(chunks, order.size());

	std::vector<std::size_t> boundaries(chunks + 1);
	for (std::size_t i = 0; i <= chunks; ++i) {
		boundaries[i] = (order.size() * i) / chunks;
	}

	std::vector<std::future<void>> sorters;
	sorters.reserve(chunks);
	for (std::size_t i = 0; i < chunks; ++i) {
		sorters.push_back(std::async(std::launch::async, [&, i] {
			std::stable_sort(order.begin() + boundaries[i],
				order.begin() + boundaries[i + 1],
				cmp);
		}));
	}
	for (auto &fut : sorters) {
		fut.get();
	}

	while (chunks > 1) {
		std::vector<std::future<void>> mergers;
		mergers.reserve(chunks / 2);
		for (std::size_t i = 0; i + 1 < chunks; i += 2) {
			const std::size_t lo = boundaries[i];
			const std::size_t mid = boundaries[i + 1];
			const std::size_t hi = boundaries[i + 2];
			mergers.push_back(std::async(std::launch::async,
				[&, lo, mid, hi] {
					std::inplace_merge(
						order.begin() + lo,
						order.begin() + mid,
						order.begin() + hi,
						cmp);
				}));
		}
		for (auto &fut : mergers) {
			fut.get();
		}
		std::vector<std::size_t> next_boundaries;
		next_boundaries.reserve(chunks / 2 + 2);
		next_boundaries.push_back(boundaries.front());
		for (std::size_t i = 2; i < boundaries.size(); i += 2) {
			next_boundaries.push_back(boundaries[i]);
		}
		if ((chunks % 2U) != 0U) {
			next_boundaries.push_back(boundaries.back());
		}
		boundaries = std::move(next_boundaries);
		chunks = boundaries.size() - 1;
	}
}

// ---------------------------------------------------------------------------
// Per-dictionary build
// ---------------------------------------------------------------------------

struct dictionary_build {
	std::string name;
	std::string source;
	std::uint64_t source_size = 0;
	std::uint64_t source_mtime = 0;
	std::uint32_t entry_count = 0;
	std::uint32_t max_word_size = 0;
	std::uint32_t block_size = default_block_size;
	std::vector<std::uint32_t> block_offsets;
	std::vector<char> data;
};

[[nodiscard]] auto
build_dictionary_cache(const std::string &dict_path) -> dictionary_build
{
	zip_archive zip{dict_path};

	std::string dictionary_name = dictionary_name_from_path(dict_path);
	simdjson::dom::parser parser;
	{
		const auto index_json = zip.read_text_file("index.json");
		simdjson::dom::element index_doc;
		if (parser.parse(index_json).get(index_doc) == simdjson::SUCCESS) {
			std::string_view title;
			if (index_doc["title"].get(title) == simdjson::SUCCESS) {
				dictionary_name = std::string{title};
			}
		}
	}

	std::vector<std::string_view> bank_names;
	std::uint64_t total_bank_bytes = 0;
	for (const auto &entry : zip.entries()) {
		if (term_meta_bank_number(entry.name) !=
			std::numeric_limits<std::uint32_t>::max()) {
			bank_names.emplace_back(entry.name);
			total_bank_bytes += entry.uncompressed_size;
		}
	}
	if (bank_names.empty()) {
		throw std::runtime_error("Yomitan dictionary has no "
					 "term_meta_bank_*.json files: " +
			dict_path);
	}
	std::ranges::sort(
		bank_names, [](std::string_view lhs, std::string_view rhs) {
			const auto lhs_n = term_meta_bank_number(lhs);
			const auto rhs_n = term_meta_bank_number(rhs);
			return lhs_n != rhs_n ? lhs_n < rhs_n : lhs < rhs;
		});

	const std::size_t estimated_entries =
		std::min<std::size_t>(total_bank_bytes / 64, 1U << 22U);
	build_arena arena;
	arena.reserve(estimated_entries,
		total_bank_bytes / 6,
		total_bank_bytes / 64);

	for (const auto bank_name : bank_names) {
		const auto bank_json = zip.read_text_file(bank_name);
		simdjson::dom::element bank_doc;
		if (parser.parse(bank_json).get(bank_doc) != simdjson::SUCCESS) {
			throw std::runtime_error(
				"Failed to parse Yomitan term metadata bank: " +
				std::string{bank_name});
		}
		simdjson::dom::array bank_array;
		if (bank_doc.get(bank_array) != simdjson::SUCCESS) {
			throw std::runtime_error(
				"Yomitan term metadata bank is not an array: " +
				std::string{bank_name});
		}
		for (simdjson::dom::element entry : bank_array) {
			simdjson::dom::array tuple;
			if (entry.get(tuple) != simdjson::SUCCESS) {
				continue;
			}
			auto it = tuple.begin();
			const auto end = tuple.end();
			if (it == end) {
				continue;
			}
			std::string_view word;
			if ((*it).get(word) != simdjson::SUCCESS) {
				continue;
			}
			++it;
			if (it == end) {
				continue;
			}
			std::string_view tag;
			if ((*it).get(tag) != simdjson::SUCCESS ||
				tag != "freq") {
				continue;
			}
			++it;
			if (it == end) {
				continue;
			}
			auto freq = read_freq_simdjson(*it);
			if (!freq) {
				continue;
			}
			arena.push(word, *freq);
		}
	}

	std::vector<std::uint32_t> order(arena.size());
	std::iota(order.begin(), order.end(), 0U);
	parallel_stable_sort(order,
		[&arena](std::uint32_t a, std::uint32_t b) noexcept {
			return arena.word(a) < arena.word(b);
		});

	dictionary_build dict;
	dict.name = std::move(dictionary_name);
	dict.source = dict_path;
	dict.source_size = std::filesystem::file_size(dict_path);
	dict.source_mtime = source_mtime_stamp(dict_path);
	dict.block_offsets.reserve(
		(order.size() + dict.block_size - 1U) / dict.block_size);
	dict.data.reserve(arena.word_arena.size() + arena.text_arena.size() +
		arena.size() * 2);

	std::string_view previous_in_block;
	std::string_view previous_word;
	std::uint32_t emitted_total = 0;
	std::uint32_t emitted_in_block = 0;

	for (const std::uint32_t idx : order) {
		const auto word = arena.word(idx);
		if (emitted_total != 0 && word == previous_word) {
			continue;
		}

		if (emitted_in_block == 0) {
			dict.block_offsets.push_back(
				checked_u32(dict.data.size(), "block data offset"));
			append_varint(dict.data, word.size());
			dict.data.insert(
				dict.data.end(), word.begin(), word.end());
		} else {
			const auto prefix =
				common_prefix_size(previous_in_block, word);
			append_varint(dict.data, prefix);
			append_varint(dict.data, word.size() - prefix);
			dict.data.insert(dict.data.end(),
				word.begin() +
					static_cast<std::ptrdiff_t>(prefix),
				word.end());
		}
		append_frequency_indexed(dict.data, arena, idx);

		dict.max_word_size = std::max(
			dict.max_word_size, checked_u32(word.size(), "word size"));
		previous_in_block = word;
		previous_word = word;
		++emitted_total;
		emitted_in_block =
			(emitted_in_block + 1U) % dict.block_size;
	}

	dict.entry_count = emitted_total;
	return dict;
}

// ---------------------------------------------------------------------------
// Cache file write
// ---------------------------------------------------------------------------

void
write_descriptor(std::span<char> file,
	std::size_t offset,
	const dictionary_build &dict,
	std::uint64_t name_offset,
	std::uint64_t source_offset,
	std::uint64_t block_offsets_offset,
	std::uint64_t data_offset)
{
	write_le_at<std::uint64_t>(file, offset, name_offset);
	write_le_at<std::uint32_t>(file, offset + 8,
		checked_u32(dict.name.size(), "dictionary name size"));
	write_le_at<std::uint64_t>(file, offset + 12, source_offset);
	write_le_at<std::uint32_t>(file, offset + 20,
		checked_u32(dict.source.size(), "source path size"));
	write_le_at<std::uint64_t>(file, offset + 24, block_offsets_offset);
	write_le_at<std::uint32_t>(file, offset + 32,
		checked_u32(dict.block_offsets.size(), "block count"));
	write_le_at<std::uint64_t>(file, offset + 36, data_offset);
	write_le_at<std::uint64_t>(file, offset + 44,
		checked_u32(dict.data.size(), "dictionary data size"));
	write_le_at<std::uint32_t>(file, offset + 52, dict.entry_count);
	write_le_at<std::uint32_t>(file, offset + 56, dict.max_word_size);
	write_le_at<std::uint32_t>(file, offset + 60, dict.block_size);
	write_le_at<std::uint64_t>(file, offset + 64, dict.source_size);
	write_le_at<std::uint64_t>(file, offset + 72, dict.source_mtime);
}

void
write_cache_file(const std::string &bin_path,
	const std::vector<dictionary_build> &dictionaries)
{
	const auto parent = std::filesystem::path{bin_path}.parent_path();
	if (!parent.empty()) {
		std::filesystem::create_directories(parent);
	}

	std::vector<char> file;
	file.reserve(3 * 1024 * 1024);
	file.insert(file.end(), cache_magic.begin(), cache_magic.end());
	append_le<std::uint32_t>(file, cache_version);
	append_le<std::uint32_t>(file, header_size);
	append_le<std::uint32_t>(
		file, checked_u32(dictionaries.size(), "dictionary count"));
	append_le<std::uint32_t>(file, dictionary_descriptor_size);
	append_le<std::uint64_t>(file, header_size);

	const std::size_t descriptor_table_offset = file.size();
	file.resize(file.size() +
			dictionaries.size() * dictionary_descriptor_size,
		0);

	struct dict_layout {
		std::uint64_t name_offset;
		std::uint64_t source_offset;
		std::uint64_t block_offsets_offset;
		std::uint64_t data_offset;
	};
	std::vector<dict_layout> layouts(dictionaries.size());

	for (std::size_t index = 0; index < dictionaries.size(); ++index) {
		const auto &dict = dictionaries[index];
		layouts[index].name_offset =
			checked_u32(file.size(), "name offset");
		file.insert(file.end(), dict.name.begin(), dict.name.end());

		layouts[index].source_offset =
			checked_u32(file.size(), "source offset");
		file.insert(file.end(), dict.source.begin(), dict.source.end());

		layouts[index].block_offsets_offset =
			checked_u32(file.size(), "block offsets offset");
		for (const auto block_offset : dict.block_offsets) {
			append_le<std::uint32_t>(file, block_offset);
		}

		layouts[index].data_offset =
			checked_u32(file.size(), "data offset");
		file.insert(file.end(), dict.data.begin(), dict.data.end());
	}

	for (std::size_t index = 0; index < dictionaries.size(); ++index) {
		write_descriptor(file,
			descriptor_table_offset +
				index * dictionary_descriptor_size,
			dictionaries[index],
			layouts[index].name_offset,
			layouts[index].source_offset,
			layouts[index].block_offsets_offset,
			layouts[index].data_offset);
	}

	const std::string temp_path = bin_path + ".tmp";
	{
		std::ofstream output(
			temp_path, std::ios::binary | std::ios::trunc);
		if (!output.is_open()) {
			throw std::runtime_error(
				"Failed to open cache for writing: " +
				temp_path);
		}
		output.write(
			file.data(), static_cast<std::streamsize>(file.size()));
		if (!output) {
			throw std::runtime_error(
				"Failed to write cache: " + temp_path);
		}
	}
	std::filesystem::rename(temp_path, bin_path);
}

void
build_cache(const builder_params &params)
{
	std::vector<std::future<dictionary_build>> futures;
	futures.reserve(params.dict_paths.size());
	for (const auto &dict_path : params.dict_paths) {
		futures.push_back(std::async(std::launch::async,
			[&dict_path] {
				return build_dictionary_cache(dict_path);
			}));
	}
	std::vector<dictionary_build> dictionaries;
	dictionaries.reserve(futures.size());
	for (auto &fut : futures) {
		dictionaries.push_back(fut.get());
	}
	write_cache_file(params.path_to_bin, dictionaries);
}

} // namespace

int
main(int argc, char *argv[])
{
	builder_params params;
	try {
		params.assign(argc, argv);
	} catch (const std::invalid_argument &error) {
		write_stderr_line(error.what());
		std::fwrite(help_text.data(), 1, help_text.size(), stderr);
		return 2;
	}

	if (params.show_help) {
		std::fwrite(help_text.data(), 1, help_text.size(), stdout);
		return 0;
	}

	apply_default_paths(params);

	try {
		if (params.dict_paths.empty()) {
			throw std::runtime_error(
				"No dictionaries available (provide --dict-path)");
		}
		if (params.path_to_bin.empty()) {
			throw std::runtime_error(
				"No --bin-path provided and no default available");
		}
		build_cache(params);
		return 0;
	} catch (const std::exception &error) {
		write_stderr_line(error.what());
		return 1;
	}
}
