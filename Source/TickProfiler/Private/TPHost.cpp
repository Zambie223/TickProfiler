#include "TPHost.h"

#include "Async/TaskGraphInterfaces.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"

#if PLATFORM_LINUX
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#endif

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Psapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace TickProfiler
{
	namespace
	{
#if PLATFORM_LINUX
		/** Reads a small pseudo-file (/proc, /sys). UE's file helpers trust the reported size, which is 0 for these. */
		FString ReadSmallFile(const char* Path)
		{
			FILE* File = fopen(Path, "r");
			if (!File)
			{
				return FString();
			}
			char Buffer[8192];
			const size_t Read = fread(Buffer, 1, sizeof(Buffer) - 1, File);
			fclose(File);
			Buffer[Read] = 0;
			return FString(UTF8_TO_TCHAR(Buffer));
		}

		FString ReadSmallFile(const FString& Path)
		{
			return ReadSmallFile(TCHAR_TO_UTF8(*Path));
		}

		/** Value of a "Key value" or "Key: value kB" line, or -1. */
		int64 FindKeyedValue(const FString& Text, const TCHAR* Key)
		{
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				if (Line.StartsWith(Key))
				{
					FString Rest = Line.Mid(FCString::Strlen(Key));
					Rest.RemoveFromStart(TEXT(":"));
					Rest.TrimStartInline();
					return FCString::Atoi64(*Rest);
				}
			}
			return -1;
		}

		/** Directory of this process's cgroup (v2 unified or the v1 cpu controller), resolved once. */
		struct FCgroupPaths
		{
			FString CpuStat;
			FString CpuMax;      // v2 "quota period" or "max period"
			FString CfsQuota;    // v1
			FString CfsPeriod;   // v1
			FString MemCurrent;
			FString MemMax;
			bool bV2 = false;
		};

		const FCgroupPaths& GetCgroupPaths()
		{
			static const FCgroupPaths Paths = []
			{
				FCgroupPaths Result;
				// Inside a container with a cgroup namespace, /sys/fs/cgroup is already the container's own group.
				// Without one, /proc/self/cgroup names the group's path below the mount.
				FString V2Dir = TEXT("/sys/fs/cgroup");
				FString V1CpuDir;
				FString V1MemDir;
				const FString Self = ReadSmallFile("/proc/self/cgroup");
				TArray<FString> Lines;
				Self.ParseIntoArrayLines(Lines);
				for (const FString& Line : Lines)
				{
					TArray<FString> Parts;
					Line.ParseIntoArray(Parts, TEXT(":"), false);
					if (Parts.Num() < 3)
					{
						continue;
					}
					if (Parts[0] == TEXT("0") && Parts[1].IsEmpty() && Parts[2] != TEXT("/"))
					{
						const FString Candidate = V2Dir + Parts[2];
						if (!ReadSmallFile(Candidate + TEXT("/cpu.stat")).IsEmpty())
						{
							V2Dir = Candidate;
						}
					}
					if (Parts[1].Contains(TEXT("cpu,")) || Parts[1] == TEXT("cpu") || Parts[1].Contains(TEXT(",cpu")))
					{
						V1CpuDir = Parts[2];
					}
					if (Parts[1] == TEXT("memory"))
					{
						V1MemDir = Parts[2];
					}
				}
				if (!ReadSmallFile(V2Dir + TEXT("/cpu.max")).IsEmpty() || !ReadSmallFile(V2Dir + TEXT("/cgroup.controllers")).IsEmpty())
				{
					Result.bV2 = true;
					Result.CpuStat = V2Dir + TEXT("/cpu.stat");
					Result.CpuMax = V2Dir + TEXT("/cpu.max");
					Result.MemCurrent = V2Dir + TEXT("/memory.current");
					Result.MemMax = V2Dir + TEXT("/memory.max");
				}
				else
				{
					for (const TCHAR* Mount : {TEXT("/sys/fs/cgroup/cpu,cpuacct"), TEXT("/sys/fs/cgroup/cpu")})
					{
						for (const FString& Dir : {FString(Mount) + V1CpuDir, FString(Mount)})
						{
							if (Result.CpuStat.IsEmpty() && !ReadSmallFile(Dir + TEXT("/cpu.stat")).IsEmpty())
							{
								Result.CpuStat = Dir + TEXT("/cpu.stat");
								Result.CfsQuota = Dir + TEXT("/cpu.cfs_quota_us");
								Result.CfsPeriod = Dir + TEXT("/cpu.cfs_period_us");
							}
						}
					}
					for (const FString& Dir : {FString(TEXT("/sys/fs/cgroup/memory")) + V1MemDir, FString(TEXT("/sys/fs/cgroup/memory"))})
					{
						if (Result.MemCurrent.IsEmpty() && !ReadSmallFile(Dir + TEXT("/memory.usage_in_bytes")).IsEmpty())
						{
							Result.MemCurrent = Dir + TEXT("/memory.usage_in_bytes");
							Result.MemMax = Dir + TEXT("/memory.limit_in_bytes");
						}
					}
				}
				return Result;
			}();
			return Paths;
		}

		/** CPU quota in cores, or 0 when unlimited/unknown. */
		double GetCgroupQuotaCores()
		{
			const FCgroupPaths& Paths = GetCgroupPaths();
			if (Paths.bV2)
			{
				TArray<FString> Parts;
				ReadSmallFile(Paths.CpuMax).TrimStartAndEnd().ParseIntoArrayWS(Parts);
				if (Parts.Num() == 2 && Parts[0] != TEXT("max"))
				{
					const double Period = FCString::Atod(*Parts[1]);
					return Period > 0 ? FCString::Atod(*Parts[0]) / Period : 0.0;
				}
				return 0.0;
			}
			if (Paths.CfsQuota.IsEmpty())
			{
				return 0.0;
			}
			const double Quota = FCString::Atod(*ReadSmallFile(Paths.CfsQuota).TrimStartAndEnd());
			const double Period = FCString::Atod(*ReadSmallFile(Paths.CfsPeriod).TrimStartAndEnd());
			return (Quota > 0 && Period > 0) ? Quota / Period : 0.0;
		}

		/** Memory limit in MB, or 0 when unlimited/unknown (v1 reports a huge number for "unlimited"). */
		double GetCgroupMemLimitMb()
		{
			const FString Text = ReadSmallFile(GetCgroupPaths().MemMax).TrimStartAndEnd();
			if (Text.IsEmpty() || Text == TEXT("max"))
			{
				return 0.0;
			}
			const double Mb = FCString::Atod(*Text) / (1024.0 * 1024.0);
			return Mb > 64.0 * 1024.0 * 1024.0 ? 0.0 : Mb;
		}

		int32 GetAllowedCpuCount()
		{
			cpu_set_t Set;
			CPU_ZERO(&Set);
			return sched_getaffinity(0, sizeof(Set), &Set) == 0 ? CPU_COUNT(&Set) : 0;
		}
#endif

#if PLATFORM_WINDOWS
		uint64 FileTimeToU64(const FILETIME& Time)
		{
			return (static_cast<uint64>(Time.dwHighDateTime) << 32) | Time.dwLowDateTime;
		}

		int32 GetAllowedCpuCount()
		{
			DWORD_PTR ProcessMask = 0;
			DWORD_PTR SystemMask = 0;
			if (!GetProcessAffinityMask(GetCurrentProcess(), &ProcessMask, &SystemMask))
			{
				return 0;
			}
			return FMath::CountBits(static_cast<uint64>(ProcessMask));
		}
#endif
	}

	FHostSampler::FHostSampler() = default;

	FString FHostSampler::Header()
	{
		// proc_cpu_pct: 100 = one full core. sys_* are shares of the whole machine as the OS reports it.
		return TEXT("time_utc,uptime_s,proc_cpu_pct,sys_busy_pct,steal_pct,iowait_pct,cg_throttled_periods,cg_throttled_ms,")
			TEXT("rss_mb,swap_mb,avail_phys_mb,cg_mem_mb,page_faults,load1");
	}

	FString FHostSampler::Sample(const FString& TimeColumns)
	{
		const double Now = FPlatformTime::Seconds();
		const double WallDelta = bPrimed ? FMath::Max(Now - LastWallSeconds, 1e-6) : 0.0;

		double ProcCpuSeconds = 0.0;
		uint64 SysTotal = 0, SysIdle = 0, SysSteal = 0, SysIoWait = 0;
		uint64 ThrottledPeriods = 0, ThrottledUsec = 0, PageFaults = 0;
		double SwapMb = -1.0, CgMemMb = -1.0, Load1 = -1.0;

		const FPlatformMemoryStats Mem = FPlatformMemory::GetStats();
		const double RssMb = Mem.UsedPhysical / (1024.0 * 1024.0);
		const double AvailMb = Mem.AvailablePhysical / (1024.0 * 1024.0);

#if PLATFORM_LINUX
		{
			const FString Stat = ReadSmallFile("/proc/self/stat");
			int32 Paren = INDEX_NONE;
			if (Stat.FindLastChar(TEXT(')'), Paren))
			{
				TArray<FString> Fields;
				Stat.Mid(Paren + 2).ParseIntoArrayWS(Fields);
				// After "pid (comm) ": [0]=state ... [11]=utime [12]=stime, in clock ticks.
				if (Fields.Num() > 12)
				{
					static const double TicksPerSecond = static_cast<double>(sysconf(_SC_CLK_TCK));
					ProcCpuSeconds = (FCString::Atod(*Fields[11]) + FCString::Atod(*Fields[12])) / TicksPerSecond;
				}
			}
		}
		{
			FString ProcStat = ReadSmallFile("/proc/stat");
			FString First;
			ProcStat.Split(TEXT("\n"), &First, nullptr);
			TArray<FString> Fields;
			First.ParseIntoArrayWS(Fields);
			// cpu user nice system idle iowait irq softirq steal [guest guest_nice] (guest time is already in user).
			if (Fields.Num() > 8 && Fields[0] == TEXT("cpu"))
			{
				for (int32 Index = 1; Index <= 8; ++Index)
				{
					SysTotal += FCString::Strtoui64(*Fields[Index], nullptr, 10);
				}
				SysIdle = FCString::Strtoui64(*Fields[4], nullptr, 10);
				SysIoWait = FCString::Strtoui64(*Fields[5], nullptr, 10);
				SysSteal = FCString::Strtoui64(*Fields[8], nullptr, 10);
			}
		}
		{
			const FCgroupPaths& Paths = GetCgroupPaths();
			if (!Paths.CpuStat.IsEmpty())
			{
				const FString CpuStat = ReadSmallFile(Paths.CpuStat);
				ThrottledPeriods = static_cast<uint64>(FMath::Max<int64>(0, FindKeyedValue(CpuStat, TEXT("nr_throttled"))));
				const int64 V2Usec = FindKeyedValue(CpuStat, TEXT("throttled_usec"));
				const int64 V1Nsec = FindKeyedValue(CpuStat, TEXT("throttled_time"));
				ThrottledUsec = V2Usec >= 0 ? V2Usec : (V1Nsec >= 0 ? V1Nsec / 1000 : 0);
			}
			if (!Paths.MemCurrent.IsEmpty())
			{
				const FString Text = ReadSmallFile(Paths.MemCurrent).TrimStartAndEnd();
				if (!Text.IsEmpty())
				{
					CgMemMb = FCString::Atod(*Text) / (1024.0 * 1024.0);
				}
			}
		}
		{
			const int64 SwapKb = FindKeyedValue(ReadSmallFile("/proc/self/status"), TEXT("VmSwap"));
			SwapMb = SwapKb >= 0 ? SwapKb / 1024.0 : -1.0;
			// majflt is field [9] of /proc/self/stat; minor faults are not interesting here.
			const FString Stat = ReadSmallFile("/proc/self/stat");
			int32 Paren = INDEX_NONE;
			if (Stat.FindLastChar(TEXT(')'), Paren))
			{
				TArray<FString> Fields;
				Stat.Mid(Paren + 2).ParseIntoArrayWS(Fields);
				if (Fields.Num() > 9)
				{
					PageFaults = FCString::Strtoui64(*Fields[9], nullptr, 10);
				}
			}
			TArray<FString> LoadFields;
			ReadSmallFile("/proc/loadavg").ParseIntoArrayWS(LoadFields);
			if (LoadFields.Num() > 0)
			{
				Load1 = FCString::Atod(*LoadFields[0]);
			}
		}
#elif PLATFORM_WINDOWS
		{
			FILETIME Creation, Exit, Kernel, User;
			if (GetProcessTimes(GetCurrentProcess(), &Creation, &Exit, &Kernel, &User))
			{
				ProcCpuSeconds = (FileTimeToU64(Kernel) + FileTimeToU64(User)) / 1e7;
			}
			FILETIME Idle, SysKernel, SysUser;
			if (GetSystemTimes(&Idle, &SysKernel, &SysUser))
			{
				// Kernel time includes idle time.
				SysTotal = FileTimeToU64(SysKernel) + FileTimeToU64(SysUser);
				SysIdle = FileTimeToU64(Idle);
			}
			PROCESS_MEMORY_COUNTERS Counters{};
			if (K32GetProcessMemoryInfo(GetCurrentProcess(), &Counters, sizeof(Counters)))
			{
				// On Windows this counts soft faults too; a sudden jump still points at memory pressure.
				PageFaults = Counters.PageFaultCount;
				SwapMb = Counters.PagefileUsage / (1024.0 * 1024.0);
			}
		}
#endif

		FString Row;
		if (bPrimed)
		{
			const double ProcPct = 100.0 * (ProcCpuSeconds - LastProcCpuSeconds) / WallDelta;
			const double DTotal = static_cast<double>(SysTotal - LastSysTotal);
			const auto Pct = [DTotal](const uint64 Delta) { return DTotal > 0 ? 100.0 * Delta / DTotal : 0.0; };
			const double BusyPct = DTotal > 0 ? 100.0 - Pct(SysIdle - LastSysIdle) - Pct(SysIoWait - LastSysIoWait) : 0.0;
			Row = FString::Printf(TEXT("%s,%.1f,%.1f,%.2f,%.2f,%llu,%.1f,%.0f,%.0f,%.0f,%.0f,%llu,%.2f"),
				*TimeColumns,
				ProcPct,
				BusyPct,
				Pct(SysSteal - LastSysSteal),
				Pct(SysIoWait - LastSysIoWait),
				ThrottledPeriods - LastThrottledPeriods,
				(ThrottledUsec - LastThrottledUsec) / 1000.0,
				RssMb,
				SwapMb,
				AvailMb,
				CgMemMb,
				PageFaults - LastPageFaults,
				Load1);
		}

		LastWallSeconds = Now;
		LastProcCpuSeconds = ProcCpuSeconds;
		LastSysTotal = SysTotal;
		LastSysIdle = SysIdle;
		LastSysSteal = SysSteal;
		LastSysIoWait = SysIoWait;
		LastThrottledPeriods = ThrottledPeriods;
		LastThrottledUsec = ThrottledUsec;
		LastPageFaults = PageFaults;
		bPrimed = true;
		return Row;
	}

	void FHostSampler::DescribeStatic(FJsonObject& Out)
	{
		Out.SetStringField(TEXT("os"), FPlatformMisc::GetOSVersion());
		Out.SetStringField(TEXT("cpu_brand"), FPlatformMisc::GetCPUBrand().TrimStartAndEnd());
		Out.SetNumberField(TEXT("cores_physical"), FPlatformMisc::NumberOfCores());
		Out.SetNumberField(TEXT("cores_logical"), FPlatformMisc::NumberOfCoresIncludingHyperthreads());
		Out.SetNumberField(TEXT("cpus_allowed_by_affinity"), GetAllowedCpuCount());
		Out.SetNumberField(TEXT("engine_worker_threads_to_spawn"), FPlatformMisc::NumberOfWorkerThreadsToSpawn());
		if (FTaskGraphInterface::IsRunning())
		{
			Out.SetNumberField(TEXT("taskgraph_worker_threads"), FTaskGraphInterface::Get().GetNumWorkerThreads());
		}
		Out.SetNumberField(TEXT("total_physical_mb"), FPlatformMemory::GetConstants().TotalPhysical / (1024.0 * 1024.0));
#if PLATFORM_LINUX
		const FCgroupPaths& Paths = GetCgroupPaths();
		Out.SetStringField(TEXT("cgroup_version"), Paths.bV2 ? TEXT("v2") : (Paths.CpuStat.IsEmpty() ? TEXT("none") : TEXT("v1")));
		Out.SetNumberField(TEXT("cgroup_cpu_quota_cores"), GetCgroupQuotaCores());
		Out.SetNumberField(TEXT("cgroup_mem_limit_mb"), GetCgroupMemLimitMb());
#endif
	}
}
