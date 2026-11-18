#include "FileTimeTestUtil.h"
#include "common/assert.h"
#include "common/cacheTopology.h"
#include "common/config.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/hostException.h"
#include "common/logging/log.h"
#include "common/platform/sysDbg.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/virtualMemory.h"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace VirtualMemory = Common::VirtualMemory;

constexpr uint64_t PAGE = 0x10000;
constexpr uint64_t BASE = 0x0000'0600'0000'0000ull;

TEST(VirtualMemory, AllocFixedRefusesMappedPagesAndFreeReleasesOnlyItsRange) {
	ASSERT_TRUE(VirtualMemory::AllocFixed(BASE, PAGE, VirtualMemory::Mode::ReadWrite));
	ASSERT_TRUE(VirtualMemory::AllocFixed(BASE + PAGE, PAGE, VirtualMemory::Mode::ReadWrite));

	EXPECT_FALSE(VirtualMemory::AllocFixed(BASE, PAGE, VirtualMemory::Mode::ReadWrite));
	EXPECT_FALSE(VirtualMemory::AllocFixed(BASE - PAGE, 2 * PAGE, VirtualMemory::Mode::ReadWrite));

	ASSERT_TRUE(VirtualMemory::Free(BASE, PAGE));
	EXPECT_FALSE(VirtualMemory::AllocFixed(BASE + PAGE, PAGE, VirtualMemory::Mode::ReadWrite));
	ASSERT_TRUE(VirtualMemory::AllocFixed(BASE, PAGE, VirtualMemory::Mode::ReadWrite));

	EXPECT_TRUE(VirtualMemory::Free(BASE, PAGE));
	EXPECT_TRUE(VirtualMemory::Free(BASE + PAGE, PAGE));
}

volatile uint64_t g_fault_rsp = 0;

// Resolves the write fault on BASE by making the page writable.
bool MakeBaseWritable(const Common::HostException::ExceptionInfo& info) {
	g_fault_rsp = info.rsp;
	return info.type == Common::HostException::ExceptionType::AccessViolation &&
	       info.access_violation_type == Common::HostException::AccessViolationType::Write &&
	       info.access_violation_vaddr == BASE &&
	       VirtualMemory::Protect(BASE, PAGE, VirtualMemory::Mode::ReadWrite);
}

bool RefuseFault(const Common::HostException::ExceptionInfo& /*info*/) {
	return false;
}

TEST(HostException, TheFirstHandlerStaysInstalledAndResolvesFaults) {
	EXPECT_EXIT(
	    {
		    namespace HostException = Common::HostException;
		    const bool installs     = HostException::InstallHandler(MakeBaseWritable) &&
			                          HostException::InstallHandler(MakeBaseWritable) &&
			                          !HostException::InstallHandler(RefuseFault);
		    const bool mapped = VirtualMemory::AllocFixed(BASE, PAGE, VirtualMemory::Mode::Read);
		    auto*      value  = reinterpret_cast<volatile uint32_t*>(BASE);
		    *value            = 7;
		    // The faulting frame's stack pointer lies just below this local.
		    const auto local  = reinterpret_cast<uint64_t>(&value);
		    const bool rsp_ok = g_fault_rsp <= local && local - g_fault_rsp < 0x10000;
		    std::_Exit(installs && mapped && *value == 7 && rsp_ok ? 0 : 1);
	    },
	    testing::ExitedWithCode(0), "");
}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
volatile sig_atomic_t g_dispatched = 0;

// Refuses the fault if the guest signal it raises was delivered before the handler returned.
bool RaiseDispatchDuringFault(const Common::HostException::ExceptionInfo& info) {
	std::raise(Common::HostException::GuestSignalDispatchSignal());
	return g_dispatched == 0 && info.access_violation_vaddr == BASE &&
	       VirtualMemory::Protect(BASE, PAGE, VirtualMemory::Mode::ReadWrite);
}

