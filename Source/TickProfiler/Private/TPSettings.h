// The user-facing settings: one table shared by the settings file (SML Mod Config), launch parameters, the /perf chat
// command and the console. Keys here must match Tools/create_config_asset.py.
#pragma once

#include "CoreMinimal.h"

namespace TickProfiler
{
	struct FSettings;

	enum class ESettingKind : uint8
	{
		Int,
		Float,
		Bool,
	};

	struct FSettingDef
	{
		/** Key in Configs/TickProfiler.cfg and for /perf set. */
		const TCHAR* Key;
		/** Launch parameter: "-<Param>=<value>" (a bool setting is switched off by "-<Param>"). */
		const TCHAR* LaunchParam;
		ESettingKind Kind;
		double Min;
		double Max;
		/** Only read when the data files start (the first world of a run). */
		bool bNeedsRestart;
		const TCHAR* Help;
	};

	enum class ESetting : uint8
	{
		Level,
		HitchMs,
		SummaryIntervalSec,
		ClassIntervalSec,
		SnapshotIntervalSec,
		TopN,
		MaxHitchLinesPerMinute,
		DiskCapMB,
		WriteFiles,
		Count
	};
	constexpr int32 NumSettings = static_cast<int32>(ESetting::Count);

	const FSettingDef& GetSettingDef(ESetting Setting);
	/** Case-insensitive key lookup; false if there is no such key. */
	bool FindSetting(const FString& Key, ESetting& OutSetting);

	double GetSettingValue(const FSettings& Settings, ESetting Setting);
	/** Stores the value clamped to the setting's range (and rounded for whole-number settings). */
	void SetSettingValue(FSettings& Settings, ESetting Setting, double Value);
	/** Value text as shown to people: "2", "0 (automatic)", "true". */
	FString FormatSettingValue(ESetting Setting, double Value);
	/** Parses a typed value ("3", "12.5", "true"/"on"/"1"); false if the text does not fit the setting's kind. */
	bool ParseSettingValue(ESetting Setting, const FString& Text, double& OutValue);
}
