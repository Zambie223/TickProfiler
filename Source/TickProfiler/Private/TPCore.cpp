#include "TPCore.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY(LogTickProfiler);

namespace TickProfiler
{
	std::atomic<int32> GLevel{2};

	namespace
	{
		std::atomic<uint64> GPhaseCycles[NumPhases];
		std::atomic<uint32> GPhaseCalls[NumPhases];
		thread_local uint8 GPhaseDepth[NumPhases] = {};

		FCriticalSection GSlotsLock;
		TArray<FThreadSlot*> GSlots;
		thread_local FThreadSlot* GThisSlot = nullptr;
		std::atomic<uint64> GTotalRecords{0};

		FThreadSlot& GetThisSlot()
		{
			if (!GThisSlot)
			{
				// Slots live for the whole process: engine and task-graph threads are long-lived, and a slot is small.
				FThreadSlot* Slot = new FThreadSlot();
				Slot->bGameThread = IsInGameThread();
				{
					FScopeLock Guard(&GSlotsLock);
					GSlots.Add(Slot);
				}
				GThisSlot = Slot;
			}
			return *GThisSlot;
		}
	}

	const TCHAR* GetPhaseName(const EPhase Phase)
	{
		switch (Phase)
		{
		case EPhase::WorldTick: return TEXT("World tick (total)");
		case EPhase::TG_PrePhysics: return TEXT("Tick group PrePhysics");
		case EPhase::TG_StartPhysics: return TEXT("Tick group StartPhysics");
		case EPhase::TG_DuringPhysics: return TEXT("Tick group DuringPhysics");
		case EPhase::TG_EndPhysics: return TEXT("Tick group EndPhysics");
		case EPhase::TG_PostPhysics: return TEXT("Tick group PostPhysics");
		case EPhase::TG_PostUpdateWork: return TEXT("Tick group PostUpdateWork");
		case EPhase::TG_LastDemotable: return TEXT("Tick group LastDemotable");
		case EPhase::TimerManager: return TEXT("Timers");
		case EPhase::TickableObjects: return TEXT("Tickable objects");
		case EPhase::LatentActions: return TEXT("Latent actions");
		case EPhase::LevelStreaming: return TEXT("Level streaming");
		case EPhase::EndOfFrameUpdates: return TEXT("End-of-frame updates");
		case EPhase::NetTickDispatch: return TEXT("Net receive (TickDispatch)");
		case EPhase::NetTickFlush: return TEXT("Net send (TickFlush)");
		case EPhase::NetReplicateActors: return TEXT("Net replicate actors");
		case EPhase::FactoryTick: return TEXT("Factory tick");
		case EPhase::GarbageCollect: return TEXT("Garbage collection");
		case EPhase::SaveWorld: return TEXT("Save game");
		case EPhase::CoreTicker: return TEXT("Core ticker (HTTP, server API, online)");
		case EPhase::ProfilerSelf: return TEXT("TickProfiler itself");
		default: return TEXT("?");
		}
	}

	const TCHAR* GetPhaseColumn(const EPhase Phase)
	{
		switch (Phase)
		{
		case EPhase::WorldTick: return TEXT("world");
		case EPhase::TG_PrePhysics: return TEXT("tg_prephys");
		case EPhase::TG_StartPhysics: return TEXT("tg_startphys");
		case EPhase::TG_DuringPhysics: return TEXT("tg_duringphys");
		case EPhase::TG_EndPhysics: return TEXT("tg_endphys");
		case EPhase::TG_PostPhysics: return TEXT("tg_postphys");
		case EPhase::TG_PostUpdateWork: return TEXT("tg_postupdate");
		case EPhase::TG_LastDemotable: return TEXT("tg_lastdemotable");
		case EPhase::TimerManager: return TEXT("timers");
		case EPhase::TickableObjects: return TEXT("tickables");
		case EPhase::LatentActions: return TEXT("latent");
		case EPhase::LevelStreaming: return TEXT("streaming");
		case EPhase::EndOfFrameUpdates: return TEXT("eof_updates");
		case EPhase::NetTickDispatch: return TEXT("net_recv");
		case EPhase::NetTickFlush: return TEXT("net_send");
		case EPhase::NetReplicateActors: return TEXT("net_replicate");
		case EPhase::FactoryTick: return TEXT("factory");
		case EPhase::GarbageCollect: return TEXT("gc");
		case EPhase::SaveWorld: return TEXT("save");
		case EPhase::CoreTicker: return TEXT("core_ticker");
		case EPhase::ProfilerSelf: return TEXT("profiler");
		default: return TEXT("unknown");
		}
	}

	const TCHAR* GetKindName(const EObjKind Kind)
	{
		switch (Kind)
		{
		case EObjKind::ActorTick: return TEXT("actor_tick");
		case EObjKind::ComponentTick: return TEXT("component_tick");
		case EObjKind::Replication: return TEXT("replication");
		default: return TEXT("?");
		}
	}

	void AddPhaseTime(const EPhase Phase, const uint64 Cycles)
	{
		const int32 Index = static_cast<int32>(Phase);
		GPhaseCycles[Index].fetch_add(Cycles, std::memory_order_relaxed);
		GPhaseCalls[Index].fetch_add(1, std::memory_order_relaxed);
	}

	void DrainPhases(double OutMs[NumPhases], uint32 OutCalls[NumPhases])
	{
		for (int32 Index = 0; Index < NumPhases; ++Index)
		{
			OutMs[Index] = CyclesToMs(GPhaseCycles[Index].exchange(0, std::memory_order_relaxed));
			OutCalls[Index] = GPhaseCalls[Index].exchange(0, std::memory_order_relaxed);
		}
	}

	FPhaseScope::FPhaseScope(const EPhase InPhase)
		: Phase(InPhase)
	{
		if (GetLevel() <= 0)
		{
			return;
		}
		bCounted = true;
		if (GPhaseDepth[static_cast<int32>(Phase)]++ == 0)
		{
			bActive = true;
			Start = FPlatformTime::Cycles64();
		}
	}

	FPhaseScope::~FPhaseScope()
	{
		if (bActive)
		{
			AddPhaseTime(Phase, FPlatformTime::Cycles64() - Start);
		}
		// Only undo our own increment, so a level change in the middle of a call cannot unbalance the depth.
		if (bCounted)
		{
			--GPhaseDepth[static_cast<int32>(Phase)];
		}
	}

	void RecordObject(const EObjKind Kind, const UObject* Object, const uint64 Cycles, const uint64 Extra)
	{
		const int32 Level = GetLevel();
		if (Level < 2 || !Object)
		{
			return;
		}
		FThreadSlot& Slot = GetThisSlot();
		const UClass* Class = Object->GetClass();
		Slot.Lock();
		Slot.Classes.FindOrAdd(FClassKey{Class, Kind}).Add(Cycles, Extra);
		if (Level >= 3)
		{
			Slot.Objects.FindOrAdd(FObjKey{FObjectKey(Object), Kind}).Add(Cycles, Extra);
		}
		Slot.Unlock();
		GTotalRecords.fetch_add(1, std::memory_order_relaxed);
	}

	void ForEachSlot(const TFunctionRef<void(FThreadSlot& Slot)> Visitor)
	{
		TArray<FThreadSlot*> Slots;
		{
			FScopeLock Guard(&GSlotsLock);
			Slots = GSlots;
		}
		for (FThreadSlot* Slot : Slots)
		{
			Slot->Lock();
			Visitor(*Slot);
			Slot->Unlock();
		}
	}

	uint64 GetTotalObjectRecords()
	{
		return GTotalRecords.load(std::memory_order_relaxed);
	}
}