TEST(HostException, GuestSignalWaitsUntilTheFaultHandlerReturns) {
	EXPECT_EXIT(
	    {
		    std::signal(Common::HostException::GuestSignalDispatchSignal(),
			            [](int) { g_dispatched = 1; });
		    const bool installs = Common::HostException::InstallHandler(RaiseDispatchDuringFault);
		    const bool mapped   = VirtualMemory::AllocFixed(BASE, PAGE, VirtualMemory::Mode::Read);
		    *reinterpret_cast<volatile uint32_t*>(BASE) = 1;
		    std::_Exit(installs && mapped && g_dispatched == 1 ? 0 : 1);
	    },
	    testing::ExitedWithCode(0), "");
}
#endif

TEST(SysDbg, StackUsageReportsTheCallingThreadStackBase) {
	std::thread([] {
		const auto s = SysStackUsage();
		EXPECT_NE(s.reserved_addr, 0u);
		EXPECT_LT(s.reserved_addr, reinterpret_cast<uintptr_t>(&s));
	}).join();
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
constexpr int FATAL_EXIT_STATUS = 321;
#else
// POSIX keeps the low 8 bits of the exit status.
constexpr int FATAL_EXIT_STATUS = 321 & 0xff;
#endif

TEST(Assert, ExitMacrosEvaluateOnceAndExitWithTheFatalStatus) {
	int evaluations = 0;
	EXIT_IF(++evaluations == 0);
	EXIT_NOT_IMPLEMENTED(++evaluations == 0);
	EXPECT_EQ(evaluations, 2);

	EXPECT_EXIT(EXIT_IF(evaluations == 2), testing::ExitedWithCode(FATAL_EXIT_STATUS), "");
	EXPECT_EXIT(EXIT_NOT_IMPLEMENTED(true), testing::ExitedWithCode(FATAL_EXIT_STATUS), "");
	EXPECT_EXIT(EXIT("%d\n", evaluations), testing::ExitedWithCode(FATAL_EXIT_STATUS), "");
}

TEST(File, MissingFileStaysInvalidAndCreatedFileRoundTrips) {
	const auto path = std::filesystem::temp_directory_path() / "kyty_common_file_test.bin";
	std::filesystem::remove(path);

	Common::File missing;
	EXPECT_FALSE(missing.Open(path, Common::File::Mode::Read));
	EXPECT_TRUE(missing.IsInvalid());

	const uint32_t value = 0x12345678u;
	{
		Common::File out;
		ASSERT_TRUE(out.Create(path));
		out.Write(&value, sizeof(value));
		EXPECT_EQ(out.Size(), sizeof(value));
	}

	Common::File in;
	ASSERT_TRUE(in.Open(path, Common::File::Mode::Read));
	uint32_t read_value = 0;
	in.Read(&read_value, sizeof(read_value));
	EXPECT_EQ(read_value, value);
	EXPECT_TRUE(in.Seek(2));
	EXPECT_EQ(in.Tell(), 2u);
	in.Close();
	std::filesystem::remove(path);
}

TEST(File, NativeExistenceQueriesFollowTheEntryType) {
	const auto dir  = std::filesystem::temp_directory_path();
	const auto file = dir / "kyty_common_exists_test.bin";
	{
		Common::File out;
		ASSERT_TRUE(out.Create(file));
		const uint8_t byte = 1;
		out.Write(&byte, sizeof(byte));
	}

	EXPECT_TRUE(Common::File::IsDirectoryExisting(dir));
	EXPECT_TRUE(Common::File::IsDirectoryExisting(dir / ""));
	EXPECT_FALSE(Common::File::IsFileExisting(dir));
	EXPECT_EQ(Common::File::Size(dir), 0u);

	EXPECT_TRUE(Common::File::IsFileExisting(file));
	EXPECT_FALSE(Common::File::IsFileExisting(file / ""));
	EXPECT_FALSE(Common::File::IsDirectoryExisting(file));
	EXPECT_EQ(Common::File::Size(file), 1u);

	std::filesystem::remove(file);
	EXPECT_FALSE(Common::File::IsFileExisting(file));
}

TEST(File, GetTimesReportsExactNanoseconds) {
	const auto path = std::filesystem::temp_directory_path() / "kyty_common_time_test.bin";
	ASSERT_TRUE(Common::File().Create(path));
	// A multiple of 100 ns, the Windows file time unit.
	constexpr int64_t WRITE_NS = 1700000000123456700;
	SetUnixWriteTime(path, WRITE_NS);

	const auto times = Common::File::GetTimes(path);
	ASSERT_TRUE(times.has_value());
	EXPECT_EQ(times->write_ns, WRITE_NS);
	EXPECT_GT(times->access_ns, WRITE_NS);

	Common::File in(path, Common::File::Mode::Read);
	const auto   open_times = in.GetTimes();
	ASSERT_TRUE(open_times.has_value());
	EXPECT_EQ(open_times->write_ns, WRITE_NS);
	in.Close();
	std::filesystem::remove(path);
}

std::string ReadAll(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), {}};
}

