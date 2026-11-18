#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TPTestActor.generated.h"

/**
 * Ground-truth load for testing the profiler: busy-waits a known time every tick. Only the dev command
 * TickProfiler.DevBusy spawns it (that command is compiled out of release builds). Not replicated.
 */
UCLASS(NotBlueprintable, Transient)
class ATickProfilerTestActor : public AActor
{
	GENERATED_BODY()

public:
	ATickProfilerTestActor();

	virtual void Tick(float DeltaSeconds) override;

	float BusyMs = 0.0f;
};
