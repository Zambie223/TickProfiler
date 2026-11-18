#include "TPChatCommand.h"

#include "Command/CommandSender.h"
#include "Engine/World.h"
#include "FGPlayerController.h"
#include "Server/FGDSAuthenticationTypes.h"
#include "TPCollector.h"
#include "TPConfig.h"

using namespace TickProfiler;

namespace
{
	/** Chat is narrow; keep replies short and point at the files for the full picture. */
	constexpr int32 ChatTopClasses = 5;
	constexpr int32 ChatHitches = 3;

	void Reply(UCommandSender* Sender, const TArray<FString>& Lines, const FLinearColor Color = FLinearColor::Green)
	{
		for (const FString& Line : Lines)
		{
			Sender->SendChatMessage(Line, Color);
		}
	}
}

ATickProfilerPerfCommand::ATickProfilerPerfCommand()
{
	CommandName = TEXT("perf");
	Aliases.Add(TEXT("tickprofiler"));
	Usage = FText::FromString(TEXT("/perf [summary [N] | hitches [N] | net | status | settings | level <0-3> [minutes] | set <setting> <value> | mark <text> | snapshot]"));
	MinNumberOfArguments = 0;
	bOnlyUsableByPlayer = false;
	bReplicates = false;
}

bool ATickProfilerPerfCommand::IsAllowed(UCommandSender* Sender)
{
	if (!Sender->IsPlayerSender())
	{
		return true; // server console
	}
	const AFGPlayerController* Player = Sender->GetPlayer();
	if (!Player)
	{
		return false;
	}
	const UWorld* World = Player->GetWorld();
	if (World && World->GetNetMode() == NM_DedicatedServer)
	{
		return Player->GetDSPrivilegeLevel() == EPrivilegeLevel::Administrator;
	}
	return Player->IsLocalController(); // the host in single player or on a listen server
}

EExecutionStatus ATickProfilerPerfCommand::ExecuteCommand_Implementation(UCommandSender* Sender, const TArray<FString>& Arguments, const FString& Label)
{
	if (!IsAllowed(Sender))
	{
		Sender->SendChatMessage(TEXT("/perf is limited to server administrators (or the host)."), FLinearColor::Red);
		return EExecutionStatus::INSUFFICIENT_PERMISSIONS;
	}
	FCollector& Collector = FCollector::Get();
	const FString Sub = Arguments.Num() > 0 ? Arguments[0].ToLower() : TEXT("summary");
	const int32 Number = Arguments.Num() > 1 && Arguments[1].IsNumeric() ? FCString::Atoi(*Arguments[1]) : -1;
	TArray<FString> Lines;

	if (Sub == TEXT("summary"))
	{
		Collector.DescribeSummary(Lines, Number > 0 ? Number : ChatTopClasses);
	}
	else if (Sub == TEXT("hitches"))
	{
		Collector.DescribeHitches(Lines, Number > 0 ? Number : ChatHitches);
	}
	else if (Sub == TEXT("net"))
	{
		Collector.DescribeNet(Lines);
	}
	else if (Sub == TEXT("status"))
	{
		Collector.DescribeStatus(Lines);
	}
	else if (Sub == TEXT("level"))
	{
		if (Number < 0)
		{
			Lines.Add(FString::Printf(TEXT("Level %d (0 paused, 1 basic, 2 detailed, 3 extreme). /perf level <0-3> saves it; /perf level <0-3> <minutes> runs it for a while."), GetLevel()));
		}
		else if (Arguments.Num() > 2)
		{
			if (!Arguments[2].IsNumeric() || FCString::Atod(*Arguments[2]) <= 0.0)
			{
				Lines.Add(TEXT("Minutes must be a number above 0, e.g. /perf level 3 10"));
				Reply(Sender, Lines, FLinearColor::Red);
				return EExecutionStatus::BAD_ARGUMENTS;
			}
			Collector.SetTemporaryLevel(Number, FCString::Atod(*Arguments[2]), Sender->GetSenderName());
			Lines.Add(FString::Printf(TEXT("Level %d for %s minute(s), then back to %d. Not saved."), GetLevel(), *Arguments[2], Collector.GetSettings().Level));
		}
		else if (!Config::Set(TEXT("Level"), Arguments[1], Sender->GetSenderName(), Lines))
		{
			Reply(Sender, Lines, FLinearColor::Red);
			return EExecutionStatus::BAD_ARGUMENTS;
		}
	}
	else if (Sub == TEXT("set"))
	{
		if (Arguments.Num() < 3)
		{
			Lines.Add(TEXT("Usage: /perf set <setting> <value>. /perf settings lists them."));
			Reply(Sender, Lines, FLinearColor::Red);
			return EExecutionStatus::BAD_ARGUMENTS;
		}
		if (!Config::Set(Arguments[1], Arguments[2], Sender->GetSenderName(), Lines))
		{
			Reply(Sender, Lines, FLinearColor::Red);
			return EExecutionStatus::BAD_ARGUMENTS;
		}
	}
	else if (Sub == TEXT("settings"))
	{
		Config::Describe(Lines);
	}
	else if (Sub == TEXT("mark"))
	{
		TArray<FString> Words = Arguments;
		Words.RemoveAt(0);
		const FString Text = FString::Join(Words, TEXT(" "));
		Collector.Mark(Text.IsEmpty() ? TEXT("(mark)") : Text, Sender->GetSenderName());
		Lines.Add(TEXT("Marked."));
	}
	else if (Sub == TEXT("snapshot"))
	{
		Collector.RequestSnapshot();
		Lines.Add(TEXT("Snapshot requested; it is written to snapshots.jsonl (needs level 2 or higher)."));
	}
	else
	{
		PrintCommandUsage(Sender);
		return EExecutionStatus::BAD_ARGUMENTS;
	}
	Reply(Sender, Lines);
	return EExecutionStatus::COMPLETED;
}

UTickProfilerWorldModule::UTickProfilerWorldModule()
{
	bRootModule = true;
	mChatCommands.Add(ATickProfilerPerfCommand::StaticClass());
}
