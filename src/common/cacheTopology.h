#ifndef KYTY_COMMON_CACHE_TOPOLOGY_H_
#define KYTY_COMMON_CACHE_TOPOLOGY_H_

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace Common {

// A CPU list of the Linux sysfs ("0-7,16-23"), expanded; empty when malformed.
[[nodiscard]] std::vector<int> ParseCpuList(std::string_view text);
// A cache size of the Linux sysfs ("98304K"), in bytes; 0 when malformed.
[[nodiscard]] uint64_t ParseCacheSize(std::string_view text);
// The logical CPUs that share the largest last-level cache, read from a Linux sysfs CPU tree
// (/sys/devices/system/cpu): on a Ryzen X3D, those of the CCD with the 3D V-Cache. Between caches
// of the same size, the one of the lowest CPU. Empty when the tree describes no cache.
[[nodiscard]] std::vector<int> LargestCacheCpus(const std::filesystem::path& cpu_root);

} // namespace Common

#endif /* KYTY_COMMON_CACHE_TOPOLOGY_H_ */