TEST(Log, FileGetsPlainTextAndTheConsoleGetsEachLineOnce) {
	EXPECT_EXIT(
	    {
		    const auto dir =
		        std::filesystem::temp_directory_path() /
		        ("kyty_log_test_" +
				 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		    std::filesystem::create_directories(dir);
		    const auto console_file = dir / "console.txt";
		    std::freopen(console_file.string().c_str(), "wb", stdout);
		    Log::Write({}, "p");
		    const auto log_file = dir / "log.txt";
		    std::ofstream(log_file) << "previous run";
		    Config::Initialize();
		    Config::ConfigOptions options;
		    options.printf_direction   = Config::LogDirection::File;
		    options.printf_output_file = log_file;
		    Config::Load(options);
		    Log::Initialize();
		    Log::Write(Log::Color::Red, "a");
		    Log::WriteToConsoleAndLog("b");
		    Log::Flush();
		    const bool file_ok = ReadAll(log_file) == "ab";

		    options.printf_direction = Config::LogDirection::Console;
		    Config::Load(options);
		    Log::Initialize();
		    Log::Write(Log::Color::Red, "c");
		    Log::WriteToConsoleAndLog("d");
		    const bool console_ok = ReadAll(console_file) == "pb\x1b[31mc\x1b[0md";

		    options.printf_direction = Config::LogDirection::Silent;
		    Config::Load(options);
		    Log::Initialize();
		    const bool      silent_ok = Log::IsSilent();
		    std::error_code ec;
		    std::filesystem::remove_all(dir, ec);
		    std::_Exit(file_ok && console_ok && silent_ok ? 0 : 1);
	    },
	    testing::ExitedWithCode(0), "");
}

TEST(StringUtils, PathRoundTripsUtf8) {
	constexpr std::string_view Utf8 = "dir/\xc3\xa9-\xe6\x97\xa5.bin";
	EXPECT_EQ(Common::PathToString(Common::PathFromUtf8(Utf8)), Utf8);
	EXPECT_EQ(Common::PathToGenericString(Common::PathFromUtf8(Utf8)), Utf8);
}

TEST(Thread, MutexIsRecursiveAndCondVarTimesOutOrWakes) {
	Common::Mutex   mutex;
	Common::CondVar cond_var;
	mutex.Lock();
	mutex.Lock();
	mutex.Unlock();
	EXPECT_FALSE(cond_var.WaitFor(&mutex, 1000));

	static int polls = 0;
	polls            = 0;
	Common::CondVar::SetWaitPollCallback([] { polls++; });
	cond_var.Wait(&mutex);
	Common::CondVar::SetWaitPollCallback(nullptr);
	EXPECT_EQ(polls, 1);

	bool        ready = false;
	std::thread signaller([&] {
		Common::LockGuard lock(mutex);
		ready = true;
		cond_var.Signal();
	});
	while (!ready) {
		cond_var.Wait(&mutex);
	}
	mutex.Unlock();
	signaller.join();
}

TEST(Thread, SleepsAtLeastTheRequestedTimeAndIdsAreUniquePerThread) {
	Common::InitializeThreads();
	EXPECT_TRUE(Common::Thread::IsMainThread());

	const auto start = std::chrono::steady_clock::now();
	Common::Thread::SleepMicro(2000);
	EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::microseconds(2000));

	const int id = Common::Thread::GetThreadIdUnique();
	EXPECT_EQ(Common::Thread::GetThreadIdUnique(), id);
	int  other_id   = id;
	bool other_main = true;
	std::thread([&] {
		other_id   = Common::Thread::GetThreadIdUnique();
		other_main = Common::Thread::IsMainThread();
	}).join();
	EXPECT_NE(other_id, id);
	EXPECT_FALSE(other_main);
}

