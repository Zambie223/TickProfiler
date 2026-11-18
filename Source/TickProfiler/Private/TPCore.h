// Core recording primitives shared by the hooks and the collector.
//
// Two kinds of data are recorded:
//  * Phases: coarse engine/game stages (a tick group, the net flush, GC, ...). Low call counts, recorded into
//    relaxed atomics so any thread may add to them. Only the outermost call of a phase on a thread is timed, so
//    a phase that re-enters itself is not counted twice.
//  * Objects: per-class (and at level 3 per-object) attribution of actor ticks, component ticks and actor
//    replication. High call counts and often on worker threads, so each thread writes to its own slot, guarded by a
//    per-slot spin lock that is only ever contended while the game thread merges the slot.
#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectKey.h"
#include <atomic>

DECLARE_LOG_CATEGORY_EXTERN(LogTickProfiler, Log, All);

namespace TickProfiler
{
	enum class EPhase : uint8
	{
		WorldTick,
		TG_PrePhysics,
		TG_StartPhysics,
		TG_DuringPhysics,
		TG_EndPhysics,
		TG_PostPhysics,
		TG_PostUpdateWork,
		TG_LastDemotable,
		TimerManager,
		TickableObjects,
		LatentActions,
		LevelStreaming,
		EndOfFrameUpdates,
		NetTickDispatch,
		NetTickFlush,
		NetReplicateActors,
		FactoryTick,
		GarbageCollect,
		SaveWorld,
		/** The engine's core ticker (FTSTicker): HTTP and the server API, online services, other periodic engine work. Runs outside the world tick. */
		CoreTicker,
		ProfilerSelf,
		Count
	};
	constexpr int32 NumPhases = static_cast<int32>(EPhase::Count);
	const TCHAR* GetPhaseName(EPhase Phase);
	/** Short name used for CSV column headers. */
	const TCHAR* GetPhaseColumn(EPhase Phase);

	enum class EObjKind : uint8
	{
		ActorTick,
		ComponentTick,
		Replication,
		Count
	};
	constexpr int32 NumObjKinds = static_cast<int32>(EObjKind::Count);
	const TCHAR* GetKindName(EObjKind Kind);

	/** 0 = paused, 1 = basic (phases only), 2 = detailed (+ per class), 3 = extreme (+ per object). */
	extern std::atomic<int32> GLevel;
	FORCEINLINE int32 GetLevel() { return GLevel.load(std::memory_order_relaxed); }

	void AddPhaseTime(EPhase Phase, uint64 Cycles);
	/** Drains the phase accumulators (game thread, once per frame). */
	void DrainPhases(double OutMs[NumPhases], uint32 OutCalls[NumPhases]);

	/** Times the outermost call of a phase on the current thread. */
	struct FPhaseScope
	{
		explicit FPhaseScope(EPhase InPhase);
		~FPhaseScope();

	private:
		uint64 Start = 0;
		EPhase Phase;
		bool bActive = false;
		bool bCounted = false;
	};

	struct FAcc
	{
		uint64 Cycles = 0;
		uint64 MaxCycles = 0;
		/** Kind-specific extra: bits written for replication, 0 otherwise. */
		uint64 Extra = 0;
		uint32 Calls = 0;

		FORCEINLINE void Add(uint64 InCycles, uint64 InExtra)
		{
			Cycles += InCycles;
			MaxCycles = FMath::Max(MaxCycles, InCycles);
			Extra += InExtra;
			++Calls;
		}
		FORCEINLINE void Merge(const FAcc& Other)
		{
			Cycles += Other.Cycles;
			MaxCycles = FMath::Max(MaxCycles, Other.MaxCycles);
			Extra += Other.Extra;
			Calls += Other.Calls;
		}
	};

	struct FClassKey
	{
		const UClass* Class = nullptr;
		EObjKind Kind = EObjKind::ActorTick;

		bool operator==(const FClassKey& Other) const { return Class == Other.Class && Kind == Other.Kind; }
		friend uint32 GetTypeHash(const FClassKey& Key) { return HashCombine(PointerHash(Key.Class), static_cast<uint32>(Key.Kind)); }
	};

	struct FObjKey
	{
		FObjectKey Object;
		EObjKind Kind = EObjKind::ActorTick;

		bool operator==(const FObjKey& Other) const { return Object == Other.Object && Kind == Other.Kind; }
		friend uint32 GetTypeHash(const FObjKey& Key) { return HashCombine(GetTypeHash(Key.Object), static_cast<uint32>(Key.Kind)); }
	};

	/** What one thread recorded since the last merge, split by whether it ran on the game thread. */
	struct FThreadSlot
	{
		std::atomic<bool> bLocked{false};
		bool bGameThread = false;
		TMap<FClassKey, FAcc> Classes;
		TMap<FObjKey, FAcc> Objects;

		FORCEINLINE void Lock()
		{
			while (bLocked.exchange(true, std::memory_order_acquire))
			{
				FPlatformProcess::YieldThread();
			}
		}
		FORCEINLINE void Unlock() { bLocked.store(false, std::memory_order_release); }
	};

	/** Records one actor tick, component tick or actor replication. Object must be alive for the call. */
	void RecordObject(EObjKind Kind, const UObject* Object, uint64 Cycles, uint64 Extra = 0);

	/** Visits every thread slot under its lock (game thread). */
	void ForEachSlot(TFunctionRef<void(FThreadSlot& Slot)> Visitor);

	/** Number of RecordObject calls since startup (for the self-overhead estimate). */
	uint64 GetTotalObjectRecords();

	FORCEINLINE double CyclesToMs(uint64 Cycles) { return FPlatformTime::ToMilliseconds64(Cycles); }
}
