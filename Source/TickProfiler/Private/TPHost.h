// Host-level metrics: tells "the game has too much work" apart from "the machine or container is starving the
// process". Sampled on the writer thread (never the game thread), once per second.
#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace TickProfiler
{
	class FHostSampler
	{
	public:
		FHostSampler();

		/** CSV header matching Sample(). */
		static FString Header();
		/** One CSV row of rates since the previous call (the first call only primes the counters). */
		FString Sample(const FString& TimeColumns);
		/** Facts that do not change while running: cores, CPU quota, total memory, OS. */
		static void DescribeStatic(FJsonObject& Out);

	private:
		double LastWallSeconds = 0.0;
		double LastProcCpuSeconds = 0.0;
		// System-wide CPU counters (jiffies on Linux, 100 ns units on Windows).
		uint64 LastSysTotal = 0;
		uint64 LastSysIdle = 0;
		uint64 LastSysSteal = 0;
		uint64 LastSysIoWait = 0;
		// cgroup CPU throttling.
		uint64 LastThrottledPeriods = 0;
		uint64 LastThrottledUsec = 0;
		uint64 LastPageFaults = 0;
		bool bPrimed = false;
	};
}
