// Console commands. On a dedicated server they run from the Server Manager console or the HTTPS API's RunCommand,
// both of which the game restricts to administrators.

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "TPCollector.h"
#include "TPConfig.h"

#if TICKPROFILER_DEV_COMMANDS
#include "EngineUtils.h"
#include "FGPlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Player/SMLRemoteCallObject.h"
#include "TPTestActor.h"
#endif

using namespace TickProfiler;

namespace
{
	void Print(FOutputDevice& Ar, const TArray<FString>& Lines)
	{
		for (const FString& Line : Lines)
		{
			Ar.Log(Line);
		}
	}

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GLevelCommand(
		TEXT("TickProfiler.Level"),
		TEXT("TickProfiler.Level [0-3] [minutes] - shows or sets the capture level: 0 paused, 1 basic (phases), 2 detailed (+ per class, default), 3 extreme (+ per object). Without minutes it is saved; with minutes it runs that long, then the saved level applies again."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*, FOutputDevice& Ar)
		{
			TArray<FString> Lines;
			if (Args.Num() >= 2 && Args[0].IsNumeric() && Args[1].IsNumeric() && FCString::Atod(*Args[1]) > 0.0)
			{
				FCollector::Get().SetTemporaryLevel(FCString::Atoi(*Args[0]), FCString::Atod(*Args[1]), TEXT("console"));
				Lines.Add(FString::Printf(TEXT("Level %d for %s minute(s), then back to %d. Not saved."), GetLevel(), *Args[1], FCollector::Get().GetSettings().Level));
			}
			else if (Args.Num() >= 1)
			{
				Config::Set(TEXT("Level"), Args[0], TEXT("console"), Lines);
			}
			else
			{
				Lines.Add(FString::Printf(TEXT("Tick Profiler level %d"), GetLevel()));
			}
			Print(Ar, Lines);
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GSetCommand(
		TEXT("TickProfiler.Set"),
		TEXT("TickProfiler.Set <setting> <value> - changes a setting now and saves it to Configs/TickProfiler.cfg. TickProfiler.Settings lists them."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*, FOutputDevice& Ar)
		{
			TArray<FString> Lines;
			if (Args.Num() < 2)
			{
				Lines.Add(TEXT("Usage: TickProfiler.Set <setting> <value>"));
			}
			else
			{
				Config::Set(Args[0], Args[1], TEXT("console"), Lines);
			}
			Print(Ar, Lines);
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GSettingsCommand(
		TEXT("TickProfiler.Settings"),
		TEXT("TickProfiler.Settings - every setting, its value and where it came from (settings file, launch parameter or default)."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>&, UWorld*, FOutputDevice& Ar)
		{
			TArray<FString> Lines;
			Config::Describe(Lines);
			Print(Ar, Lines);
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GSummaryCommand(
		TEXT("TickProfiler.Summary"),
		TEXT("TickProfiler.Summary [top N classes] - the last summary window plus the last second."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*, FOutputDevice& Ar)
		{
			TArray<FString> Lines;
			FCollector::Get().DescribeSummary(Lines, Args.Num() >= 1 && Args[0].IsNumeric() ? FCString::Atoi(*Args[0]) : 10);
			Print(Ar, Lines);
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GStatusCommand(
		TEXT("TickProfiler.Status"),
		TEXT("TickProfiler.Status - level, data folder, disk use and which hooks are installed."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>&, UWorld*, FOutputDevice& Ar)
		{
			TArray<FString> Lines;
			FCollector::Get().DescribeStatus(Lines);
			Print(Ar, Lines);
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GHitchesCommand(
		TEXT("TickProfiler.Hitches"),
		TEXT("TickProfiler.Hitches [count] - the most recent hitches with their biggest phases and classes."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*, FOutputDevice& Ar)
		{
			TArray<FString> Lines;
			FCollector::Get().DescribeHitches(Lines, Args.Num() >= 1 && Args[0].IsNumeric() ? FCString::Atoi(*Args[0]) : 5);
			Print(Ar, Lines);
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GNetCommand(
		TEXT("TickProfiler.Net"),
		TEXT("TickProfiler.Net - per-connection ping, bandwidth, loss and saturation from the last second."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>&, UWorld*, FOutputDevice& Ar)
		{
			TArray<FString> Lines;
			FCollector::Get().DescribeNet(Lines);
			Print(Ar, Lines);
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GMarkCommand(
		TEXT("TickProfiler.Mark"),
		TEXT("TickProfiler.Mark <text> - writes a bookmark into every output, e.g. 'players report lag now'."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*, FOutputDevice& Ar)
		{
			const FString Text = FString::Join(Args, TEXT(" "));
			FCollector::Get().Mark(Text.IsEmpty() ? TEXT("(mark)") : Text, TEXT("console"));
			Ar.Log(TEXT("Marked."));
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GSnapshotCommand(
		TEXT("TickProfiler.Snapshot"),
		TEXT("TickProfiler.Snapshot - counts actors by class at the end of this frame (snapshots.jsonl)."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>&, UWorld*, FOutputDevice& Ar)
		{
			FCollector::Get().RequestSnapshot();
			Ar.Log(TEXT("Snapshot requested (needs level 2 or higher)."));
		}));

#if TICKPROFILER_DEV_COMMANDS
	FAutoConsoleCommandWithWorldArgsAndOutputDevice GDevBusyCommand(
		TEXT("TickProfiler.DevBusy"),
		TEXT("[dev] TickProfiler.DevBusy <ms per tick> [worker 0|1] [count] - spawns test actors that busy-wait every tick; 0 ms removes them."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
		{
			if (!World || Args.Num() < 1)
			{
				Ar.Log(TEXT("Usage: TickProfiler.DevBusy <ms per tick> [worker 0|1] [count]"));
				return;
			}
			for (TActorIterator<ATickProfilerTestActor> It(World); It; ++It)
			{
				It->Destroy();
			}
			const float Ms = FCString::Atof(*Args[0]);
			const bool bWorker = Args.Num() >= 2 && FCString::Atoi(*Args[1]) != 0;
			const int32 Count = Args.Num() >= 3 ? FMath::Clamp(FCString::Atoi(*Args[2]), 1, 1000) : 1;
			if (Ms <= 0.0f)
			{
				FCollector::Get().Mark(TEXT("dev: busy test actors removed"), TEXT("dev"));
				Ar.Log(TEXT("Test actors removed."));
				return;
			}
			for (int32 Index = 0; Index < Count; ++Index)
			{
				ATickProfilerTestActor* Actor = World->SpawnActorDeferred<ATickProfilerTestActor>(ATickProfilerTestActor::StaticClass(), FTransform::Identity);
				if (Actor)
				{
					Actor->BusyMs = Ms;
					Actor->PrimaryActorTick.bRunOnAnyThread = bWorker;
					Actor->FinishSpawning(FTransform::Identity);
				}
			}
			const FString Text = FString::Printf(TEXT("dev: %d busy test actor(s), %.2f ms per tick each, %s"), Count, Ms, bWorker ? TEXT("any thread") : TEXT("game thread"));
			FCollector::Get().Mark(Text, TEXT("dev"));
			Ar.Log(Text);
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GDevStallCommand(
		TEXT("TickProfiler.DevStall"),
		TEXT("[dev] TickProfiler.DevStall <ms> - busy-waits once on the game thread to force a hitch."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*, FOutputDevice& Ar)
		{
			const double Ms = Args.Num() >= 1 ? FMath::Clamp(FCString::Atod(*Args[0]), 1.0, 10000.0) : 200.0;
			FCollector::Get().Mark(FString::Printf(TEXT("dev: stalling the game thread for %.0f ms"), Ms), TEXT("dev"));
			const double End = FPlatformTime::Seconds() + Ms / 1000.0;
			while (FPlatformTime::Seconds() < End)
			{
			}
			Ar.Log(TEXT("Stalled."));
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GDevChatAsCommand(
		TEXT("TickProfiler.DevChatAs"),
		TEXT("[dev] TickProfiler.DevChatAs <player name> <chat command without the slash> - runs a chat command as that connected player, through the same server entry point SML uses for typed chat (permission checks included). Replies go to that player's chat and to LogChatCommand."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
		{
			if (!World || Args.Num() < 2)
			{
				Ar.Log(TEXT("Usage: TickProfiler.DevChatAs <player name> <command line>"));
				return;
			}
			TArray<FString> Rest = Args;
			Rest.RemoveAt(0);
			const FString CommandLine = FString::Join(Rest, TEXT(" "));
			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			{
				AFGPlayerController* Player = Cast<AFGPlayerController>(It->Get());
				if (!Player || !Player->PlayerState || !Player->PlayerState->GetPlayerName().Equals(Args[0], ESearchCase::IgnoreCase))
				{
					continue;
				}
				USMLRemoteCallObject* Remote = Cast<USMLRemoteCallObject>(Player->GetRemoteCallObjectOfClass(USMLRemoteCallObject::StaticClass()));
				if (!Remote)
				{
					Ar.Log(TEXT("That player has no SML remote call object."));
					return;
				}
				Remote->HandleChatCommand_Implementation(CommandLine);
				Ar.Log(FString::Printf(TEXT("Ran '/%s' as %s; the result is in LogChatCommand and that player's chat."), *CommandLine, *Args[0]));
				return;
			}
			Ar.Log(FString::Printf(TEXT("No connected player named '%s'."), *Args[0]));
		}));
#endif
}
