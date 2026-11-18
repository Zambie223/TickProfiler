// The settings file, through SML's Mod Config: Configs/TickProfiler.cfg next to the game's FactoryGame folder, plus the
// in-game Mods menu wherever the mod runs with a user interface (single player, or a player's own copy).
// Every machine reads only its own file; nothing here is replicated.
#pragma once

#include "CoreMinimal.h"
#include "Module/GameInstanceModule.h"
#include "TPConfig.generated.h"

/** Root game instance module: registers the Mod Configuration and applies it to the collector. */
UCLASS()
class UTickProfilerGameInstanceModule : public UGameInstanceModule
{
	GENERATED_BODY()

public:
	UTickProfilerGameInstanceModule();

	virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;

private:
	UFUNCTION()
	void OnConfigValueChanged();
};

namespace TickProfiler::Config
{
	/**
	 * Changes a setting now and saves it to the settings file. A launch parameter for the same setting stops counting
	 * for the rest of this run (it applies again at the next start). False when refused (unknown key, bad value).
	 */
	bool Set(const FString& Key, const FString& ValueText, const FString& Source, TArray<FString>& OutLines);

	/** Every setting with its value and where it came from. */
	void Describe(TArray<FString>& OutLines);
}
