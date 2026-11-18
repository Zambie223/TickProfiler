#include "TickProfilerModule.h"

#include "TPCollector.h"
#include "TPHooks.h"

void FTickProfilerModule::StartupModule()
{
#if !WITH_EDITOR
	// The editor (used only to build the mod's assets) is not profiled; hooks there would only add risk.
	TickProfiler::FCollector::Get().Startup();
	FTickProfilerHooks::Install();
#endif
}

void FTickProfilerModule::ShutdownModule()
{
#if !WITH_EDITOR
	TickProfiler::FCollector::Get().Shutdown();
#endif
}

IMPLEMENT_MODULE(FTickProfilerModule, TickProfiler)
