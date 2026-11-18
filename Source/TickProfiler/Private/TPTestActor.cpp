#include "TPTestActor.h"

ATickProfilerTestActor::ATickProfilerTestActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	bReplicates = false;
	SetCanBeDamaged(false);
}

void ATickProfilerTestActor::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const double End = FPlatformTime::Seconds() + BusyMs / 1000.0;
	while (FPlatformTime::Seconds() < End)
	{
	}
}
