#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"

class UWorld;

/**
 * Installs the timing hooks. Global (not namespaced) because Config/AccessTransformers.ini names it as a friend of
 * UFGSaveSession, whose save function is private.
 *
 * A failed SML hook is fatal, so every hook can be skipped by name with -TickProfilerSkipHooks=Name1,Name2
 * (or -TickProfilerSkipHooks=All). Names: WorldTick, TickGroups, ActorTick, ComponentTick, FactoryTick, Timers,
 * Tickables, Latent, Streaming, EndOfFrame, GC, Save, ReplicateActor, Net.
 */
class FTickProfilerHooks
{
public:
	static void Install();

private:
	static bool ShouldInstall(const TCHAR* Name);
	static void InstallNetHooks(UWorld* World, ELevelTick TickType, float DeltaSeconds);

	static TSet<FString> Skip;
	static bool bSkipAll;
	static bool bNetHooksInstalled;
	static FDelegateHandle NetHookHandle;
};
