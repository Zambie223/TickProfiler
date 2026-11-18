#include "TPSettings.h"

#include "TPCollector.h"

namespace TickProfiler
{
	namespace
	{
		// Defaults live in FSettings (TPCollector.h); ranges and help here.
		const FSettingDef GSettingDefs[NumSettings] = {
			{TEXT("Level"), TEXT("TickProfilerLevel"), ESettingKind::Int, 0, 3, false,
				TEXT("0 paused, 1 basic (frame stages), 2 detailed (+ which kinds of objects, default), 3 extreme (+ single objects; short captures)")},
			{TEXT("HitchMs"), TEXT("TickProfilerHitchMs"), ESettingKind::Float, 0, 60000, false,
				TEXT("frames slower than this many ms are hitches; 0 = automatic (2x the target frame time, at least 50 ms)")},
			{TEXT("SummaryIntervalSec"), TEXT("TickProfilerSummarySec"), ESettingKind::Int, 5, 3600, false,
				TEXT("seconds between summaries in FactoryGame.log")},
			{TEXT("ClassIntervalSec"), TEXT("TickProfilerClassSec"), ESettingKind::Int, 1, 600, false,
				TEXT("seconds per row group in classes.csv")},
			{TEXT("SnapshotIntervalSec"), TEXT("TickProfilerSnapshotSec"), ESettingKind::Int, 30, 86400, false,
				TEXT("seconds between object counts in snapshots.jsonl")},
			{TEXT("TopN"), TEXT("TickProfilerTopN"), ESettingKind::Int, 1, 500, false,
				TEXT("how many classes each table lists")},
			{TEXT("MaxHitchLinesPerMinute"), TEXT("TickProfilerHitchLines"), ESettingKind::Int, 0, 600, false,
				TEXT("hitch lines per minute in FactoryGame.log (hitches.jsonl always gets all of them)")},
			{TEXT("DiskCapMB"), TEXT("TickProfilerDiskCapMB"), ESettingKind::Int, 4, 1000000, true,
				TEXT("total size of Logs/TickProfiler; the oldest data is deleted first")},
			{TEXT("WriteFiles"), TEXT("TickProfilerNoFiles"), ESettingKind::Bool, 0, 1, true,
				TEXT("write the data files under Logs/TickProfiler (the log summaries are written either way)")},
		};
	}

	const FSettingDef& GetSettingDef(const ESetting Setting)
	{
		return GSettingDefs[static_cast<int32>(Setting)];
	}

	bool FindSetting(const FString& Key, ESetting& OutSetting)
	{
		for (int32 Index = 0; Index < NumSettings; ++Index)
		{
			if (Key.Equals(GSettingDefs[Index].Key, ESearchCase::IgnoreCase))
			{
				OutSetting = static_cast<ESetting>(Index);
				return true;
			}
		}
		return false;
	}

	double GetSettingValue(const FSettings& Settings, const ESetting Setting)
	{
		switch (Setting)
		{
		case ESetting::Level: return Settings.Level;
		case ESetting::HitchMs: return Settings.HitchMs;
		case ESetting::SummaryIntervalSec: return Settings.SummaryIntervalSec;
		case ESetting::ClassIntervalSec: return Settings.ClassIntervalSec;
		case ESetting::SnapshotIntervalSec: return Settings.SnapshotIntervalSec;
		case ESetting::TopN: return Settings.TopN;
		case ESetting::MaxHitchLinesPerMinute: return Settings.MaxHitchLogLinesPerMinute;
		case ESetting::DiskCapMB: return static_cast<double>(Settings.DiskCapMB);
		case ESetting::WriteFiles: return Settings.bWriteFiles ? 1.0 : 0.0;
		default: return 0.0;
		}
	}

	void SetSettingValue(FSettings& Settings, const ESetting Setting, double Value)
	{
		const FSettingDef& Def = GetSettingDef(Setting);
		Value = FMath::Clamp(Value, Def.Min, Def.Max);
		if (Def.Kind != ESettingKind::Float)
		{
			Value = FMath::RoundToDouble(Value);
		}
		switch (Setting)
		{
		case ESetting::Level: Settings.Level = static_cast<int32>(Value); break;
		case ESetting::HitchMs: Settings.HitchMs = Value; break;
		case ESetting::SummaryIntervalSec: Settings.SummaryIntervalSec = Value; break;
		case ESetting::ClassIntervalSec: Settings.ClassIntervalSec = Value; break;
		case ESetting::SnapshotIntervalSec: Settings.SnapshotIntervalSec = Value; break;
		case ESetting::TopN: Settings.TopN = static_cast<int32>(Value); break;
		case ESetting::MaxHitchLinesPerMinute: Settings.MaxHitchLogLinesPerMinute = static_cast<int32>(Value); break;
		case ESetting::DiskCapMB: Settings.DiskCapMB = static_cast<int64>(Value); break;
		case ESetting::WriteFiles: Settings.bWriteFiles = Value != 0.0; break;
		default: break;
		}
	}

	FString FormatSettingValue(const ESetting Setting, const double Value)
	{
		switch (GetSettingDef(Setting).Kind)
		{
		case ESettingKind::Bool:
			return Value != 0.0 ? TEXT("true") : TEXT("false");
		case ESettingKind::Float:
			if (Setting == ESetting::HitchMs && Value <= 0.0)
			{
				return TEXT("0 (automatic)");
			}
			return FString::SanitizeFloat(Value);
		default:
			return FString::Printf(TEXT("%lld"), static_cast<int64>(Value));
		}
	}

	bool ParseSettingValue(const ESetting Setting, const FString& Text, double& OutValue)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (GetSettingDef(Setting).Kind == ESettingKind::Bool)
		{
			if (Trimmed == TEXT("1") || Trimmed.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("on"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("yes"), ESearchCase::IgnoreCase))
			{
				OutValue = 1.0;
				return true;
			}
			if (Trimmed == TEXT("0") || Trimmed.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("off"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("no"), ESearchCase::IgnoreCase))
			{
				OutValue = 0.0;
				return true;
			}
			return false;
		}
		if (!Trimmed.IsNumeric())
		{
			return false;
		}
		OutValue = FCString::Atod(*Trimmed);
		return FMath::IsFinite(OutValue);
	}
}
