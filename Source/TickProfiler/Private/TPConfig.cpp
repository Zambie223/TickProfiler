#include "TPConfig.h"

#include "Configuration/ConfigManager.h"
#include "Configuration/ModConfiguration.h"
#include "Configuration/Properties/ConfigPropertyBool.h"
#include "Configuration/Properties/ConfigPropertyFloat.h"
#include "Configuration/Properties/ConfigPropertyInteger.h"
#include "Configuration/Properties/ConfigPropertySection.h"
#include "Engine/GameInstance.h"
#include "TPCollector.h"
#include "TPSettings.h"

using namespace TickProfiler;

namespace
{
	const TCHAR* const ModReference = TEXT("TickProfiler");
	/** The Blueprint made by Tools/create_config_asset.py. */
	const TCHAR* const ConfigClassPath = TEXT("/TickProfiler/TickProfilerConfiguration.TickProfilerConfiguration_C");

	TWeakObjectPtr<UConfigManager> GConfigManager;
	TWeakObjectPtr<UConfigPropertySection> GRootSection;
	/** Who is changing a setting through Config::Set; the change event it fires is applied under this name. */
	FString GSetSource;

	FConfigId GetConfigId()
	{
		FConfigId Id;
		Id.ModReference = ModReference;
		return Id;
	}

	UConfigProperty* FindProperty(UConfigPropertySection* Root, const ESetting Setting)
	{
		const TObjectPtr<UConfigProperty>* Found = Root ? Root->SectionProperties.Find(FString(GetSettingDef(Setting).Key)) : nullptr;
		return Found ? Found->Get() : nullptr;
	}

	/** The value stored in the settings file; false if the key is missing or of another type. */
	bool ReadProperty(UConfigPropertySection* Root, const ESetting Setting, double& OutValue)
	{
		UConfigProperty* Property = FindProperty(Root, Setting);
		switch (GetSettingDef(Setting).Kind)
		{
		case ESettingKind::Int:
			if (const UConfigPropertyInteger* Int = Cast<UConfigPropertyInteger>(Property))
			{
				OutValue = Int->Value;
				return true;
			}
			return false;
		case ESettingKind::Float:
			if (const UConfigPropertyFloat* Float = Cast<UConfigPropertyFloat>(Property))
			{
				OutValue = Float->Value;
				return true;
			}
			return false;
		case ESettingKind::Bool:
			if (const UConfigPropertyBool* Bool = Cast<UConfigPropertyBool>(Property))
			{
				OutValue = Bool->Value ? 1.0 : 0.0;
				return true;
			}
			return false;
		default:
			return false;
		}
	}

	bool WriteProperty(UConfigPropertySection* Root, const ESetting Setting, const double Value)
	{
		UConfigProperty* Property = FindProperty(Root, Setting);
		switch (GetSettingDef(Setting).Kind)
		{
		case ESettingKind::Int:
			if (UConfigPropertyInteger* Int = Cast<UConfigPropertyInteger>(Property))
			{
				Int->Value = static_cast<int32>(Value);
				break;
			}
			return false;
		case ESettingKind::Float:
			if (UConfigPropertyFloat* Float = Cast<UConfigPropertyFloat>(Property))
			{
				Float->Value = static_cast<float>(Value);
				break;
			}
			return false;
		case ESettingKind::Bool:
			if (UConfigPropertyBool* Bool = Cast<UConfigPropertyBool>(Property))
			{
				Bool->Value = Value != 0.0;
				break;
			}
			return false;
		default:
			return false;
		}
		// MarkDirty queues the save and fires the section's change event, which re-applies the whole file.
		Property->MarkDirty();
		return true;
	}

