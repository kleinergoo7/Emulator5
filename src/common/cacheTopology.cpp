#include "common/cacheTopology.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace Common {

static std::string_view TrimLineEnd(std::string_view text) {
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
		text.remove_suffix(1);
	}
	return text;
}

std::vector<int> ParseCpuList(std::string_view text) {
	// Far above any CPU_SETSIZE: a larger number is not a CPU.
	constexpr long MaxCpu = 65535;
	text                  = TrimLineEnd(text);
	std::vector<int> cpus;
	while (!text.empty()) {
		const auto        comma = text.find(',');
		const auto        range = text.substr(0, comma);
		const auto        dash  = range.find('-');
		const std::string first(range.substr(0, dash));
		const std::string last(dash == std::string_view::npos ? range : range.substr(dash + 1));
		char*             first_end = nullptr;
		char*             last_end  = nullptr;
		const long        low       = std::strtol(first.c_str(), &first_end, 10);
		const long        high      = std::strtol(last.c_str(), &last_end, 10);
		if (first.empty() || last.empty() || *first_end != '\0' || *last_end != '\0' || low < 0 ||
		    high < low || high > MaxCpu) {
			return {};
		}
		for (long cpu = low; cpu <= high; cpu++) {
			cpus.push_back(static_cast<int>(cpu));
		}
		if (comma == std::string_view::npos) {
			break;
		}
		text.remove_prefix(comma + 1);
	}
	return cpus;
}

uint64_t ParseCacheSize(std::string_view text) {
	const std::string value(TrimLineEnd(text));
	char*             end  = nullptr;
	const auto        size = std::strtoull(value.c_str(), &end, 10);
	if (end == value.c_str()) {
		return 0;
	}
	switch (*end) {
		case '\0': return size;
		case 'K': return size << 10u;
		case 'M': return size << 20u;
		case 'G': return size << 30u;
		default: return 0;
	}
}

std::vector<int> LargestCacheCpus(const std::filesystem::path& cpu_root) {
	struct Cache {
		uint64_t         size = 0;
		std::vector<int> cpus;
	};
	const auto read_line = [](const std::filesystem::path& path) {
		std::ifstream file(path);
		std::string   line;
		std::getline(file, line);
		return line;
	};
	std::vector<Cache> caches;
	std::error_code    error;
	for (std::filesystem::directory_iterator entry(cpu_root, error), end; !error && entry != end;
	     entry.increment(error)) {
		const auto name = entry->path().filename().string();
		if (name.size() < 4 || name.compare(0, 3, "cpu") != 0 ||
		    !std::all_of(name.begin() + 3, name.end(),
		                 [](char c) { return c >= '0' && c <= '9'; })) {
			continue;
		}
		// The last level of this CPU: the cache/indexN with the highest level.
		int                   last_level = 0;
		std::filesystem::path last;
		for (int index = 0; index < 16; index++) {
			const auto dir   = entry->path() / "cache" / ("index" + std::to_string(index));
			const int  level = std::atoi(read_line(dir / "level").c_str());
			if (level > last_level) {
				last_level = level;
				last       = dir;
			}
		}
		if (last_level == 0) {
			continue;
		}
		Cache cache {ParseCacheSize(read_line(last / "size")),
		             ParseCpuList(read_line(last / "shared_cpu_list"))};
		if (cache.size == 0 || cache.cpus.empty()) {
			continue;
		}
		// Every CPU of a cache lists the same CPUs: keep each cache once.
		std::ranges::sort(cache.cpus);
		if (std::ranges::none_of(caches,
		                         [&](const Cache& other) { return other.cpus == cache.cpus; })) {
			caches.push_back(std::move(cache));
		}
	}
	const Cache* largest = nullptr;
	for (const auto& cache: caches) {
		if (largest == nullptr || cache.size > largest->size ||
		    (cache.size == largest->size && cache.cpus.front() < largest->cpus.front())) {
			largest = &cache;
		}
	}
	return largest != nullptr ? largest->cpus : std::vector<int> {};
}

} // namespace Common
