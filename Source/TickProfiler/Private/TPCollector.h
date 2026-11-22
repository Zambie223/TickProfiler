// Turns the raw per-frame recordings into frames, hitches, per-class tables, log summaries and data-file rows.
// Everything here runs on the game thread.
#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "TPCore.h"
#include "TPSettings.h"

class UWorld;

namespace TickProfiler
{
	struct FSettings
	{
		/** 0 paused, 1 basic, 2 detailed (default), 3 extreme. */
		int32 Level = 2;
		/** Frames whose work exceeds this are hitches. 0 = automatic: 2x the server's target frame time, at least HitchMinMs, or 2x the previous second's average work if higher (see GetHitchThresholdMs). */
		double HitchMs = 0.0;
		double HitchMinMs = 50.0;
		double SummaryIntervalSec = 60.0;
		double ClassIntervalSec = 10.0;
		double SnapshotIntervalSec = 300.0;
		int32 TopN = 25;
		int64 DiskCapMB = 256;
		bool bWriteFiles = true;
		bool bSampleHost = true;
		/** At most this many hitch lines per minute in FactoryGame.log (all of them still go to hitches.jsonl). */
		int32 MaxHitchLogLinesPerMinute = 20;
	};

	struct FFrameRecord
	{
		uint64 FrameNumber = 0;
		double EndSeconds = 0.0;
		double PeriodMs = 0.0;
		double WorkMs = 0.0;
		double IdleMs = 0.0;
		double PhaseMs[NumPhases] = {};
		uint32 PhaseCalls[NumPhases] = {};
		double KindGameThreadMs[NumObjKinds] = {};
		double KindWorkerMs[NumObjKinds] = {};
		uint32 KindCalls[NumObjKinds] = {};
		uint64 ReplicationBits = 0;
		uint32 GarbageCollections = 0;
		double GarbageCollectMs = 0.0;
		/** Frame work inside neither the world tick (UWorld::Tick) nor the core ticker: engine-loop work no hook times. */
		double UntimedMs = 0.0;
	};

	struct FClassStat
	{
		/** Copied from the interned class when the entry is created. */
		FString Name;
		FString Origin;
		uint64 GameThreadCycles = 0;
		uint64 WorkerCycles = 0;
		uint64 MaxCallCycles = 0;
		uint64 Extra = 0;
		uint64 Calls = 0;
		/** Largest total of this class within one frame. */
		uint64 MaxFrameCycles = 0;

		uint64 TotalCycles() const { return GameThreadCycles + WorkerCycles; }
	};

	struct FObjectStat : FClassStat
	{
		FString ClassName;
	};

	/** A class as the collector keeps it: interned by path name, so a Blueprint class that is reloaded with a world
	 *  (a new UClass with the same path) stays one entry, and no UClass pointer outlives a garbage collection. */
	struct FStatKey
	{
		int32 ClassId = INDEX_NONE;
		EObjKind Kind = EObjKind::ActorTick;

		bool operator==(const FStatKey& Other) const { return ClassId == Other.ClassId && Kind == Other.Kind; }
		friend uint32 GetTypeHash(const FStatKey& Key) { return HashCombine(::GetTypeHash(Key.ClassId), static_cast<uint32>(Key.Kind)); }
	};

	class FCollector
	{
	public:
		static FCollector& Get();

		void Startup();
		void Shutdown();

		const FSettings& GetSettings() const { return Settings; }
		/**
		 * Takes every setting from Wanted except those fixed by a launch parameter, and except the restart-only ones
		 * (disk cap, data files) once the data files have started. Changed values are logged and marked in the data.
		 */
		void ApplySettings(const FSettings& Wanted, const FString& Source);
		bool IsLaunchOverride(ESetting Setting) const { return bLaunchOverride[static_cast<int32>(Setting)]; }
		void ClearLaunchOverride(ESetting Setting) { bLaunchOverride[static_cast<int32>(Setting)] = false; }
		bool IsSessionStarted() const { return bSessionStarted; }

		/** A level for a limited time (not saved); afterwards the configured level applies again. */
		void SetTemporaryLevel(int32 Level, double Minutes, const FString& Source);
		void CancelTemporaryLevel(const FString& Source);
		bool GetTemporaryLevel(int32& OutLevel, double& OutRemainingSec) const;

		void Mark(const FString& Text, const FString& Source);

		/** Lines for /perf and the console: the last finished summary window plus live numbers. */
		void DescribeSummary(TArray<FString>& Out, int32 TopN) const;
		void DescribeStatus(TArray<FString>& Out) const;
		void DescribeHitches(TArray<FString>& Out, int32 Count) const;
		void DescribeNet(TArray<FString>& Out) const;
		void RequestSnapshot() { bSnapshotRequested = true; }

		/** Hook bookkeeping, filled by the hook installer (shown by TickProfiler.Status and written to session.json). */
		void NoteHook(const FString& Name, bool bInstalled, const FString& Detail = FString());