	/** Reads the whole file and hands it to the collector (which skips settings fixed by a launch parameter). */
	void ApplyFromFile(const FString& Source)
	{
		UConfigPropertySection* Root = GRootSection.Get();
		if (!Root)
		{
			return;
		}
		FCollector& Collector = FCollector::Get();
		FSettings Wanted = Collector.GetSettings();
		for (int32 Index = 0; Index < NumSettings; ++Index)
		{
			const ESetting Setting = static_cast<ESetting>(Index);
			double Value = 0.0;
			if (!ReadProperty(Root, Setting, Value))
			{
				UE_LOG(LogTickProfiler, Warning, TEXT("Settings file has no usable '%s'; keeping %s"), GetSettingDef(Setting).Key,
					*FormatSettingValue(Setting, GetSettingValue(Wanted, Setting)));
				continue;
			}
			const FSettingDef& Def = GetSettingDef(Setting);
			if (Value < Def.Min || Value > Def.Max)
			{
				UE_LOG(LogTickProfiler, Warning, TEXT("Settings file: %s = %s is outside %s..%s; using the nearest allowed value"), Def.Key,
					*FString::SanitizeFloat(Value), *FormatSettingValue(Setting, Def.Min), *FormatSettingValue(Setting, Def.Max));
			}
			SetSettingValue(Wanted, Setting, Value);
		}
		Collector.ApplySettings(Wanted, Source);
	}
}

UTickProfilerGameInstanceModule::UTickProfilerGameInstanceModule()
{
	bRootModule = true;
}

void UTickProfilerGameInstanceModule::DispatchLifecycleEvent(const ELifecyclePhase Phase)
{
	// SML registers (and loads) configurations during INITIALIZATION, so ours is added during CONSTRUCTION.
	// Loaded here instead of in the constructor so the mod's content is mounted.
	if (Phase == ELifecyclePhase::CONSTRUCTION)
	{
		const TSoftClassPtr<UModConfiguration> ConfigClass{FSoftObjectPath(ConfigClassPath)};
		if (UClass* Loaded = ConfigClass.LoadSynchronous())
		{
			ModConfigurations.AddUnique(Loaded);
		}
		else
		{
			UE_LOG(LogTickProfiler, Error, TEXT("Could not load the Mod Configuration %s; using defaults and launch parameters only"), ConfigClassPath);
		}
	}
	Super::DispatchLifecycleEvent(Phase);

	if (Phase == ELifecyclePhase::POST_INITIALIZATION)
	{
		UGameInstance* GameInstance = GetGameInstance();
		UConfigManager* Manager = GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
		UConfigPropertySection* Root = Manager ? Manager->GetConfigurationRootSection(GetConfigId()) : nullptr;
		if (!Root)
		{
			UE_LOG(LogTickProfiler, Warning, TEXT("Settings file not registered; using defaults and launch parameters only"));
			return;
		}
		GConfigManager = Manager;
		GRootSection = Root;
		Root->OnPropertyValueChanged.AddUniqueDynamic(this, &UTickProfilerGameInstanceModule::OnConfigValueChanged);
		ApplyFromFile(TEXT("settings file"));
		UE_LOG(LogTickProfiler, Display, TEXT("Settings file: %s"), *(UConfigManager::GetConfigurationFolderPath() + ModReference + TEXT(".cfg")));
	}
}

void UTickProfilerGameInstanceModule::OnConfigValueChanged()
{
	// A change made in the Mods menu has no source of its own.
	ApplyFromFile(GSetSource.IsEmpty() ? FString(TEXT("Mods menu")) : GSetSource);
}