TEST(CacheTopology, ParsesSysfsCpuListsAndCacheSizes) {
	EXPECT_EQ(Common::ParseCpuList("0-3,16-17\n"), (std::vector<int> {0, 1, 2, 3, 16, 17}));
	EXPECT_EQ(Common::ParseCpuList("5"), (std::vector<int> {5}));
	EXPECT_TRUE(Common::ParseCpuList("").empty());
	EXPECT_TRUE(Common::ParseCpuList("4-2").empty());
	EXPECT_TRUE(Common::ParseCpuList("0-x").empty());
	EXPECT_TRUE(Common::ParseCpuList("-1").empty());

	EXPECT_EQ(Common::ParseCacheSize("98304K\n"), uint64_t {96} << 20u);
	EXPECT_EQ(Common::ParseCacheSize("32M"), uint64_t {32} << 20u);
	EXPECT_EQ(Common::ParseCacheSize("512"), 512u);
	EXPECT_EQ(Common::ParseCacheSize("K"), 0u);
	EXPECT_EQ(Common::ParseCacheSize("8X"), 0u);
}

// A fake /sys/devices/system/cpu of a Ryzen 9 with two CCDs: CPUs 0-7 and their SMT siblings
// 16-23 share one L3, CPUs 8-15 and 24-31 the other.
std::filesystem::path WriteTwoCcdCpuTree(std::string_view first_l3, std::string_view second_l3) {
	const auto      root = std::filesystem::temp_directory_path() / "kyty_cache_topology_test";
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	const auto write = [](const std::filesystem::path& path, std::string_view text) {
		std::filesystem::create_directories(path.parent_path());
		std::ofstream(path) << text << '\n';
	};
	for (int cpu = 0; cpu < 32; cpu++) {
		const auto cache  = root / ("cpu" + std::to_string(cpu)) / "cache";
		const auto core   = cpu % 16;
		const bool first  = core < 8;
		const auto shared = std::to_string(core) + "," + std::to_string(core + 16);
		write(cache / "index0" / "level", "1");
		write(cache / "index0" / "size", "32K");
		write(cache / "index0" / "shared_cpu_list", shared);
		write(cache / "index2" / "level", "2");
		write(cache / "index2" / "size", "1024K");
		write(cache / "index2" / "shared_cpu_list", shared);
		write(cache / "index3" / "level", "3");
		write(cache / "index3" / "size", first ? first_l3 : second_l3);
		write(cache / "index3" / "shared_cpu_list", first ? "0-7,16-23" : "8-15,24-31");
	}
	// Entries that are not CPUs.
	write(root / "cpufreq" / "boost", "1");
	write(root / "cpuidle" / "current_driver", "acpi_idle");
	write(root / "online", "0-31");
	return root;
}

TEST(CacheTopology, LargestCacheCpusAreThoseOfTheLargestLastLevelCache) {
	const std::vector<int> first_ccd {0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23};
	const std::vector<int> second_ccd {8,  9,  10, 11, 12, 13, 14, 15,
	                                   24, 25, 26, 27, 28, 29, 30, 31};

	// Ryzen 9 7950X3D: the 3D V-Cache (96 MiB) is on the first CCD.
	auto root = WriteTwoCcdCpuTree("98304K", "32768K");
	EXPECT_EQ(Common::LargestCacheCpus(root), first_ccd);

	// The 3D V-Cache on the other CCD.
	root = WriteTwoCcdCpuTree("32768K", "98304K");
	EXPECT_EQ(Common::LargestCacheCpus(root), second_ccd);

	// Ryzen 9 7950X: two equal L3s, the one of CPU 0.
	root = WriteTwoCcdCpuTree("32768K", "32768K");
	EXPECT_EQ(Common::LargestCacheCpus(root), first_ccd);

	// No cache information (some virtual machines): nothing to keep to.
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	EXPECT_TRUE(Common::LargestCacheCpus(root).empty());
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