	private:
		struct FHitch
		{
			FString Utc;
			double Uptime = 0.0;
			double WorkMs = 0.0;
			FString Text;
		};

		void OnBeginFrame();
		void OnEndFrame();
		void OnWorldTickStart(UWorld* World, ELevelTick TickType, float DeltaSeconds);
		void OnWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
		void OnPreGarbageCollect();
		void OnPostGarbageCollect();

		void MergeSlots();
		void StartSession(UWorld* World);
		void WriteSessionJson();
		void UpdateTarget();
		double GetHitchThresholdMs() const;

		void EmitSecond(double NowSeconds);
		void EmitClassWindow();
		void EmitSummary();
		void EmitNet();
		void EmitSnapshot();
		void HandleHitch(const FFrameRecord& Frame);

		static FString DescribeObject(const FObjKey& Key, FString& OutClassName);
		static void TopClasses(const TMap<FStatKey, FClassStat>& Map, int32 Count, TArray<const TPair<FStatKey, FClassStat>*>& Out);

		/** Id of a live class (game thread). The pointer cache is cleared before every garbage collection. */
		int32 InternClass(const UClass* Class);

		/** Puts the configured or the timed level into effect; true if the effective level changed. */
		bool ApplyEffectiveLevel();

		FSettings Settings;
		bool bLaunchOverride[NumSettings] = {};
		int32 TemporaryLevel = 0;
		/** FPlatformTime::Seconds() when the timed level ends; 0 when none is running. */
		double TemporaryLevelUntil = 0.0;
		bool bStarted = false;
		bool bSessionStarted = false;
		TWeakObjectPtr<UWorld> TrackedWorld;
		FString Role;

		// Current frame.
		uint64 BeginCycles = 0;
		uint64 LastEndCycles = 0;
		uint64 FrameCounter = 0;
		uint32 FrameGarbageCollections = 0;
		double FrameGarbageCollectMs = 0.0;
		double GarbageCollectStart = 0.0;
		TMap<FStatKey, uint64> FrameClassCycles;
		struct FClassInfo
		{
			FString Name;
			FString Origin;
		};
		TArray<FClassInfo> ClassInfos;
		TMap<FString, int32> ClassIdByPath;
		TMap<const UClass*, int32> ClassIdCache;
		double FrameKindGameThreadMs[NumObjKinds] = {};
		double FrameKindWorkerMs[NumObjKinds] = {};
		uint32 FrameKindCalls[NumObjKinds] = {};
		uint64 FrameReplicationBits = 0;

		// Windows.
		TArray<FFrameRecord> SecondFrames;
		double SecondStart = 0.0;
		TArray<double> SummaryWork;
		double SummaryStart = 0.0;
		double SummaryPhaseMs[NumPhases] = {};
		double SummaryPhaseMaxMs[NumPhases] = {};
		uint64 SummaryFrames = 0;
		double SummaryWorstSecondHz = 0.0;
		double SummaryIdleMs = 0.0;
		double SummaryUntimedMs = 0.0;
		double SummaryUntimedMaxMs = 0.0;
		int32 SummaryHitches = 0;
		double SummaryWorstHitchMs = 0.0;
		FString SummaryWorstHitchUtc;
		TMap<FStatKey, FClassStat> SummaryClasses;
		TMap<FStatKey, FClassStat> WindowClasses;
		TMap<FObjKey, FObjectStat> WindowObjects;
		double WindowStart = 0.0;
		double NextNet = 0.0;
		double NextSnapshot = 0.0;
		bool bSnapshotRequested = false;

		// Results kept for commands.
		TArray<FString> LastSummaryLines;
		TArray<FHitch> RecentHitches;
		TArray<FString> LastNetLines;
		double LastSecondHz = 0.0;
		double LastSecondWorkAvgMs = 0.0;
		double LastSecondWorkMaxMs = 0.0;

		// Hitch log rate limit.
		double HitchLogMinuteStart = 0.0;
		int32 HitchLogLinesThisMinute = 0;
		int32 HitchLogSuppressed = 0;

		// Target frame time (dedicated server tick rate cap), 0 if unknown/uncapped.
		double TargetHz = 0.0;
		int32 MaxClientRate = 0;
		int32 MaxInternetClientRate = 0;

		TArray<FString> HookNotes;
		/** Calls since startup, so TickProfiler.Status can show a hook that is installed but never runs. */
		uint64 TotalPhaseCalls[NumPhases] = {};
		uint64 TotalKindCalls[NumObjKinds] = {};
		bool bReportedSilentPhases = false;
		double ProfilerSelfMs = 0.0;
		FDelegateHandle BeginFrameHandle, EndFrameHandle, WorldTickStartHandle, PostActorTickHandle, PreGCHandle, PostGCHandle;
	};
}