namespace TickProfiler::Config
{
	bool Set(const FString& Key, const FString& ValueText, const FString& Source, TArray<FString>& OutLines)
	{
		ESetting Setting = ESetting::Level;
		if (!FindSetting(Key, Setting))
		{
			FString Keys;
			for (int32 Index = 0; Index < NumSettings; ++Index)
			{
				Keys += FString::Printf(TEXT("%s%s"), Index ? TEXT(", ") : TEXT(""), GetSettingDef(static_cast<ESetting>(Index)).Key);
			}
			OutLines.Add(FString::Printf(TEXT("Unknown setting '%s'. Settings: %s"), *Key, *Keys));
			return false;
		}
		const FSettingDef& Def = GetSettingDef(Setting);
		double Value = 0.0;
		if (!ParseSettingValue(Setting, ValueText, Value))
		{
			OutLines.Add(FString::Printf(TEXT("%s needs %s."), Def.Key, Def.Kind == ESettingKind::Bool ? TEXT("true or false") : TEXT("a number")));
			return false;
		}
		FSettings Clamped;
		SetSettingValue(Clamped, Setting, Value);
		Value = GetSettingValue(Clamped, Setting);

		FCollector& Collector = FCollector::Get();
		const bool bHadLaunchParam = Collector.IsLaunchOverride(Setting);
		Collector.ClearLaunchOverride(Setting);
		if (Setting == ESetting::Level)
		{
			Collector.CancelTemporaryLevel(Source);
		}

		UConfigPropertySection* Root = GRootSection.Get();
		GSetSource = Source;
		const bool bSaved = Root && WriteProperty(Root, Setting, Value);
		GSetSource.Reset();
		if (bSaved)
		{
			if (UConfigManager* Manager = GConfigManager.Get())
			{
				Manager->FlushPendingSaves();
			}
		}
		// Applied directly as well: covers a missing settings file, and is a no-op when the change event already applied it.
		FSettings Wanted = Collector.GetSettings();
		SetSettingValue(Wanted, Setting, Value);
		Collector.ApplySettings(Wanted, Source);

		const bool bLater = Def.bNeedsRestart && Collector.IsSessionStarted();
		OutLines.Add(FString::Printf(TEXT("%s = %s%s%s"), Def.Key, *FormatSettingValue(Setting, Value),
			bLater ? TEXT(", applies at the next start") : TEXT(", applied now"),
			bSaved ? TEXT(", saved to the settings file.") : TEXT(" (NOT saved: the settings file is not available, so this lasts until restart).")));
		if (bHadLaunchParam)
		{
			OutLines.Add(FString::Printf(TEXT("Note: the launch parameter -%s still sets it again at the next start."), Def.LaunchParam));
		}
		return true;
	}

	void Describe(TArray<FString>& OutLines)
	{
		const FCollector& Collector = FCollector::Get();
		UConfigPropertySection* Root = GRootSection.Get();
		OutLines.Add(Root
			? FString::Printf(TEXT("Settings (file FactoryGame/Configs/%s.cfg; change with /perf set <name> <value>):"), ModReference)
			: FString(TEXT("Settings (settings file not available: defaults and launch parameters only):")));
		for (int32 Index = 0; Index < NumSettings; ++Index)
		{
			const ESetting Setting = static_cast<ESetting>(Index);
			const FSettingDef& Def = GetSettingDef(Setting);
			const double Value = GetSettingValue(Collector.GetSettings(), Setting);
			FString Origin = Collector.IsLaunchOverride(Setting) ? FString::Printf(TEXT("launch parameter -%s"), Def.LaunchParam) : FString(Root ? TEXT("file") : TEXT("default"));
			double FileValue = 0.0;
			if (Def.bNeedsRestart && Root && ReadProperty(Root, Setting, FileValue) && !Collector.IsLaunchOverride(Setting))
			{
				FSettings Clamped;
				SetSettingValue(Clamped, Setting, FileValue);
				if (GetSettingValue(Clamped, Setting) != Value)
				{
					Origin += FString::Printf(TEXT("; %s at the next start"), *FormatSettingValue(Setting, GetSettingValue(Clamped, Setting)));
				}
			}
			OutLines.Add(FString::Printf(TEXT("  %s = %s (%s) - %s"), Def.Key, *FormatSettingValue(Setting, Value), *Origin, Def.Help));
		}
		int32 TempLevel = 0;
		double RemainingSec = 0.0;
		if (Collector.GetTemporaryLevel(TempLevel, RemainingSec))
		{
			OutLines.Add(FString::Printf(TEXT("  Timed capture: level %d for %.0f more minute(s), then back to %d."), TempLevel, FMath::CeilToDouble(RemainingSec / 60.0), Collector.GetSettings().Level));
		}
	}
}
