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

#include <array>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace gdf;

namespace
{

constexpr std::string_view help_text = R"EOF(usage: gd-frequency [OPTIONS]

Find Japanese word frequency for GoldenDict.

OPTIONS
   --word WORD                    search term
   --dict-path /path/to/dict.zip  path to a Yomitan frequency dictionary zip
                                  can be provided more than once
   --bin-path /path/to/bin        path to compact cache data
   --build-cache                  build or rebuild the cache and exit
   -h, --help                     show this help

EXAMPLES
   gd-frequency --dict-path ./JPDB.zip --word 歌う
   gd-frequency --dict-path ./JPDB.zip --dict-path ./VN.zip --word 鹿
   gd-frequency --build-cache --dict-path ./JPDB.zip --bin-path ./dict.bin
)EOF";

constexpr std::string_view css_style = R"EOF(
<style>
.gd_frequency {
   font-size: 1rem;
   line-height: 1.2;
   text-decoration: none !important;
   width: 100%;
}

.gd_frequency * {
   text-decoration: none !important;
}

.gd_frequency_results {
   display: flex;
   flex-wrap: wrap;
   gap: 0.35rem;
   width: 100%;
}

.gd_frequency_badge {
   display: inline-flex;
   align-items: baseline;
   gap: 0.35rem;
   box-sizing: border-box;
   max-width: 100%;
   padding: 0.18rem 0.45rem;
   border: 1px solid rgba(0, 0, 0, 0.25);
   border-radius: 0.35rem;
   background: rgba(0, 0, 0, 0.04);
   color: inherit;
}

.gd_frequency_value {
   font-weight: 600;
   white-space: nowrap;
   text-decoration: none;
}

.gd_frequency_source {
   overflow: hidden;
   text-overflow: ellipsis;
   white-space: nowrap;
   opacity: 0.75;
   font-size: 0.8em;
   text-decoration: none;
}

.gd_frequency_source::after {
   content: ":";
   opacity: 0.75;
}

.gd_frequency_missing {
   opacity: 0.45;
   text-decoration: none;
}
</style>
)EOF";

void
write_stdout(std::string_view text) noexcept
{
	std::fwrite(text.data(), 1, text.size(), stdout);
}

void
write_stderr_line(std::string_view text) noexcept
{
	std::fwrite(text.data(), 1, text.size(), stderr);
	std::fputc('\n', stderr);
}

struct decoded_frequency {
	frequency_kind kind = frequency_kind::integer;
	std::uint64_t first = 0;
	std::uint64_t second = 0;
	std::string_view text;
};

[[nodiscard]] auto
read_frequency(const std::uint8_t *&cursor,
	const std::uint8_t *const end,
	decoded_frequency &frequency) noexcept -> bool
{
	std::uint64_t encoded = 0;
	if (!read_varint(cursor, end, encoded)) {
		return false;
	}
	const auto kind = static_cast<frequency_kind>(encoded & 3U);
	frequency = {.kind = kind,
		.first = encoded >> 2U,
		.second = 0,
		.text = {}};

	switch (kind) {
	case frequency_kind::integer:
	case frequency_kind::decimal1:
		return true;
	case frequency_kind::range:
		return read_varint(cursor, end, frequency.second);
	case frequency_kind::text: {
		const std::uint64_t text_size = frequency.first;
		frequency.first = 0;
		if (text_size > static_cast<std::uint64_t>(end - cursor)) {
			return false;
		}
		frequency.text = {reinterpret_cast<const char *>(cursor),
			static_cast<std::size_t>(text_size)};
		cursor += text_size;
		return true;
	}
	}
	return false;
}

class mapped_file
{
      public:
	mapped_file() = default;

	explicit mapped_file(const std::string &path)
	{
		open(path);
	}

	mapped_file(const mapped_file &) = delete;
	auto operator=(const mapped_file &) -> mapped_file & = delete;

	mapped_file(mapped_file &&other) noexcept
		: fd_{std::exchange(other.fd_, -1)},
		  data_{std::exchange(other.data_, nullptr)},
		  size_{std::exchange(other.size_, 0)}
	{
	}

