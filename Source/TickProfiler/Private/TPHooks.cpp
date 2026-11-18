#include "TPHooks.h"

#include "Buildables/FGBuildable.h"
#include "Containers/Ticker.h"
#include "TickTaskManagerInterface.h"
#include "Components/ActorComponent.h"
#include "Engine/ActorChannel.h"
#include "Engine/Engine.h"
#include "Engine/LatentActionManager.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "FGBuildableSubsystem.h"
#include "FGSaveSession.h"
#include "GameFramework/Actor.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Patching/NativeHookManager.h"
#include "Tickable.h"
#include "TimerManager.h"
#include "TPCollector.h"
#include "TPCore.h"

using namespace TickProfiler;

TSet<FString> FTickProfilerHooks::Skip;
bool FTickProfilerHooks::bSkipAll = false;
bool FTickProfilerHooks::bNetHooksInstalled = false;
FDelegateHandle FTickProfilerHooks::NetHookHandle;

namespace
{
	/** Logs before installing, so a fatal hook failure names the hook that caused it. */
	void Announce(const TCHAR* Name)
	{
		UE_LOG(LogTickProfiler, Display, TEXT("Installing hook %s (skip it with -TickProfilerSkipHooks=%s if it fails)"), Name, Name);
	}

	EPhase TickGroupPhase(const ETickingGroup Group)
	{
		// UWorld::Tick only runs TG_PrePhysics..TG_LastDemotable; newly spawned ticks run inside those groups.
		const int32 Index = static_cast<int32>(EPhase::TG_PrePhysics) + static_cast<int32>(Group);
		return Index <= static_cast<int32>(EPhase::TG_LastDemotable) ? static_cast<EPhase>(Index) : EPhase::TG_LastDemotable;
	}
}

bool FTickProfilerHooks::ShouldInstall(const TCHAR* Name)
{
	if (bSkipAll || Skip.Contains(Name))
	{
		FCollector::Get().NoteHook(Name, false, TEXT("skipped by -TickProfilerSkipHooks"));
		return false;
	}
	Announce(Name);
	return true;
}

