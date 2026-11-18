#include "TPCollector.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Misc/Parse.h"
#include "ModLoading/ModLoadingLibrary.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TPHost.h"
#include "TPOutput.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace TickProfiler
{
	namespace
	{
		constexpr int32 MaxRecentHitches = 20;
		constexpr int32 HitchTopClasses = 8;
		constexpr int32 SnapshotTopClasses = 150;

		/** Where a class comes from: "FactoryGame"/"Engine" for native code, "Game" for base-game Blueprints, else the mod reference. */
		FString ClassOrigin(const UClass* Class)
		{
			if (!Class)
			{
				return TEXT("?");
			}
			const FString Package = Class->GetOutermost()->GetName();
			if (Package.StartsWith(TEXT("/Script/")))
			{
				return Package.Mid(8);
			}
			FString Root = Package.Mid(1);
			FString Left;
			if (Root.Split(TEXT("/"), &Left, nullptr))
			{
				Root = Left;
			}
			return Root;
		}

		FString CsvEscape(const FString& Value)
		{
			if (Value.Contains(TEXT(",")) || Value.Contains(TEXT("\"")) || Value.Contains(TEXT("\n")))
			{
				return TEXT("\"") + Value.Replace(TEXT("\""), TEXT("\"\"")) + TEXT("\"");
			}
			return Value;
		}

		FString JsonEscape(const FString& Value)
		{
			FString Out;
			Out.Reserve(Value.Len() + 2);
			for (const TCHAR Char : Value)
			{
				switch (Char)
				{
				case TEXT('"'): Out += TEXT("\\\""); break;
				case TEXT('\\'): Out += TEXT("\\\\"); break;
				case TEXT('\n'): Out += TEXT("\\n"); break;
				case TEXT('\r'): Out += TEXT("\\r"); break;
				case TEXT('\t'): Out += TEXT("\\t"); break;
				default:
					if (Char < 0x20)
					{
						Out += FString::Printf(TEXT("\\u%04x"), static_cast<int32>(Char));
					}
					else
					{
						Out.AppendChar(Char);
					}
				}
			}
			return Out;
		}

		double Percentile(TArray<double>& Sorted, const double Fraction)
		{
			if (Sorted.Num() == 0)
			{
				return 0.0;
			}
			const int32 Index = FMath::Clamp(FMath::CeilToInt(Fraction * Sorted.Num()) - 1, 0, Sorted.Num() - 1);
			return Sorted[Index];
		}

		int32 CountPlayers(const UWorld* World)
		{
			const AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
			return GameState ? GameState->PlayerArray.Num() : 0;
		}

		FString GetModVersion()
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("TickProfiler"));
			return Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : TEXT("?");
		}
	}

	FCollector& FCollector::Get()
	{
		static FCollector Instance;
		return Instance;
	}

	void FCollector::Startup()
	{
		if (bStarted)
		{
			return;
		}
		bStarted = true;

		// Launch parameters win over the settings file (which is applied later, once the game instance exists).
		const TCHAR* CommandLine = FCommandLine::Get();
		for (int32 Index = 0; Index < NumSettings; ++Index)
		{
			const ESetting Setting = static_cast<ESetting>(Index);
			const FSettingDef& Def = GetSettingDef(Setting);
			if (Def.Kind == ESettingKind::Bool)
			{
				// The only switch is WriteFiles, which a launch parameter can only turn off.
				if (FParse::Param(CommandLine, Def.LaunchParam))
				{
					SetSettingValue(Settings, Setting, 0.0);
					bLaunchOverride[Index] = true;
				}
				continue;
			}
			FString Text;
			double Value = 0.0;
			if (FParse::Value(CommandLine, *FString::Printf(TEXT("%s="), Def.LaunchParam), Text))
			{
				if (ParseSettingValue(Setting, Text, Value))
				{
					SetSettingValue(Settings, Setting, Value);
					bLaunchOverride[Index] = true;
				}
				else
				{
					UE_LOG(LogTickProfiler, Warning, TEXT("Ignoring launch parameter -%s=%s (not a number)"), Def.LaunchParam, *Text);
				}
			}
		}
		GLevel.store(Settings.Level);

		FString FramesHeader = TEXT("time_utc,uptime_s,frames,tick_hz,target_hz,period_avg_ms,period_max_ms,work_avg_ms,work_p50_ms,work_p95_ms,work_max_ms,idle_avg_ms,untimed_avg_ms,untimed_max_ms");
		for (int32 Index = 0; Index < NumPhases; ++Index)
		{
			const TCHAR* Column = GetPhaseColumn(static_cast<EPhase>(Index));
			FramesHeader += FString::Printf(TEXT(",%s_avg_ms,%s_max_ms"), Column, Column);
		}
		FramesHeader += TEXT(",actor_gt_ms,actor_worker_ms,actor_ticks,comp_gt_ms,comp_worker_ms,comp_ticks,repl_ms,repl_actors,repl_kbits,gc_count,gc_ms,hitches,players,level");
		SetFileHeader(EFile::Frames, FramesHeader);
		SetFileHeader(EFile::Classes, TEXT("time_utc,uptime_s,window_s,kind,class,origin,total_ms_per_s,gt_ms_per_s,worker_ms_per_s,calls_per_s,avg_us,max_call_ms,max_frame_ms,kbits_per_s"));
		SetFileHeader(EFile::Objects, TEXT("time_utc,uptime_s,window_s,kind,object,class,total_ms_per_s,calls_per_s,avg_us,max_call_ms,kbits_per_s"));
		SetFileHeader(EFile::Net, TEXT("time_utc,uptime_s,connection,player,ping_ms,in_kbytes_s,out_kbytes_s,in_packets_s,out_packets_s,in_lost,out_lost,queued_kbytes,net_speed_kbytes_s,saturated"));
		SetFileHeader(EFile::Marks, TEXT("time_utc,uptime_s,source,text"));

		BeginFrameHandle = FCoreDelegates::OnBeginFrame.AddRaw(this, &FCollector::OnBeginFrame);
		EndFrameHandle = FCoreDelegates::OnEndFrame.AddRaw(this, &FCollector::OnEndFrame);
		WorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddRaw(this, &FCollector::OnWorldTickStart);
		PostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FCollector::OnWorldPostActorTick);
		PreGCHandle = FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddRaw(this, &FCollector::OnPreGarbageCollect);
		PostGCHandle = FCoreUObjectDelegates::GetPostGarbageCollect().AddRaw(this, &FCollector::OnPostGarbageCollect);

		UE_LOG(LogTickProfiler, Display, TEXT("Tick Profiler %s started at level %d (0 paused, 1 basic, 2 detailed, 3 extreme)"), *GetModVersion(), Settings.Level);
	}

	void FCollector::Shutdown()
	{
		if (!bStarted)
		{
			return;
		}
		FCoreDelegates::OnBeginFrame.Remove(BeginFrameHandle);
		FCoreDelegates::OnEndFrame.Remove(EndFrameHandle);
		FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
		FWorldDelegates::OnWorldPostActorTick.Remove(PostActorTickHandle);
		FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(PreGCHandle);
		FCoreUObjectDelegates::GetPostGarbageCollect().Remove(PostGCHandle);
		if (bSessionStarted)
		{
			Mark(TEXT("profiler shutting down"), TEXT("TickProfiler"));
		}
		FOutput::Get().StopSession();
		bStarted = false;
	}

	void FCollector::ApplySettings(const FSettings& Wanted, const FString& Source)
	{
		for (int32 Index = 0; Index < NumSettings; ++Index)
		{
			const ESetting Setting = static_cast<ESetting>(Index);
			const FSettingDef& Def = GetSettingDef(Setting);
			const double Old = GetSettingValue(Settings, Setting);
			const double New = GetSettingValue(Wanted, Setting);
			if (Old == New || bLaunchOverride[Index] || (Def.bNeedsRestart && bSessionStarted))
			{
				continue;
			}
			SetSettingValue(Settings, Setting, New);
			Mark(FString::Printf(TEXT("setting %s = %s"), Def.Key, *FormatSettingValue(Setting, GetSettingValue(Settings, Setting))), Source);
		}
		ApplyEffectiveLevel();
	}

	bool FCollector::ApplyEffectiveLevel()
	{
		const int32 Wanted = TemporaryLevelUntil > 0.0 ? TemporaryLevel : Settings.Level;
		return GLevel.exchange(Wanted) != Wanted;
	}

	void FCollector::SetTemporaryLevel(const int32 Level, const double Minutes, const FString& Source)
	{
		TemporaryLevel = FMath::Clamp(Level, 0, 3);
		TemporaryLevelUntil = FPlatformTime::Seconds() + FMath::Clamp(Minutes, 0.1, 24.0 * 60.0) * 60.0;
		ApplyEffectiveLevel();
		Mark(FString::Printf(TEXT("timed capture: level %d for %s min, then back to %d"), TemporaryLevel, *FString::SanitizeFloat(Minutes), Settings.Level), Source);
	}

	void FCollector::CancelTemporaryLevel(const FString& Source)
	{
		if (TemporaryLevelUntil > 0.0)
		{
			TemporaryLevelUntil = 0.0;
			ApplyEffectiveLevel();
			Mark(FString::Printf(TEXT("timed capture cancelled; level %d"), Settings.Level), Source);
		}
	}

	bool FCollector::GetTemporaryLevel(int32& OutLevel, double& OutRemainingSec) const
	{
		if (TemporaryLevelUntil <= 0.0)
		{
			return false;
		}
		OutLevel = TemporaryLevel;
		OutRemainingSec = FMath::Max(0.0, TemporaryLevelUntil - FPlatformTime::Seconds());
		return true;
	}

	void FCollector::Mark(const FString& Text, const FString& Source)
	{
		UE_LOG(LogTickProfiler, Display, TEXT("MARK [%s] %s"), *Source, *Text);
		FOutput& Output = FOutput::Get();
		if (Output.IsActive())
		{
			Output.Write(EFile::Marks, FString::Printf(TEXT("%s,%s,%s"), *Output.TimeColumns(), *CsvEscape(Source), *CsvEscape(Text)));
		}
	}

	void FCollector::NoteHook(const FString& Name, const bool bInstalled, const FString& Detail)
	{
		HookNotes.Add(FString::Printf(TEXT("%s: %s%s%s"), *Name, bInstalled ? TEXT("installed") : TEXT("NOT installed"),
			Detail.IsEmpty() ? TEXT("") : TEXT(" - "), *Detail));
		if (bSessionStarted)
		{
			WriteSessionJson();
		}
	}

	void FCollector::OnBeginFrame()
	{
		if (BeginCycles == 0)
		{
			// Phases timed during startup (the first map load) belong to no frame; drop them so they do not inflate frame 1.
			double DiscardMs[NumPhases];
			uint32 DiscardCalls[NumPhases];
			DrainPhases(DiscardMs, DiscardCalls);
		}
		BeginCycles = FPlatformTime::Cycles64();
	}

	void FCollector::OnWorldTickStart(UWorld* World, ELevelTick, float)
	{
		if (!World || !World->IsGameWorld())
		{
			return;
		}
		if (TrackedWorld.Get() != World)
		{
			TrackedWorld = World;
			UpdateTarget();
			if (!bSessionStarted)
			{
				StartSession(World);
			}
			else
			{
				WriteSessionJson();
			}
			Mark(FString::Printf(TEXT("world %s, net mode %d, target %.0f Hz"), *World->GetMapName(), static_cast<int32>(World->GetNetMode()), TargetHz), TEXT("TickProfiler"));
		}
	}

	void FCollector::OnWorldPostActorTick(UWorld* World, ELevelTick, float)
	{
		// Merging here, before garbage collection, keeps per-object labels resolvable for objects destroyed this frame.
		if (World && World == TrackedWorld.Get())
		{
			const uint64 Start = FPlatformTime::Cycles64();
			MergeSlots();
			ProfilerSelfMs += CyclesToMs(FPlatformTime::Cycles64() - Start);
		}
	}

	void FCollector::OnPreGarbageCollect()
	{
		GarbageCollectStart = FPlatformTime::Seconds();
		// Everything recorded so far names classes and objects that are still alive. Resolve them now: this collection
		// may free them (a map load frees the old world's classes in the middle of a frame), and a freed address can be
		// reused by a different class afterwards.
		const uint64 Start = FPlatformTime::Cycles64();
		MergeSlots();
		ClassIdCache.Reset();
		ProfilerSelfMs += CyclesToMs(FPlatformTime::Cycles64() - Start);
	}

	int32 FCollector::InternClass(const UClass* Class)
	{
		if (const int32* Cached = ClassIdCache.Find(Class))
		{
			return *Cached;
		}
		int32 Id;
		const FString Path = Class ? Class->GetPathName() : FString(TEXT("?"));
		if (const int32* Known = ClassIdByPath.Find(Path))
		{
			Id = *Known;
		}
		else
		{
			Id = ClassInfos.Add(FClassInfo{Class ? Class->GetName() : FString(TEXT("?")), ClassOrigin(Class)});
			ClassIdByPath.Add(Path, Id);
		}
		ClassIdCache.Add(Class, Id);
		return Id;
	}

	void FCollector::OnPostGarbageCollect()
	{
		++FrameGarbageCollections;
		if (GarbageCollectStart > 0.0)
		{
			FrameGarbageCollectMs += (FPlatformTime::Seconds() - GarbageCollectStart) * 1000.0;
			GarbageCollectStart = 0.0;
		}
	}

	void FCollector::UpdateTarget()
	{
		TargetHz = 0.0;
		const UWorld* World = TrackedWorld.Get();
		const UNetDriver* Driver = World ? World->GetNetDriver() : nullptr;
		if (Driver)
		{
			MaxClientRate = Driver->MaxClientRate;
			MaxInternetClientRate = Driver->MaxInternetClientRate;
			if (World->GetNetMode() == NM_DedicatedServer)
			{
				TargetHz = Driver->GetNetServerMaxTickRate();
			}
		}
	}

	double FCollector::GetHitchThresholdMs() const
	{
		if (Settings.HitchMs > 0.0)
		{
			return Settings.HitchMs;
		}
		// Hitches are spikes. A server that is slow on every frame shows up in frames.csv, not as a hitch per frame.
		double Threshold = Settings.HitchMinMs;
		if (TargetHz > 0.0)
		{
			Threshold = FMath::Max(Threshold, 2000.0 / TargetHz);
		}
		return FMath::Max(Threshold, 2.0 * LastSecondWorkAvgMs);
	}

	void FCollector::StartSession(UWorld* World)
	{
		bSessionStarted = true;
		Role = IsRunningDedicatedServer() ? TEXT("server") : TEXT("game");
		const double Now = FPlatformTime::Seconds();
		SecondStart = SummaryStart = WindowStart = Now;
		NextNet = Now + 1.0;
		NextSnapshot = Now + 60.0; // first snapshot after the world has settled
		SummaryWorstSecondHz = 0.0;
		if (Settings.bWriteFiles)
		{
			FOutput::Get().StartSession(Role, Settings.DiskCapMB * 1024 * 1024, Settings.bSampleHost);
		}
		WriteSessionJson();
	}

	void FCollector::WriteSessionJson()
	{
		FOutput& Output = FOutput::Get();
		if (!Output.IsActive())
		{
			return;
		}
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("mod_version"), GetModVersion());
		Root->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Root->SetStringField(TEXT("build_version"), FApp::GetBuildVersion());
		Root->SetStringField(TEXT("platform"), ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()));
		Root->SetStringField(TEXT("role"), Role);
		Root->SetBoolField(TEXT("dedicated_server"), IsRunningDedicatedServer());
		// The tick task manager's single-thread decision (TickTaskManager.cpp, IsSingleThreadOnce): on a dedicated
		// server every actor and component tick runs on the game thread, even ticks marked "run on any thread",
		// unless the build is a multithread server. Measured on the Windows and Linux servers (2026-11-18).
		const bool bTicksOnGameThreadOnly = !(FApp::IsMultithreadServer() && FPlatformProcess::SupportsMultithreading())
			&& (!FApp::ShouldUseThreadingForPerformance() || IsRunningDedicatedServer() || FPlatformMisc::NumberOfCores() < 3 || !FPlatformProcess::SupportsMultithreading());
		Root->SetBoolField(TEXT("ticks_on_game_thread_only"), bTicksOnGameThreadOnly);
		Root->SetStringField(TEXT("session_dir"), FPaths::GetCleanFilename(Output.GetSessionDir()));
		if (const UWorld* World = TrackedWorld.Get())
		{
			Root->SetStringField(TEXT("map"), World->GetMapName());
			Root->SetNumberField(TEXT("net_mode"), static_cast<int32>(World->GetNetMode()));
			if (const UNetDriver* Driver = World->GetNetDriver())
			{
				Root->SetStringField(TEXT("net_driver_class"), Driver->GetClass()->GetName());
				Root->SetNumberField(TEXT("net_server_max_tick_rate"), Driver->GetNetServerMaxTickRate());
				Root->SetNumberField(TEXT("max_client_rate"), Driver->MaxClientRate);
				Root->SetNumberField(TEXT("max_internet_client_rate"), Driver->MaxInternetClientRate);
				if (const UReplicationDriver* Replication = Driver->GetReplicationDriver())
				{
					Root->SetStringField(TEXT("replication_driver_class"), Replication->GetClass()->GetName());
				}
			}
			if (const UGameInstance* GameInstance = World->GetGameInstance())
			{
				if (UModLoadingLibrary* Mods = GameInstance->GetSubsystem<UModLoadingLibrary>())
				{
					TArray<TSharedPtr<FJsonValue>> ModList;
					for (const FModInfo& Mod : Mods->GetLoadedMods())
					{
						ModList.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s %s"), *Mod.Name, *Mod.Version.ToString())));
					}
					Root->SetArrayField(TEXT("mods"), ModList);
				}
			}
		}
		const TSharedRef<FJsonObject> SettingsJson = MakeShared<FJsonObject>();
		// Keys as in the settings file. Values at the time this file was written; later changes are in marks.csv.
		TArray<TSharedPtr<FJsonValue>> LaunchOverrides;
		for (int32 Index = 0; Index < NumSettings; ++Index)
		{
			const ESetting Setting = static_cast<ESetting>(Index);
			const FSettingDef& Def = GetSettingDef(Setting);
			if (Def.Kind == ESettingKind::Bool)
			{
				SettingsJson->SetBoolField(Def.Key, GetSettingValue(Settings, Setting) != 0.0);
			}
			else
			{
				SettingsJson->SetNumberField(Def.Key, GetSettingValue(Settings, Setting));
			}
			if (bLaunchOverride[Index])
			{
				LaunchOverrides.Add(MakeShared<FJsonValueString>(Def.Key));
			}
		}
		SettingsJson->SetNumberField(TEXT("HitchMinMs"), Settings.HitchMinMs);
		SettingsJson->SetNumberField(TEXT("EffectiveLevel"), GetLevel());
		Root->SetObjectField(TEXT("settings"), SettingsJson);
		Root->SetArrayField(TEXT("settings_from_launch_parameters"), LaunchOverrides);
		const TSharedRef<FJsonObject> HostJson = MakeShared<FJsonObject>();
		FHostSampler::DescribeStatic(*HostJson);
		Root->SetObjectField(TEXT("host"), HostJson);
		TArray<TSharedPtr<FJsonValue>> Hooks;
		for (const FString& Note : HookNotes)
		{
			Hooks.Add(MakeShared<FJsonValueString>(Note));
		}
		Root->SetArrayField(TEXT("hooks"), Hooks);

		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Root, Writer);
		Output.WriteSessionJson(MoveTemp(Json));
	}

	FString FCollector::DescribeObject(const FObjKey& Key, FString& OutClassName)
	{
		const UObject* Object = Key.Object.ResolveObjectPtr();
		if (!Object)
		{
			OutClassName = TEXT("?");
			return TEXT("<destroyed>");
		}
		OutClassName = Object->GetClass()->GetName();
		const AActor* Actor = Cast<AActor>(Object);
		FString Name = Object->GetName();
		if (const UActorComponent* Component = Cast<UActorComponent>(Object))
		{
			Actor = Component->GetOwner();
			Name = FString::Printf(TEXT("%s.%s"), Actor ? *Actor->GetName() : TEXT("?"), *Component->GetName());
		}
		if (Actor && Actor->GetRootComponent())
		{
			const FVector Location = Actor->GetActorLocation() / 100.0; // metres
			Name += FString::Printf(TEXT(" @ %.0f %.0f %.0f m"), Location.X, Location.Y, Location.Z);
		}
		return Name;
	}

	void FCollector::MergeSlots()
	{
		ForEachSlot([this](FThreadSlot& Slot)
		{
			for (const TPair<FClassKey, FAcc>& Pair : Slot.Classes)
			{
				const FClassKey& Key = Pair.Key;
				const FAcc& Acc = Pair.Value;
				const int32 Kind = static_cast<int32>(Key.Kind);
				const double Ms = CyclesToMs(Acc.Cycles);
				(Slot.bGameThread ? FrameKindGameThreadMs[Kind] : FrameKindWorkerMs[Kind]) += Ms;
				FrameKindCalls[Kind] += Acc.Calls;
				if (Key.Kind == EObjKind::Replication)
				{
					FrameReplicationBits += Acc.Extra;
				}
				TotalKindCalls[Kind] += Acc.Calls;
				const FStatKey StatKey{InternClass(Key.Class), Key.Kind};
				FrameClassCycles.FindOrAdd(StatKey) += Acc.Cycles;
				for (TMap<FStatKey, FClassStat>* Target : {&WindowClasses, &SummaryClasses})
				{
					FClassStat& Stat = Target->FindOrAdd(StatKey);
					if (Stat.Name.IsEmpty())
					{
						Stat.Name = ClassInfos[StatKey.ClassId].Name;
						Stat.Origin = ClassInfos[StatKey.ClassId].Origin;
					}
					(Slot.bGameThread ? Stat.GameThreadCycles : Stat.WorkerCycles) += Acc.Cycles;
					Stat.MaxCallCycles = FMath::Max(Stat.MaxCallCycles, Acc.MaxCycles);
					Stat.Extra += Acc.Extra;
					Stat.Calls += Acc.Calls;
				}
			}
			Slot.Classes.Reset();
			for (const TPair<FObjKey, FAcc>& Pair : Slot.Objects)
			{
				FObjectStat& Stat = WindowObjects.FindOrAdd(Pair.Key);
				if (Stat.Name.IsEmpty())
				{
					Stat.Name = DescribeObject(Pair.Key, Stat.ClassName);
				}
				(Slot.bGameThread ? Stat.GameThreadCycles : Stat.WorkerCycles) += Pair.Value.Cycles;
				Stat.MaxCallCycles = FMath::Max(Stat.MaxCallCycles, Pair.Value.MaxCycles);
				Stat.Extra += Pair.Value.Extra;
				Stat.Calls += Pair.Value.Calls;
			}
			Slot.Objects.Reset();
		});
	}

	void FCollector::OnEndFrame()
	{
		if (!bStarted)
		{
			return;
		}
		if (TemporaryLevelUntil > 0.0 && FPlatformTime::Seconds() >= TemporaryLevelUntil)
		{
			// Before the level is read: a timed level 0 (paused) must still end.
			TemporaryLevelUntil = 0.0;
			ApplyEffectiveLevel();
			Mark(FString::Printf(TEXT("timed capture ended; back to level %d"), Settings.Level), TEXT("TickProfiler"));
		}
		const uint64 EndCycles = FPlatformTime::Cycles64();
		const uint64 SelfStart = EndCycles;
		const int32 Level = GetLevel();

		MergeSlots(); // replication happened after the post-actor-tick merge

		FFrameRecord Frame;
		Frame.FrameNumber = ++FrameCounter;
		Frame.EndSeconds = FPlatformTime::Seconds();
		const double WallMs = BeginCycles ? CyclesToMs(EndCycles - BeginCycles) : 0.0;
		Frame.IdleMs = FApp::GetIdleTime() * 1000.0;
		Frame.WorkMs = FMath::Max(0.0, WallMs - Frame.IdleMs);
		Frame.PeriodMs = LastEndCycles ? CyclesToMs(EndCycles - LastEndCycles) : WallMs;
		LastEndCycles = EndCycles;
		DrainPhases(Frame.PhaseMs, Frame.PhaseCalls);
		for (int32 Index = 0; Index < NumPhases; ++Index)
		{
			TotalPhaseCalls[Index] += Frame.PhaseCalls[Index];
		}
		Frame.UntimedMs = FMath::Max(0.0, Frame.WorkMs - Frame.PhaseMs[static_cast<int32>(EPhase::WorldTick)] - Frame.PhaseMs[static_cast<int32>(EPhase::CoreTicker)]);
		Frame.PhaseMs[static_cast<int32>(EPhase::ProfilerSelf)] += ProfilerSelfMs;
		ProfilerSelfMs = 0.0;
		for (int32 Kind = 0; Kind < NumObjKinds; ++Kind)
		{
			Frame.KindGameThreadMs[Kind] = FrameKindGameThreadMs[Kind];
			Frame.KindWorkerMs[Kind] = FrameKindWorkerMs[Kind];
			Frame.KindCalls[Kind] = FrameKindCalls[Kind];
			FrameKindGameThreadMs[Kind] = FrameKindWorkerMs[Kind] = 0.0;
			FrameKindCalls[Kind] = 0;
		}
		Frame.ReplicationBits = FrameReplicationBits;
		FrameReplicationBits = 0;
		Frame.GarbageCollections = FrameGarbageCollections;
		Frame.GarbageCollectMs = FrameGarbageCollectMs;
		FrameGarbageCollections = 0;
		FrameGarbageCollectMs = 0.0;

		if (!bSessionStarted || Level <= 0)
		{
			FrameClassCycles.Reset();
			return;
		}

		for (const TPair<FStatKey, uint64>& Pair : FrameClassCycles)
		{
			for (TMap<FStatKey, FClassStat>* Target : {&WindowClasses, &SummaryClasses})
			{
				if (FClassStat* Stat = Target->Find(Pair.Key))
				{
					Stat->MaxFrameCycles = FMath::Max(Stat->MaxFrameCycles, Pair.Value);
				}
			}
		}

		SecondFrames.Add(Frame);
		SummaryWork.Add(Frame.WorkMs);
		++SummaryFrames;
		SummaryIdleMs += Frame.IdleMs;
		SummaryUntimedMs += Frame.UntimedMs;
		SummaryUntimedMaxMs = FMath::Max(SummaryUntimedMaxMs, Frame.UntimedMs);
		for (int32 Index = 0; Index < NumPhases; ++Index)
		{
			SummaryPhaseMs[Index] += Frame.PhaseMs[Index];
			SummaryPhaseMaxMs[Index] = FMath::Max(SummaryPhaseMaxMs[Index], Frame.PhaseMs[Index]);
		}
		if (Frame.WorkMs > GetHitchThresholdMs())
		{
			HandleHitch(Frame);
		}
		FrameClassCycles.Reset();

		const double Now = Frame.EndSeconds;
		if (Now - SecondStart >= 1.0)
		{
			EmitSecond(Now);
		}
		if (Now - WindowStart >= Settings.ClassIntervalSec)
		{
			EmitClassWindow();
		}
		if (Now - SummaryStart >= Settings.SummaryIntervalSec)
		{
			EmitSummary();
		}
		if (Level >= 2 && Now >= NextNet)
		{
			EmitNet();
			NextNet = Now + 1.0;
		}
		if (Level >= 2 && (bSnapshotRequested || Now >= NextSnapshot))
		{
			EmitSnapshot();
			bSnapshotRequested = false;
			NextSnapshot = Now + Settings.SnapshotIntervalSec;
		}
		ProfilerSelfMs += CyclesToMs(FPlatformTime::Cycles64() - SelfStart);
	}

	void FCollector::HandleHitch(const FFrameRecord& Frame)
	{
		FOutput& Output = FOutput::Get();
		++SummaryHitches;
		const FString Utc = FDateTime::UtcNow().ToIso8601();
		if (Frame.WorkMs > SummaryWorstHitchMs)
		{
			SummaryWorstHitchMs = Frame.WorkMs;
			SummaryWorstHitchUtc = Utc;
		}

		// Phases, largest first. "World tick" contains the tick groups, so it is listed but not summed.
		TArray<TPair<double, int32>> Phases;
		for (int32 Index = 0; Index < NumPhases; ++Index)
		{
			if (Frame.PhaseMs[Index] >= 0.05)
			{
				Phases.Emplace(Frame.PhaseMs[Index], Index);
			}
		}
		Phases.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key > B.Key; });

		TArray<TPair<uint64, FStatKey>> Top;
		for (const TPair<FStatKey, uint64>& Pair : FrameClassCycles)
		{
			Top.Emplace(Pair.Value, Pair.Key);
		}
		Top.Sort([](const TPair<uint64, FStatKey>& A, const TPair<uint64, FStatKey>& B) { return A.Key > B.Key; });
		if (Top.Num() > HitchTopClasses)
		{
			Top.SetNum(HitchTopClasses);
		}

		FString Json = FString::Printf(TEXT("{\"time_utc\":\"%s\",\"uptime_s\":%.3f,\"frame\":%llu,\"work_ms\":%.2f,\"period_ms\":%.2f,\"idle_ms\":%.2f,\"threshold_ms\":%.1f,\"gc_count\":%u,\"gc_ms\":%.2f,\"untimed_ms\":%.2f,\"phases\":{"),
			*Utc, Output.GetUptime(), Frame.FrameNumber, Frame.WorkMs, Frame.PeriodMs, Frame.IdleMs, GetHitchThresholdMs(), Frame.GarbageCollections, Frame.GarbageCollectMs, Frame.UntimedMs);
		// Untimed work leads the text when it is most of the hitch: the hooks then did not see where the frame went.
		FString PhaseText = Frame.UntimedMs >= 0.5 * Frame.WorkMs ? FString::Printf(TEXT("UNTIMED (outside the world tick) %.1f, "), Frame.UntimedMs) : FString();
		for (int32 Index = 0; Index < Phases.Num(); ++Index)
		{
			const EPhase Phase = static_cast<EPhase>(Phases[Index].Value);
			Json += FString::Printf(TEXT("%s\"%s\":%.2f"), Index ? TEXT(",") : TEXT(""), GetPhaseColumn(Phase), Phases[Index].Key);
			if (Index < 6)
			{
				PhaseText += FString::Printf(TEXT("%s%s %.1f"), Index ? TEXT(", ") : TEXT(""), GetPhaseName(Phase), Phases[Index].Key);
			}
		}
		Json += TEXT("},\"top\":[");
		FString TopText;
		for (int32 Index = 0; Index < Top.Num(); ++Index)
		{
			const FStatKey& Key = Top[Index].Value;
			const FClassInfo& Info = ClassInfos[Key.ClassId];
			const FString& Name = Info.Name;
			const double Ms = CyclesToMs(Top[Index].Key);
			Json += FString::Printf(TEXT("%s{\"class\":\"%s\",\"origin\":\"%s\",\"kind\":\"%s\",\"ms\":%.2f}"),
				Index ? TEXT(",") : TEXT(""), *JsonEscape(Name), *JsonEscape(Info.Origin), GetKindName(Key.Kind), Ms);
			if (Index < 5)
			{
				TopText += FString::Printf(TEXT("%s%s %.1f"), Index ? TEXT(", ") : TEXT(""), *Name, Ms);
			}
		}
		Json += TEXT("]}");
		Output.Write(EFile::Hitches, MoveTemp(Json));

		const FString Text = FString::Printf(TEXT("HITCH %.0f ms (threshold %.0f)%s | phases ms: %s | top: %s"),
			Frame.WorkMs, GetHitchThresholdMs(), Frame.GarbageCollections ? TEXT(" [GC]") : TEXT(""), *PhaseText, TopText.IsEmpty() ? (GetLevel() >= 2 ? TEXT("(no actor or component ticks timed in this frame)") : TEXT("(per-class data needs level 2)")) : *TopText);
		RecentHitches.Add(FHitch{Utc, Output.GetUptime(), Frame.WorkMs, Text});
		if (RecentHitches.Num() > MaxRecentHitches)
		{
			RecentHitches.RemoveAt(0);
		}

		if (Frame.EndSeconds - HitchLogMinuteStart >= 60.0)
		{
			if (HitchLogSuppressed > 0)
			{
				UE_LOG(LogTickProfiler, Display, TEXT("%d more hitches were not logged in the last minute (all are in hitches.jsonl)"), HitchLogSuppressed);
			}
			HitchLogMinuteStart = Frame.EndSeconds;
			HitchLogLinesThisMinute = 0;
			HitchLogSuppressed = 0;
		}
		if (HitchLogLinesThisMinute++ < Settings.MaxHitchLogLinesPerMinute)
		{
			UE_LOG(LogTickProfiler, Display, TEXT("%s"), *Text);
		}
		else
		{
			++HitchLogSuppressed;
		}
	}

	void FCollector::EmitSecond(const double NowSeconds)
	{
		const double Elapsed = FMath::Max(NowSeconds - SecondStart, 1e-6);
		SecondStart = NowSeconds;
		const int32 Frames = SecondFrames.Num();
		if (Frames == 0)
		{
			return;
		}
		TArray<double> Work;
		Work.Reserve(Frames);
		double PeriodSum = 0, PeriodMax = 0, IdleSum = 0, WorkSum = 0, UntimedSum = 0, UntimedMax = 0;
		double PhaseSum[NumPhases] = {}, PhaseMax[NumPhases] = {};
		double KindGt[NumObjKinds] = {}, KindWorker[NumObjKinds] = {};
		uint64 KindCalls[NumObjKinds] = {};
		uint64 ReplBits = 0;
		uint32 GcCount = 0;
		double GcMs = 0;
		int32 Hitches = 0;
		const double Threshold = GetHitchThresholdMs();
		for (const FFrameRecord& Frame : SecondFrames)
		{
			Work.Add(Frame.WorkMs);
			WorkSum += Frame.WorkMs;
			PeriodSum += Frame.PeriodMs;
			PeriodMax = FMath::Max(PeriodMax, Frame.PeriodMs);
			IdleSum += Frame.IdleMs;
			UntimedSum += Frame.UntimedMs;
			UntimedMax = FMath::Max(UntimedMax, Frame.UntimedMs);
			for (int32 Index = 0; Index < NumPhases; ++Index)
			{
				PhaseSum[Index] += Frame.PhaseMs[Index];
				PhaseMax[Index] = FMath::Max(PhaseMax[Index], Frame.PhaseMs[Index]);
			}
			for (int32 Kind = 0; Kind < NumObjKinds; ++Kind)
			{
				KindGt[Kind] += Frame.KindGameThreadMs[Kind];
				KindWorker[Kind] += Frame.KindWorkerMs[Kind];
				KindCalls[Kind] += Frame.KindCalls[Kind];
			}
			ReplBits += Frame.ReplicationBits;
			GcCount += Frame.GarbageCollections;
			GcMs += Frame.GarbageCollectMs;
			Hitches += Frame.WorkMs > Threshold ? 1 : 0;
		}
		Work.Sort();
		const double Hz = Frames / Elapsed;
		LastSecondHz = Hz;
		LastSecondWorkAvgMs = WorkSum / Frames;
		LastSecondWorkMaxMs = Work.Last();
		SummaryWorstSecondHz = SummaryWorstSecondHz <= 0.0 ? Hz : FMath::Min(SummaryWorstSecondHz, Hz);

		FOutput& Output = FOutput::Get();
		if (Output.IsActive())
		{
			FString Row = FString::Printf(TEXT("%s,%d,%.2f,%.0f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f"),
				*Output.TimeColumns(), Frames, Hz, TargetHz, PeriodSum / Frames, PeriodMax, LastSecondWorkAvgMs,
				Percentile(Work, 0.5), Percentile(Work, 0.95), LastSecondWorkMaxMs, IdleSum / Frames, UntimedSum / Frames, UntimedMax);
			for (int32 Index = 0; Index < NumPhases; ++Index)
			{
				Row += FString::Printf(TEXT(",%.3f,%.3f"), PhaseSum[Index] / Frames, PhaseMax[Index]);
			}
			const int32 Actor = static_cast<int32>(EObjKind::ActorTick);
			const int32 Comp = static_cast<int32>(EObjKind::ComponentTick);
			const int32 Repl = static_cast<int32>(EObjKind::Replication);
			Row += FString::Printf(TEXT(",%.3f,%.3f,%llu,%.3f,%.3f,%llu,%.3f,%llu,%.1f,%u,%.2f,%d,%d,%d"),
				KindGt[Actor] / Frames, KindWorker[Actor] / Frames, KindCalls[Actor],
				KindGt[Comp] / Frames, KindWorker[Comp] / Frames, KindCalls[Comp],
				(KindGt[Repl] + KindWorker[Repl]) / Frames, KindCalls[Repl], ReplBits / 1000.0 / Elapsed,
				GcCount, GcMs, Hitches, CountPlayers(TrackedWorld.Get()), GetLevel());
			Output.Write(EFile::Frames, MoveTemp(Row));
		}
		SecondFrames.Reset();
	}

	void FCollector::TopClasses(const TMap<FStatKey, FClassStat>& Map, const int32 Count, TArray<const TPair<FStatKey, FClassStat>*>& Out)
	{
		Out.Reset();
		for (const TPair<FStatKey, FClassStat>& Pair : Map)
		{
			Out.Add(&Pair);
		}
		Out.Sort([](const TPair<FStatKey, FClassStat>& A, const TPair<FStatKey, FClassStat>& B)
		{
			return A.Value.TotalCycles() > B.Value.TotalCycles();
		});
		if (Out.Num() > Count)
		{
			Out.SetNum(Count);
		}
	}

	void FCollector::EmitClassWindow()
	{
		const double Now = FPlatformTime::Seconds();
		const double Window = FMath::Max(Now - WindowStart, 1e-6);
		WindowStart = Now;
		FOutput& Output = FOutput::Get();
		if (Output.IsActive() && GetLevel() >= 2)
		{
			const FString Time = Output.TimeColumns();
			for (int32 Kind = 0; Kind < NumObjKinds; ++Kind)
			{
				TMap<FStatKey, FClassStat> OfKind;
				for (const TPair<FStatKey, FClassStat>& Pair : WindowClasses)
				{
					if (static_cast<int32>(Pair.Key.Kind) == Kind)
					{
						OfKind.Add(Pair.Key, Pair.Value);
					}
				}
				TArray<const TPair<FStatKey, FClassStat>*> Top;
				TopClasses(OfKind, Settings.TopN, Top);
				for (const TPair<FStatKey, FClassStat>* Pair : Top)
				{
					const FClassStat& Stat = Pair->Value;
					Output.Write(EFile::Classes, FString::Printf(TEXT("%s,%.1f,%s,%s,%s,%.3f,%.3f,%.3f,%.1f,%.2f,%.3f,%.3f,%.1f"),
						*Time, Window, GetKindName(Pair->Key.Kind), *CsvEscape(Stat.Name), *CsvEscape(Stat.Origin),
						CyclesToMs(Stat.TotalCycles()) / Window, CyclesToMs(Stat.GameThreadCycles) / Window, CyclesToMs(Stat.WorkerCycles) / Window,
						Stat.Calls / Window, Stat.Calls ? CyclesToMs(Stat.TotalCycles()) * 1000.0 / Stat.Calls : 0.0,
						CyclesToMs(Stat.MaxCallCycles), CyclesToMs(Stat.MaxFrameCycles), Stat.Extra / 1000.0 / Window));
				}
			}
			if (GetLevel() >= 3)
			{
				for (int32 Kind = 0; Kind < NumObjKinds; ++Kind)
				{
					TArray<const TPair<FObjKey, FObjectStat>*> Top;
					for (const TPair<FObjKey, FObjectStat>& Pair : WindowObjects)
					{
						if (static_cast<int32>(Pair.Key.Kind) == Kind)
						{
							Top.Add(&Pair);
						}
					}
					Top.Sort([](const TPair<FObjKey, FObjectStat>& A, const TPair<FObjKey, FObjectStat>& B) { return A.Value.TotalCycles() > B.Value.TotalCycles(); });
					for (int32 Index = 0; Index < FMath::Min(Top.Num(), Settings.TopN); ++Index)
					{
						const FObjectStat& Stat = Top[Index]->Value;
						Output.Write(EFile::Objects, FString::Printf(TEXT("%s,%.1f,%s,%s,%s,%.3f,%.1f,%.2f,%.3f,%.1f"),
							*Time, Window, GetKindName(static_cast<EObjKind>(Kind)), *CsvEscape(Stat.Name), *CsvEscape(Stat.ClassName),
							CyclesToMs(Stat.TotalCycles()) / Window, Stat.Calls / Window,
							Stat.Calls ? CyclesToMs(Stat.TotalCycles()) * 1000.0 / Stat.Calls : 0.0,
							CyclesToMs(Stat.MaxCallCycles), Stat.Extra / 1000.0 / Window));
					}
				}
			}
		}
		WindowClasses.Reset();
		WindowObjects.Reset();
	}

	void FCollector::EmitSummary()
	{
		const double Now = FPlatformTime::Seconds();
		const double Window = FMath::Max(Now - SummaryStart, 1e-6);
		SummaryStart = Now;
		TArray<FString> Lines;
		const UWorld* World = TrackedWorld.Get();
		if (SummaryFrames > 0)
		{
			SummaryWork.Sort();
			const double AvgHz = SummaryFrames / Window;
			Lines.Add(FString::Printf(TEXT("=== Summary of the last %.0f s (%s, level %d, %d players) ==="), Window, *Role, GetLevel(), CountPlayers(World)));
			Lines.Add(FString::Printf(TEXT("Tick rate %.1f Hz avg, worst second %.1f Hz%s. Frame work ms: avg %.1f, p50 %.1f, p95 %.1f, max %.1f. Idle (sleeping) %.1f ms/frame."),
				AvgHz, SummaryWorstSecondHz, TargetHz > 0 ? *FString::Printf(TEXT(", target %.0f Hz"), TargetHz) : TEXT(""),
				[this] { double Sum = 0; for (const double W : SummaryWork) { Sum += W; } return Sum / SummaryWork.Num(); }(),
				Percentile(SummaryWork, 0.5), Percentile(SummaryWork, 0.95), SummaryWork.Last(), SummaryIdleMs / SummaryFrames));

			TArray<TPair<double, int32>> Phases;
			for (int32 Index = 0; Index < NumPhases; ++Index)
			{
				if (SummaryPhaseMs[Index] / SummaryFrames >= 0.01 || SummaryPhaseMaxMs[Index] >= 1.0)
				{
					Phases.Emplace(SummaryPhaseMs[Index] / SummaryFrames, Index);
				}
			}
			Phases.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key > B.Key; });
			FString PhaseLine = TEXT("Phases, avg ms/frame (worst frame):");
			for (const TPair<double, int32>& Phase : Phases)
			{
				PhaseLine += FString::Printf(TEXT(" %s %.2f (%.1f);"), GetPhaseName(static_cast<EPhase>(Phase.Value)), Phase.Key, SummaryPhaseMaxMs[Phase.Value]);
			}
			Lines.Add(PhaseLine);
			Lines.Add(FString::Printf(TEXT("Untimed work (inside neither the world tick nor the core ticker): %.2f ms/frame avg, worst frame %.1f ms."),
				SummaryUntimedMs / SummaryFrames, SummaryUntimedMaxMs));
			if (!bReportedSilentPhases)
			{
				// Once per run: an installed hook whose phase never ran may be one the shipped build bypasses (inlined).
				bReportedSilentPhases = true;
				FString Silent;
				for (int32 Index = 0; Index < NumPhases; ++Index)
				{
					const EPhase Phase = static_cast<EPhase>(Index);
					if (TotalPhaseCalls[Index] == 0 && Phase != EPhase::SaveWorld && Phase != EPhase::ProfilerSelf)
					{
						Silent += FString::Printf(TEXT(" %s;"), GetPhaseColumn(Phase));
					}
				}
				if (!Silent.IsEmpty())
				{
					Lines.Add(TEXT("Phases with no calls so far (skipped, not used here, or bypassed by the shipped build):") + Silent);
				}
			}

			if (GetLevel() >= 2)
			{
				TArray<const TPair<FStatKey, FClassStat>*> Top;
				TopClasses(SummaryClasses, Settings.TopN, Top);
				Lines.Add(TEXT("Top classes by time, ms per second (game thread / worker threads), calls/s, avg us per call, worst single frame ms:"));
				int32 Rank = 0;
				for (const TPair<FStatKey, FClassStat>* Pair : Top)
				{
					const FClassStat& Stat = Pair->Value;
					Lines.Add(FString::Printf(TEXT("  %2d. %s [%s, %s] %.2f ms/s (%.2f / %.2f), %.0f calls/s, %.1f us, worst frame %.2f ms%s"),
						++Rank, *Stat.Name, *Stat.Origin, GetKindName(Pair->Key.Kind),
						CyclesToMs(Stat.TotalCycles()) / Window, CyclesToMs(Stat.GameThreadCycles) / Window, CyclesToMs(Stat.WorkerCycles) / Window,
						Stat.Calls / Window, Stat.Calls ? CyclesToMs(Stat.TotalCycles()) * 1000.0 / Stat.Calls : 0.0, CyclesToMs(Stat.MaxFrameCycles),
						Pair->Key.Kind == EObjKind::Replication ? *FString::Printf(TEXT(", %.1f kbit/s"), Stat.Extra / 1000.0 / Window) : TEXT("")));
				}
			}
			Lines.Add(FString::Printf(TEXT("Hitches: %d%s. Profiler's own game-thread cost: %.2f ms/frame (excludes the hook call overhead)."),
				SummaryHitches, SummaryHitches ? *FString::Printf(TEXT(" (worst %.0f ms at %s)"), SummaryWorstHitchMs, *SummaryWorstHitchUtc) : TEXT(""),
				SummaryPhaseMs[static_cast<int32>(EPhase::ProfilerSelf)] / SummaryFrames));
		}
		for (const FString& Line : Lines)
		{
			UE_LOG(LogTickProfiler, Display, TEXT("%s"), *Line);
		}
		if (Lines.Num() > 0)
		{
			LastSummaryLines = MoveTemp(Lines);
		}

		SummaryWork.Reset();
		SummaryFrames = 0;
		SummaryIdleMs = 0.0;
		SummaryUntimedMs = 0.0;
		SummaryUntimedMaxMs = 0.0;
		FMemory::Memzero(SummaryPhaseMs);
		FMemory::Memzero(SummaryPhaseMaxMs);
		SummaryWorstSecondHz = 0.0;
		SummaryHitches = 0;
		SummaryWorstHitchMs = 0.0;
		SummaryWorstHitchUtc.Reset();
		SummaryClasses.Reset();
	}

	void FCollector::EmitNet()
	{
		const UWorld* World = TrackedWorld.Get();
		const UNetDriver* Driver = World ? World->GetNetDriver() : nullptr;
		LastNetLines.Reset();
		if (!Driver)
		{
			return;
		}
		TArray<UNetConnection*> Connections;
		for (const TObjectPtr<UNetConnection>& Connection : Driver->ClientConnections)
		{
			Connections.Add(Connection.Get());
		}
		if (Driver->ServerConnection)
		{
			Connections.Add(Driver->ServerConnection.Get());
		}
		FOutput& Output = FOutput::Get();
		const FString Time = Output.IsActive() ? Output.TimeColumns() : FString();
		int32 Index = 0;
		for (UNetConnection* Connection : Connections)
		{
			if (!Connection)
			{
				continue;
			}
			// On a client the server connection carries the local player's own controller, so it keeps the "<server>" label.
			FString Player = Connection == Driver->ServerConnection ? TEXT("<server>") : TEXT("?");
			if (Connection != Driver->ServerConnection && Connection->PlayerController && Connection->PlayerController->PlayerState)
			{
				Player = Connection->PlayerController->PlayerState->GetPlayerName();
			}
			const bool bSaturated = !Connection->IsNetReady();
			// QueuedBits goes negative while the connection has bandwidth to spare; only a positive value is a backlog.
			const double QueuedKB = FMath::Max<int64>(Connection->QueuedBits, 0) / 8192.0;
			LastNetLines.Add(FString::Printf(TEXT("%s: ping %.0f ms, in %.1f KB/s, out %.1f KB/s, lost in/out %d/%d, send backlog %.1f KB, rate cap %.0f KB/s%s"),
				*Player, Connection->AvgLag * 1000.0, Connection->InBytesPerSecond / 1024.0, Connection->OutBytesPerSecond / 1024.0,
				Connection->InPacketsLost, Connection->OutPacketsLost, QueuedKB, Connection->CurrentNetSpeed / 1024.0,
				bSaturated ? TEXT(", SATURATED") : TEXT("")));
			if (Output.IsActive())
			{
				Output.Write(EFile::Net, FString::Printf(TEXT("%s,%d,%s,%.1f,%.2f,%.2f,%d,%d,%d,%d,%.2f,%.1f,%d"),
					*Time, Index, *CsvEscape(Player), Connection->AvgLag * 1000.0, Connection->InBytesPerSecond / 1024.0, Connection->OutBytesPerSecond / 1024.0,
					Connection->InPacketsPerSecond, Connection->OutPacketsPerSecond, Connection->InPacketsLost, Connection->OutPacketsLost,
					QueuedKB, Connection->CurrentNetSpeed / 1024.0, bSaturated ? 1 : 0));
			}
			++Index;
		}
	}

	void FCollector::EmitSnapshot()
	{
		UWorld* World = TrackedWorld.Get();
		FOutput& Output = FOutput::Get();
		if (!World || !Output.IsActive())
		{
			return;
		}
		const uint64 Start = FPlatformTime::Cycles64();
		struct FCount
		{
			int32 Total = 0;
			int32 Ticking = 0;
		};
		TMap<const UClass*, FCount> Counts;
		int32 Total = 0;
		int32 Ticking = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			const AActor* Actor = *It;
			FCount& Count = Counts.FindOrAdd(Actor->GetClass());
			++Count.Total;
			++Total;
			if (Actor->PrimaryActorTick.bCanEverTick && Actor->PrimaryActorTick.IsTickFunctionEnabled())
			{
				++Count.Ticking;
				++Ticking;
			}
		}
		TArray<TPair<const UClass*, FCount>> Sorted = Counts.Array();
		Sorted.Sort([](const TPair<const UClass*, FCount>& A, const TPair<const UClass*, FCount>& B) { return A.Value.Total > B.Value.Total; });
		const double ElapsedMs = CyclesToMs(FPlatformTime::Cycles64() - Start);

		FString Json = FString::Printf(TEXT("{\"time_utc\":\"%s\",\"uptime_s\":%.3f,\"players\":%d,\"actors\":%d,\"actors_ticking\":%d,\"classes\":%d,\"snapshot_ms\":%.2f,\"top\":["),
			*FDateTime::UtcNow().ToIso8601(), Output.GetUptime(), CountPlayers(World), Total, Ticking, Counts.Num(), ElapsedMs);
		for (int32 Index = 0; Index < FMath::Min(Sorted.Num(), SnapshotTopClasses); ++Index)
		{
			Json += FString::Printf(TEXT("%s{\"class\":\"%s\",\"origin\":\"%s\",\"count\":%d,\"ticking\":%d}"), Index ? TEXT(",") : TEXT(""),
				*JsonEscape(Sorted[Index].Key->GetName()), *JsonEscape(ClassOrigin(Sorted[Index].Key)), Sorted[Index].Value.Total, Sorted[Index].Value.Ticking);
		}
		Json += TEXT("]}");
		Output.Write(EFile::Snapshots, MoveTemp(Json));
		UE_LOG(LogTickProfiler, Display, TEXT("Snapshot: %d actors (%d ticking) in %d classes, took %.1f ms (profiler work, counted as 'TickProfiler itself')"), Total, Ticking, Counts.Num(), ElapsedMs);
	}

	void FCollector::DescribeSummary(TArray<FString>& Out, const int32 TopN) const
	{
		Out.Add(FString::Printf(TEXT("Last second: %.1f Hz%s, frame work avg %.1f ms, max %.1f ms. Level %d."),
			LastSecondHz, TargetHz > 0 ? *FString::Printf(TEXT(" (target %.0f)"), TargetHz) : TEXT(""), LastSecondWorkAvgMs, LastSecondWorkMaxMs, GetLevel()));
		if (LastSummaryLines.Num() == 0)
		{
			Out.Add(FString::Printf(TEXT("No full summary yet; the first comes %.0f s after the world loads."), Settings.SummaryIntervalSec));
			return;
		}
		int32 ClassLines = 0;
		for (const FString& Line : LastSummaryLines)
		{
			const bool bClassLine = Line.StartsWith(TEXT("  "));
			if (bClassLine && ++ClassLines > TopN)
			{
				continue;
			}
			Out.Add(Line);
		}
	}

	void FCollector::DescribeStatus(TArray<FString>& Out) const
	{
		const FOutput& Output = FOutput::Get();
		Out.Add(FString::Printf(TEXT("Tick Profiler %s, level %d, role %s, hitch threshold %.0f ms"), *GetModVersion(), GetLevel(), *Role, GetHitchThresholdMs()));
		Out.Add(Output.IsActive()
			? FString::Printf(TEXT("Data files: Logs/TickProfiler/%s (%.1f MB this session, cap %lld MB%s)"),
				*FPaths::GetCleanFilename(Output.GetSessionDir()), Output.GetSessionBytes() / (1024.0 * 1024.0), Settings.DiskCapMB,
				Output.IsDiskCapReached() ? TEXT(", CAP REACHED") : TEXT(""))
			: FString(TEXT("Data files: off")));
		for (const FString& Note : HookNotes)
		{
			Out.Add(TEXT("  hook ") + Note);
		}
		FString Calls = TEXT("Calls since start:");
		for (int32 Index = 0; Index < NumPhases; ++Index)
		{
			if (static_cast<EPhase>(Index) != EPhase::ProfilerSelf)
			{
				Calls += FString::Printf(TEXT(" %s %llu;"), GetPhaseColumn(static_cast<EPhase>(Index)), TotalPhaseCalls[Index]);
			}
		}
		for (int32 Kind = 0; Kind < NumObjKinds; ++Kind)
		{
			Calls += FString::Printf(TEXT(" %s %llu;"), GetKindName(static_cast<EObjKind>(Kind)), TotalKindCalls[Kind]);
		}
		Out.Add(Calls);
	}

	void FCollector::DescribeHitches(TArray<FString>& Out, const int32 Count) const
	{
		if (RecentHitches.Num() == 0)
		{
			Out.Add(FString::Printf(TEXT("No hitches recorded (threshold %.0f ms)."), GetHitchThresholdMs()));
			return;
		}
		for (int32 Index = FMath::Max(0, RecentHitches.Num() - Count); Index < RecentHitches.Num(); ++Index)
		{
			Out.Add(FString::Printf(TEXT("%s %s"), *RecentHitches[Index].Utc, *RecentHitches[Index].Text));
		}
	}

	void FCollector::DescribeNet(TArray<FString>& Out) const
	{
		if (LastNetLines.Num() == 0)
		{
			Out.Add(TEXT("No network connections (or level below 2)."));
			return;
		}
		Out.Append(LastNetLines);
	}
}