	auto
	operator=(mapped_file &&other) noexcept -> mapped_file &
	{
		if (this != &other) {
			close();
			fd_ = std::exchange(other.fd_, -1);
			data_ = std::exchange(other.data_, nullptr);
			size_ = std::exchange(other.size_, 0);
		}
		return *this;
	}

	~mapped_file()
	{
		close();
	}

	void
	open(const std::string &path)
	{
		close();
		fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (fd_ < 0) {
			throw std::runtime_error("Failed to open cache: " +
				path + ": " + std::strerror(errno));
		}
		struct stat stat_buffer{};
		if (::fstat(fd_, &stat_buffer) != 0) {
			throw std::runtime_error("Failed to stat cache: " +
				path + ": " + std::strerror(errno));
		}
		if (stat_buffer.st_size <= 0) {
			throw std::runtime_error("Cache is empty: " + path);
		}
		size_ = static_cast<std::size_t>(stat_buffer.st_size);
		void *mapped =
			::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
		if (mapped == MAP_FAILED) {
			throw std::runtime_error("Failed to map cache: " +
				path + ": " + std::strerror(errno));
		}
		data_ = static_cast<const std::uint8_t *>(mapped);
		::posix_madvise(mapped, size_, POSIX_MADV_RANDOM);
	}

	[[nodiscard]] auto
	bytes() const noexcept -> std::span<const std::uint8_t>
	{
		return {data_, size_};
	}

      private:
	void
	close() noexcept
	{
		if (data_ != nullptr) {
			::munmap(const_cast<std::uint8_t *>(data_), size_);
		}
		if (fd_ >= 0) {
			::close(fd_);
		}
		fd_ = -1;
		data_ = nullptr;
		size_ = 0;
	}

	int fd_ = -1;
	const std::uint8_t *data_ = nullptr;
	std::size_t size_ = 0;
};

struct dictionary_view {
	std::string_view name;
	std::string_view source;
	std::uint64_t source_size = 0;
	std::uint64_t source_mtime = 0;
	std::span<const std::uint8_t> block_offsets;
	std::span<const std::uint8_t> data;
	std::uint32_t entry_count = 0;
	std::uint32_t max_word_size = 0;
	std::uint32_t block_size = 0;
};

struct lookup_result {
	std::string_view dictionary_name;
	std::optional<decoded_frequency> frequency;
};

class frequency_cache
{
      public:
	explicit frequency_cache(const std::string &path) : mapping_(path)
	{
		parse();
	}