void FTickProfilerHooks::Install()
{
	FString SkipList;
	if (FParse::Value(FCommandLine::Get(), TEXT("TickProfilerSkipHooks="), SkipList, false))
	{
		TArray<FString> Names;
		SkipList.ParseIntoArray(Names, TEXT(","));
		for (FString& Name : Names)
		{
			Name.TrimStartAndEndInline();
			bSkipAll |= Name.Equals(TEXT("All"), ESearchCase::IgnoreCase);
			Skip.Add(Name);
		}
	}

	if (ShouldInstall(TEXT("WorldTick")))
	{
		SUBSCRIBE_METHOD(UWorld::Tick, [](auto& Scope, UWorld* Self, ELevelTick TickType, float DeltaSeconds)
		{
			if (Self && Self->IsGameWorld())
			{
				FPhaseScope Phase(EPhase::WorldTick);
				Scope(Self, TickType, DeltaSeconds);
			}
		});
		FCollector::Get().NoteHook(TEXT("WorldTick"), true);
	}

	if (ShouldInstall(TEXT("TickGroups")))
	{
		// Not UWorld::RunTickGroup: it is defined next to UWorld::Tick and inlined into it in the shipped build, so a hook
		// on its exported copy never fires (seen on the Windows server). The tick task manager's RunTickGroup is virtual
		// and always called through the vtable; it is resolved through the live manager. Only game worlds tick groups
		// on a dedicated server or in a game, so no world filter is needed.
		SUBSCRIBE_METHOD_VIRTUAL(FTickTaskManagerInterface::RunTickGroup, &FTickTaskManagerInterface::Get(),
			[](auto& Scope, FTickTaskManagerInterface* Self, ETickingGroup Group, bool bBlockTillComplete)
			{
				FPhaseScope Phase(TickGroupPhase(Group));
				Scope(Self, Group, bBlockTillComplete);
			});
		FCollector::Get().NoteHook(TEXT("TickGroups"), true);
	}

	if (ShouldInstall(TEXT("ActorTick")))
	{
		// Virtual: resolved through the vtable of a real FActorTickFunction (the AActor default object's).
		SUBSCRIBE_METHOD_VIRTUAL(FActorTickFunction::ExecuteTick, &GetMutableDefault<AActor>()->PrimaryActorTick,
			[](auto& Scope, FActorTickFunction* Self, float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& CompletionEvent)
			{
				AActor* Target = Self->Target;
				if (GetLevel() < 2 || !Target)
				{
					return; // forwarded untimed
				}
				const uint64 Start = FPlatformTime::Cycles64();
				Scope(Self, DeltaTime, TickType, CurrentThread, CompletionEvent);
				RecordObject(EObjKind::ActorTick, Target, FPlatformTime::Cycles64() - Start);
			});
		FCollector::Get().NoteHook(TEXT("ActorTick"), true);
	}

	if (ShouldInstall(TEXT("ComponentTick")))
	{
		SUBSCRIBE_METHOD_VIRTUAL(FActorComponentTickFunction::ExecuteTick, &GetMutableDefault<UActorComponent>()->PrimaryComponentTick,
			[](auto& Scope, FActorComponentTickFunction* Self, float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& CompletionEvent)
			{
				UActorComponent* Target = Self->Target;
				if (GetLevel() < 2 || !Target)
				{
					return;
				}
				const uint64 Start = FPlatformTime::Cycles64();
				Scope(Self, DeltaTime, TickType, CurrentThread, CompletionEvent);
				RecordObject(EObjKind::ComponentTick, Target, FPlatformTime::Cycles64() - Start);
			});
		FCollector::Get().NoteHook(TEXT("ComponentTick"), true);
	}

	if (ShouldInstall(TEXT("FactoryTick")))
	{
		SUBSCRIBE_METHOD(AFGBuildableSubsystem::TickFactory, [](auto& Scope, AFGBuildableSubsystem* Self, float DeltaTime, ELevelTick TickType)
		{
			FPhaseScope Phase(EPhase::FactoryTick);
			Scope(Self, DeltaTime, TickType);
		});
		FCollector::Get().NoteHook(TEXT("FactoryTick"), true);
	}

	if (ShouldInstall(TEXT("Timers")))
	{
		SUBSCRIBE_METHOD(FTimerManager::Tick, [](auto& Scope, FTimerManager* Self, float DeltaTime)
		{
			FPhaseScope Phase(EPhase::TimerManager);
			Scope(Self, DeltaTime);
		});
		FCollector::Get().NoteHook(TEXT("Timers"), true);
	}

	if (ShouldInstall(TEXT("Tickables")))
	{
		SUBSCRIBE_METHOD(FTickableGameObject::TickObjects, [](auto& Scope, UWorld* World, ELevelTick TickType, bool bIsPaused, float DeltaSeconds)
		{
			FPhaseScope Phase(EPhase::TickableObjects);
			Scope(World, TickType, bIsPaused, DeltaSeconds);
		});
		FCollector::Get().NoteHook(TEXT("Tickables"), true);
	}

	if (ShouldInstall(TEXT("Latent")))
	{
		SUBSCRIBE_METHOD(FLatentActionManager::ProcessLatentActions, [](auto& Scope, FLatentActionManager* Self, UObject* InObject, float DeltaTime)
		{
			// Per-object calls happen inside actor ticks and are already counted there; time only the world-wide flush.
			if (InObject == nullptr)
			{
				FPhaseScope Phase(EPhase::LatentActions);
				Scope(Self, InObject, DeltaTime);
			}
		});
		FCollector::Get().NoteHook(TEXT("Latent"), true);
	}

	if (ShouldInstall(TEXT("Streaming")))
	{
		SUBSCRIBE_METHOD(UWorld::UpdateLevelStreaming, [](auto& Scope, UWorld* Self, const TOptional<const UE::FTimeout>& Timeout)
		{
			FPhaseScope Phase(EPhase::LevelStreaming);
			Scope(Self, Timeout);
		});
		FCollector::Get().NoteHook(TEXT("Streaming"), true);
	}

	if (ShouldInstall(TEXT("EndOfFrame")))
	{
		SUBSCRIBE_METHOD(UWorld::SendAllEndOfFrameUpdates, [](auto& Scope, UWorld* Self)
		{
			FPhaseScope Phase(EPhase::EndOfFrameUpdates);
			Scope(Self);
		});
		FCollector::Get().NoteHook(TEXT("EndOfFrame"), true);
	}

	if (ShouldInstall(TEXT("GC")))
	{
		SUBSCRIBE_METHOD(UEngine::ConditionalCollectGarbage, [](auto& Scope, UEngine* Self)
		{
			FPhaseScope Phase(EPhase::GarbageCollect);
			Scope(Self);
		});
		FCollector::Get().NoteHook(TEXT("GC"), true);
	}

	if (ShouldInstall(TEXT("Save")))
	{
		SUBSCRIBE_METHOD(UFGSaveSession::SaveWorldImplementation, [](auto& Scope, UFGSaveSession* Self, FString GameName)
		{
			FPhaseScope Phase(EPhase::SaveWorld);
			const uint64 Start = FPlatformTime::Cycles64();
			Scope(Self, GameName);
			UE_LOG(LogTickProfiler, Display, TEXT("Save took %.0f ms on the game thread"), CyclesToMs(FPlatformTime::Cycles64() - Start));
		});
		FCollector::Get().NoteHook(TEXT("Save"), true);
	}

	if (ShouldInstall(TEXT("CoreTicker")))
	{
		// Runs once per frame outside the world tick: HTTP (including the dedicated server's HTTPS API and the console
		// commands it runs), online services and other engine tickers. Other FTSTicker instances are not timed.
		SUBSCRIBE_METHOD(FTSTicker::Tick, [](auto& Scope, FTSTicker* Self, float DeltaTime)
		{
			if (Self == &FTSTicker::GetCoreTicker() && IsInGameThread())
			{
				FPhaseScope Phase(EPhase::CoreTicker);
				Scope(Self, DeltaTime);
			}
		});
		FCollector::Get().NoteHook(TEXT("CoreTicker"), true);
	}

	if (ShouldInstall(TEXT("ReplicateActor")))
	{
		SUBSCRIBE_METHOD(UActorChannel::ReplicateActor, [](auto& Scope, UActorChannel* Self)
		{
			AActor* Actor = Self->Actor;
			if (GetLevel() < 2 || !Actor)
			{
				return;
			}
			const uint64 Start = FPlatformTime::Cycles64();
			const int64 Bits = Scope(Self);
			RecordObject(EObjKind::Replication, Actor, FPlatformTime::Cycles64() - Start, Bits > 0 ? static_cast<uint64>(Bits) : 0);
		});
		FCollector::Get().NoteHook(TEXT("ReplicateActor"), true);
	}

	// The net driver's functions are virtual and the concrete class (IpNetDriver, SteamSockets, ...) is only known once
	// a world has one, so these hooks are installed from the first world tick that has a net driver. That runs on the
	// game thread before the world's own net dispatch, so the functions are not executing while they are patched.
	if (!bSkipAll && !Skip.Contains(TEXT("Net")))
	{
		NetHookHandle = FWorldDelegates::OnWorldTickStart.AddStatic(&FTickProfilerHooks::InstallNetHooks);
	}
	else
	{
		FCollector::Get().NoteHook(TEXT("Net"), false, TEXT("skipped by -TickProfilerSkipHooks"));
	}
}

