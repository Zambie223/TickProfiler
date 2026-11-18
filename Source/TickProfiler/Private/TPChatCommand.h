#pragma once

#include "CoreMinimal.h"
#include "Command/ChatCommandInstance.h"
#include "Module/GameWorldModule.h"
#include "TPChatCommand.generated.h"

/**
 * /perf chat command. Runs on the server (SML executes chat commands there). On a dedicated server only
 * administrators may use it; in single player or on a listen server only the host. Not replicated, so players
 * without the mod never see an unknown actor.
 */
UCLASS(NotBlueprintable)
class ATickProfilerPerfCommand : public AChatCommandInstance
{
	GENERATED_BODY()

public:
	ATickProfilerPerfCommand();

	virtual EExecutionStatus ExecuteCommand_Implementation(UCommandSender* Sender, const TArray<FString>& Arguments, const FString& Label) override;

private:
	static bool IsAllowed(UCommandSender* Sender);
};

/** Root world module: registers the chat command. */
UCLASS()
class UTickProfilerWorldModule : public UGameWorldModule
{
	GENERATED_BODY()

public:
	UTickProfilerWorldModule();
};