	[[nodiscard]] auto
	matches_sources(const std::vector<std::string> &sources) const -> bool
	{
		if (sources.size() != dictionaries_.size()) {
			return false;
		}
		for (std::size_t index = 0; index < sources.size(); ++index) {
			const auto &cached = dictionaries_[index];
			std::error_code error;
			if (cached.source != sources[index] ||
				!std::filesystem::exists(sources[index], error) ||
				cached.source_size !=
					std::filesystem::file_size(
						sources[index], error) ||
				cached.source_mtime !=
					source_mtime_stamp(sources[index])) {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] auto
	lookup(std::string_view word) const -> std::vector<lookup_result>
	{
		std::vector<lookup_result> results;
		results.reserve(dictionaries_.size());
		for (const auto &dictionary : dictionaries_) {
			results.push_back({dictionary.name,
				lookup_dictionary(dictionary, word)});
		}
		return results;
	}

      private:
	void
	parse()
	{
		const auto data = mapping_.bytes();
		if (data.size() < header_size ||
			std::memcmp(data.data(),
				cache_magic.data(),
				cache_magic.size()) != 0) {
			throw std::runtime_error("Unsupported cache format");
		}

		const auto version = read_le<std::uint32_t>(data.data() + 8);
		const auto actual_header_size =
			read_le<std::uint32_t>(data.data() + 12);
		const auto dict_count =
			read_le<std::uint32_t>(data.data() + 16);
		const auto descriptor_size =
			read_le<std::uint32_t>(data.data() + 20);
		const auto descriptor_table_offset =
			read_le<std::uint64_t>(data.data() + 24);

		if (version != cache_version ||
			actual_header_size < header_size ||
			descriptor_size < dictionary_descriptor_size ||
			!range_in_buffer(descriptor_table_offset,
				static_cast<std::uint64_t>(dict_count) *
					descriptor_size,
				data.size())) {
			throw std::runtime_error(
				"Unsupported or corrupt cache header");
		}

		dictionaries_.reserve(dict_count);
		for (std::uint32_t index = 0; index < dict_count; ++index) {
			const auto *desc = data.data() +
				descriptor_table_offset +
				static_cast<std::uint64_t>(index) *
					descriptor_size;

			const auto name_offset = read_le<std::uint64_t>(desc);
			const auto name_size = read_le<std::uint32_t>(desc + 8);
			const auto source_offset =
				read_le<std::uint64_t>(desc + 12);
			const auto source_path_size =
				read_le<std::uint32_t>(desc + 20);
			const auto block_offsets_offset =
				read_le<std::uint64_t>(desc + 24);
			const auto block_count =
				read_le<std::uint32_t>(desc + 32);
			const auto data_offset =
				read_le<std::uint64_t>(desc + 36);
			const auto data_size =
				read_le<std::uint64_t>(desc + 44);
			const auto entry_count =
				read_le<std::uint32_t>(desc + 52);
			const auto max_word_size =
				read_le<std::uint32_t>(desc + 56);
			const auto block_size =
				read_le<std::uint32_t>(desc + 60);
			const auto source_file_size =
				read_le<std::uint64_t>(desc + 64);
			const auto source_mtime =
				read_le<std::uint64_t>(desc + 72);

			const auto block_offsets_size =
				static_cast<std::uint64_t>(block_count) *
				sizeof(std::uint32_t);
			if (block_size == 0 ||
				!range_in_buffer(
					name_offset, name_size, data.size()) ||
				!range_in_buffer(source_offset,
					source_path_size,
					data.size()) ||
				!range_in_buffer(block_offsets_offset,
					block_offsets_size,
					data.size()) ||
				!range_in_buffer(
					data_offset, data_size, data.size())) {
				throw std::runtime_error(
					"Corrupt cache dictionary descriptor");
			}

			dictionaries_.push_back({.name = std::string_view{
							 reinterpret_cast<const char *>(
								 data.data() +
								 name_offset),
							 name_size},
				.source = std::string_view{
					reinterpret_cast<const char *>(
						data.data() + source_offset),
					source_path_size},
				.source_size = source_file_size,
				.source_mtime = source_mtime,
				.block_offsets = data.subspan(
					block_offsets_offset, block_offsets_size),
				.data = data.subspan(data_offset, data_size),
				.entry_count = entry_count,
				.max_word_size = max_word_size,
				.block_size = block_size});
		}
	}

	[[nodiscard]] static auto
	block_offset(const dictionary_view &dict,
		std::uint32_t block_index) noexcept -> std::uint32_t
	{
		return read_le<std::uint32_t>(dict.block_offsets.data() +
			static_cast<std::size_t>(block_index) *
				sizeof(std::uint32_t));
	}

	[[nodiscard]] static auto
	first_word_in_block(const dictionary_view &dict,
		std::uint32_t block_index) -> std::string_view
	{
		const auto offset = block_offset(dict, block_index);
		if (offset >= dict.data.size()) {
			throw std::runtime_error("Corrupt cache block offset");
		}
		const auto *cursor = dict.data.data() + offset;
		const auto *const end = dict.data.data() + dict.data.size();
		std::uint64_t word_size = 0;
		if (!read_varint(cursor, end, word_size) ||
			word_size > static_cast<std::uint64_t>(end - cursor)) {
			throw std::runtime_error("Corrupt cache block word");
		}
		return {reinterpret_cast<const char *>(cursor),
			static_cast<std::size_t>(word_size)};
	}

	[[nodiscard]] static auto
	lookup_dictionary(const dictionary_view &dict, std::string_view word)
		-> std::optional<decoded_frequency>
	{
		const std::uint32_t block_count = checked_u32(
			dict.block_offsets.size() / sizeof(std::uint32_t),
			"block count");
		if (dict.entry_count == 0 || block_count == 0) {
			return std::nullopt;
		}

		std::uint32_t low = 0;
		std::uint32_t high = block_count;
		while (low < high) {
			const std::uint32_t mid = low + (high - low) / 2U;
			if (first_word_in_block(dict, mid).compare(word) <= 0) {
				low = mid + 1U;
			} else {
				high = mid;
			}
		}
		if (low == 0) {
			return std::nullopt;
		}

		const std::uint32_t block_index = low - 1U;
		const std::uint32_t first_entry_index =
			block_index * dict.block_size;
		const std::uint32_t remaining_entries =
			dict.entry_count - first_entry_index;
		const std::uint32_t entries_in_block =
			std::min(dict.block_size, remaining_entries);
		const std::uint32_t start_offset =
			block_offset(dict, block_index);
		const std::uint32_t end_offset = block_index + 1U < block_count
			? block_offset(dict, block_index + 1U)
			: checked_u32(dict.data.size(), "dictionary data size");

		if (start_offset > end_offset ||
			end_offset > dict.data.size()) {
			throw std::runtime_error("Corrupt cache block range");
		}

		const auto *cursor = dict.data.data() + start_offset;
		const auto *const end = dict.data.data() + end_offset;
		std::string current;
		current.reserve(dict.max_word_size);

		for (std::uint32_t index = 0; index < entries_in_block;
			++index) {
			std::uint64_t prefix_size = 0;
			std::uint64_t suffix_size = 0;
			if (index == 0) {
				if (!read_varint(cursor, end, suffix_size)) {
					throw std::runtime_error(
						"Corrupt cache word");
				}
			} else {
				if (!read_varint(cursor, end, prefix_size) ||
					!read_varint(
						cursor, end, suffix_size) ||
					prefix_size > current.size()) {
					throw std::runtime_error(
						"Corrupt cache compressed "
						"word");
				}
			}
			if (suffix_size >
				static_cast<std::uint64_t>(end - cursor)) {
				throw std::runtime_error(
					"Corrupt cache word suffix");
			}
			current.resize(static_cast<std::size_t>(prefix_size));
			current.append(reinterpret_cast<const char *>(cursor),
				static_cast<std::size_t>(suffix_size));
			cursor += suffix_size;

			decoded_frequency frequency;
			if (!read_frequency(cursor, end, frequency)) {
				throw std::runtime_error(
					"Corrupt cache frequency");
			}

			const int comparison =
				std::string_view{current}.compare(word);
			if (comparison == 0) {
				return frequency;
			}
			if (comparison > 0) {
				return std::nullopt;
			}
		}
		return std::nullopt;
	}

	mapped_file mapping_;
	std::vector<dictionary_view> dictionaries_;
};

// ---------------------------------------------------------------------------
// HTML rendering (single buffered).
// ---------------------------------------------------------------------------

void
append_html_escaped(std::string &out, std::string_view text)
{
	out.reserve(out.size() + text.size());
	for (const char ch : text) {
		switch (ch) {
		case '&':
			out.append("&amp;");
			break;
		case '<':
			out.append("&lt;");
			break;
		case '>':
			out.append("&gt;");
			break;
		case '"':
			out.append("&quot;");
			break;
		case '\'':
			out.append("&#39;");
			break;
		default:
			out.push_back(ch);
			break;
		}
	}
}

[[nodiscard]] auto
contains_ascii_case_insensitive(std::string_view text,
	std::string_view needle) noexcept -> bool
{
	if (needle.empty() || needle.size() > text.size()) {
		return false;
	}
	for (std::size_t i = 0; i <= text.size() - needle.size(); ++i) {
		bool matched = true;
		for (std::size_t j = 0; j < needle.size(); ++j) {
			const auto left = static_cast<unsigned char>(
				text[i + j]);
			const auto right = static_cast<unsigned char>(
				needle[j]);
			if (std::tolower(left) != std::tolower(right)) {
				matched = false;
				break;
			}
		}
		if (matched) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] auto
frequency_hue(std::string_view dictionary_name,
	const decoded_frequency &frequency) noexcept -> std::uint32_t
{
	const bool score_scale =
		contains_ascii_case_insensitive(dictionary_name, "vn");
	if (score_scale || frequency.kind == frequency_kind::decimal1) {
		const auto score_tenths =
			frequency.kind == frequency_kind::decimal1
				? frequency.first
				: frequency.first * 10U;
		const auto clamped =
			std::min<std::uint64_t>(score_tenths, 1000U);
		return static_cast<std::uint32_t>((clamped * 120U) / 1000U);
	}

	const auto rank = frequency.kind == frequency_kind::range
		? (frequency.first + frequency.second) / 2U
		: frequency.first;
	const auto clamped =
		std::min<std::uint64_t>(std::max<std::uint64_t>(rank, 1U),
			20000U);
	return static_cast<std::uint32_t>(
		120U - (((clamped - 1U) * 120U) / 19999U));
}

void
append_frequency_badge_style(std::string &out,
	std::string_view dictionary_name,
	const std::optional<decoded_frequency> &frequency)
{
	if (!frequency ||
		frequency->kind == frequency_kind::text) {
		out.append(
			R"( style="border-color: hsl(0, 0%, 78%); background: hsl(0, 0%, 96%); color: hsl(0, 0%, 28%);")");
		return;
	}

	const auto hue = frequency_hue(dictionary_name, *frequency);
	out.append(R"( style="border-color: hsl()");
	out.append(std::to_string(hue));
	out.append(R"(, 72%, 58%); background: hsl()");
	out.append(std::to_string(hue));
	out.append(R"(, 72%, 94%); color: hsl()");
	out.append(std::to_string(hue));
	out.append(R"(, 72%, 20%);")");
}

void
append_frequency_value(std::string &out, const decoded_frequency &frequency)
{
	if (frequency.kind == frequency_kind::text) {
		append_html_escaped(out, frequency.text);
		return;
	}
	std::array<char, 24> buffer{};
	switch (frequency.kind) {
	case frequency_kind::integer: {
		auto [end, ec] = std::to_chars(buffer.data(),
			buffer.data() + buffer.size(),
			frequency.first);
		out.append(buffer.data(), end);
		return;
	}
	case frequency_kind::decimal1: {
		auto [whole_end, ec] = std::to_chars(buffer.data(),
			buffer.data() + buffer.size() - 2,
			frequency.first / 10U);
		*whole_end++ = '.';
		*whole_end++ =
			static_cast<char>('0' + (frequency.first % 10U));
		out.append(buffer.data(), whole_end);
		return;
	}
	case frequency_kind::range: {
		auto [first_end, ec] = std::to_chars(buffer.data(),
			buffer.data() + buffer.size(),
			frequency.first);
		out.append(buffer.data(), first_end);
		out.push_back('-');
		auto [second_end, ec2] = std::to_chars(buffer.data(),
			buffer.data() + buffer.size(),
			frequency.second);
		out.append(buffer.data(), second_end);
		return;
	}
	case frequency_kind::text:
		break;
	}
}

[[nodiscard]] auto
render_results(const std::vector<lookup_result> &results) -> std::string
{
	std::string out;
	out.reserve(1024 + css_style.size());
	out.append(R"(<div class="gd_frequency">)");

	if (!results.empty()) {
		out.append(
			R"(<div class="gd_frequency_results">)");
		for (const auto &result : results) {
			out.append(
				R"(<span class="gd_frequency_badge")");
			append_frequency_badge_style(
				out, result.dictionary_name, result.frequency);
			out.append(
				R"(><span class="gd_frequency_source">)");
			append_html_escaped(out, result.dictionary_name);
			out.append("</span>");
			if (result.frequency) {
				out.append(
					R"(<span class="gd_frequency_value">)");
				append_frequency_value(out, *result.frequency);
				out.append("</span>");
			} else {
				out.append(
					R"(<span class="gd_frequency_missing">-</span>)");
			}
			out.append("</span>");
		}
		out.append("</div>");
	}
	out.append("</div>");
	out.append(css_style);
	return out;
}

// ---------------------------------------------------------------------------
// CLI parameters
// ---------------------------------------------------------------------------

struct frequency_params {
	std::vector<std::string> dict_paths;
	std::string path_to_bin;
	std::string word;
	bool build_cache_only = false;
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
				build_cache_only = true;
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
			} else if (key == "--word") {
				word = require_value();
			} else {
				throw std::invalid_argument(
					"Unknown argument: " + std::string{key});
			}
		}
	}
};

void
apply_default_paths(frequency_params &params)
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
// Builder dispatch (fork + exec the sibling binary).
// ---------------------------------------------------------------------------

[[nodiscard]] auto
locate_builder() -> std::filesystem::path
{
	std::error_code error;
	const auto self =
		std::filesystem::read_symlink("/proc/self/exe", error);
	if (!error) {
		const auto sibling =
			self.parent_path() / builder_binary_name;
		if (std::filesystem::exists(sibling, error)) {
			return sibling;
		}
	}
	return std::filesystem::path{builder_binary_name};
}

[[nodiscard]] auto
run_builder(const frequency_params &params) -> int
{
	const auto builder_path = locate_builder();

	std::vector<std::string> arg_storage;
	arg_storage.reserve(4U + params.dict_paths.size() * 2U);
	arg_storage.emplace_back(builder_path.string());
	arg_storage.emplace_back("--build-cache");
	arg_storage.emplace_back("--bin-path");
	arg_storage.push_back(params.path_to_bin);
	for (const auto &dict : params.dict_paths) {
		arg_storage.emplace_back("--dict-path");
		arg_storage.push_back(dict);
	}

	std::vector<char *> argv_buffer;
	argv_buffer.reserve(arg_storage.size() + 1U);
	for (auto &s : arg_storage) {
		argv_buffer.push_back(s.data());
	}
	argv_buffer.push_back(nullptr);

	const pid_t pid = ::fork();
	if (pid < 0) {
		throw std::runtime_error(std::string{"fork failed: "} +
			std::strerror(errno));
	}
	if (pid == 0) {
		::execv(builder_path.c_str(), argv_buffer.data());
		// If execv returns we couldn't run the builder.
		std::fprintf(stderr,
			"failed to exec %s: %s\n",
			builder_path.c_str(),
			std::strerror(errno));
		_exit(127);
	}
	int status = 0;
	if (::waitpid(pid, &status, 0) < 0) {
		throw std::runtime_error(std::string{"waitpid failed: "} +
			std::strerror(errno));
	}
	if (!WIFEXITED(status)) {
		throw std::runtime_error("cache builder terminated abnormally");
	}
	return WEXITSTATUS(status);
}

[[nodiscard]] auto
open_or_build_cache(const frequency_params &params) -> frequency_cache
{
	try {
		frequency_cache cache{params.path_to_bin};
		if (cache.matches_sources(params.dict_paths)) {
			return cache;
		}
	} catch (const std::exception &) {
	}
	const int rc = run_builder(params);
	if (rc != 0) {
		throw std::runtime_error(
			"cache builder exited with status " +
			std::to_string(rc));
	}
	return frequency_cache{params.path_to_bin};
}

} // namespace

int
main(int argc, char *argv[])
{
	frequency_params params;
	try {
		params.assign(argc, argv);
	} catch (const std::invalid_argument &error) {
		write_stderr_line(error.what());
		write_stdout(help_text);
		return 2;
	}

	if (params.show_help) {
		write_stdout(help_text);
		return 0;
	}

	apply_default_paths(params);

	try {
		if (params.build_cache_only) {
			return run_builder(params);
		}
		if (params.word.empty()) {
			write_stderr_line("Missing required argument: --word");
			write_stdout(help_text);
			return 2;
		}
		if (params.dict_paths.empty()) {
			throw std::runtime_error(
				"No dictionaries available (provide --dict-path "
				"or install dictionaries)");
		}

		const auto cache = open_or_build_cache(params);
		const auto html =
			render_results(cache.lookup(params.word));
		write_stdout(html);
		return 0;
	} catch (const std::exception &error) {
		write_stderr_line(error.what());
		return 1;
	}
}