void FTickProfilerHooks::InstallNetHooks(UWorld* World, ELevelTick, float)
{
	if (bNetHooksInstalled || !World || !World->IsGameWorld())
	{
		return;
	}
	UNetDriver* Driver = World->GetNetDriver();
	if (!Driver)
	{
		return;
	}
	bNetHooksInstalled = true;
	FWorldDelegates::OnWorldTickStart.Remove(NetHookHandle);
	const FString DriverClass = Driver->GetClass()->GetName();
	Announce(TEXT("Net"));

	SUBSCRIBE_METHOD_VIRTUAL(UNetDriver::TickDispatch, Driver, [](auto& Scope, UNetDriver* Self, float DeltaTime)
	{
		FPhaseScope Phase(EPhase::NetTickDispatch);
		Scope(Self, DeltaTime);
	});
	SUBSCRIBE_METHOD_VIRTUAL(UNetDriver::TickFlush, Driver, [](auto& Scope, UNetDriver* Self, float DeltaSeconds)
	{
		FPhaseScope Phase(EPhase::NetTickFlush);
		Scope(Self, DeltaSeconds);
	});
	SUBSCRIBE_METHOD_VIRTUAL(UNetDriver::ServerReplicateActors, Driver, [](auto& Scope, UNetDriver* Self, float DeltaSeconds)
	{
		FPhaseScope Phase(EPhase::NetReplicateActors);
		Scope(Self, DeltaSeconds);
	});
	FCollector::Get().NoteHook(TEXT("Net"), true, FString::Printf(TEXT("for %s; other net driver classes are not timed"), *DriverClass));
}
