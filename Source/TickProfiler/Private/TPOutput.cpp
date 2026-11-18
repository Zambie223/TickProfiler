#include "TPOutput.h"

#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/RunnableThread.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "TPCore.h"
#include "TPHost.h"

namespace TickProfiler
{
	namespace
	{
		FString GHeaders[static_cast<int32>(EFile::Count)];

		/** A path for the log, relative to the Saved folder: in single player the full path contains the Windows user
		 *  name, and people post their logs when asking for help. */
		FString PathForLog(const FString& Path)
		{
			FString Relative = FPaths::ConvertRelativePathToFull(Path);
			const FString Saved = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
			if (FPaths::MakePathRelativeTo(Relative, *Saved) && !Relative.StartsWith(TEXT("..")))
			{
				return TEXT("Saved/") + Relative;
			}
			return FPaths::GetCleanFilename(Path);
		}

		struct FFileInfo
		{
			const TCHAR* Base;
			const TCHAR* Extension;
		};

		FFileInfo GetFileInfo(const EFile File)
		{
			switch (File)
			{
			case EFile::Frames: return {TEXT("frames"), TEXT("csv")};
			case EFile::Classes: return {TEXT("classes"), TEXT("csv")};
			case EFile::Objects: return {TEXT("objects"), TEXT("csv")};
			case EFile::Hitches: return {TEXT("hitches"), TEXT("jsonl")};
			case EFile::Host: return {TEXT("host"), TEXT("csv")};
			case EFile::Net: return {TEXT("net"), TEXT("csv")};
			case EFile::Snapshots: return {TEXT("snapshots"), TEXT("jsonl")};
			case EFile::Marks: return {TEXT("marks"), TEXT("csv")};
			default: return {TEXT("other"), TEXT("txt")};
			}
		}

		int64 GetDirectorySize(const FString& Dir)
		{
			int64 Total = 0;
			IFileManager::Get().IterateDirectoryStatRecursively(*Dir, [&Total](const TCHAR*, const FFileStatData& Stat)
			{
				if (!Stat.bIsDirectory && Stat.FileSize > 0)
				{
					Total += Stat.FileSize;
				}
				return true;
			});
			return Total;
		}
	}

	void SetFileHeader(const EFile File, const FString& Header)
	{
		GHeaders[static_cast<int32>(File)] = Header;
	}

	FOutput& FOutput::Get()
	{
		static FOutput Instance;
		return Instance;
	}

	FString FOutput::GetRootDir() const
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir() / TEXT("TickProfiler"));
	}

	double FOutput::GetUptime() const
	{
		return FPlatformTime::Seconds() - SessionStartSeconds;
	}

	FString FOutput::TimeColumns() const
	{
		return FString::Printf(TEXT("%s,%.3f"), *FDateTime::UtcNow().ToIso8601(), GetUptime());
	}

	bool FOutput::StartSession(const FString& Role, const int64 InDiskCapBytes, const bool bInSampleHost)
	{
		check(IsInGameThread());
		if (bActive.load())
		{
			return true;
		}
		DiskCapBytes = FMath::Max<int64>(InDiskCapBytes, 4 * 1024 * 1024);
		MaxPartBytes = FMath::Clamp<int64>(DiskCapBytes / 20, 1024 * 1024, 64 * 1024 * 1024);
		bSampleHost = bInSampleHost;
		SessionStartSeconds = FPlatformTime::Seconds();
		SessionStartUtc = FDateTime::UtcNow();
		SessionDir = GetRootDir() / (SessionStartUtc.ToString(TEXT("%Y-%m-%d_%H-%M-%S_")) + Role);
		if (!IFileManager::Get().MakeDirectory(*SessionDir, true))
		{
			UE_LOG(LogTickProfiler, Error, TEXT("Could not create %s; data files are off (log summaries still work)"), *PathForLog(SessionDir));
			return false;
		}
		SetFileHeader(EFile::Host, FHostSampler::Header());
		for (int32 Index = 0; Index < static_cast<int32>(EFile::Count); ++Index)
		{
			const FFileInfo Info = GetFileInfo(static_cast<EFile>(Index));
			Files[Index] = FFileState();
			Files[Index].Base = Info.Base;
			Files[Index].Extension = Info.Extension;
			Files[Index].Header = GHeaders[Index];
		}
		ClosedParts.Reset();
		SessionBytes = 0;
		bDiskCapReached = false;
		ScanOldSessions();

		bStopRequested = false;
		WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
		bActive = true;
		Thread = FRunnableThread::Create(this, TEXT("TickProfilerWriter"), 0, TPri_BelowNormal);
		if (!Thread)
		{
			bActive = false;
			FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
			WakeEvent = nullptr;
			UE_LOG(LogTickProfiler, Error, TEXT("Could not start the writer thread; data files are off"));
			return false;
		}
		UE_LOG(LogTickProfiler, Display, TEXT("Writing data files to %s (disk cap %lld MB, earlier sessions %lld MB)"),
			*PathForLog(SessionDir), DiskCapBytes / (1024 * 1024), OldSessionsBytes / (1024 * 1024));
		return true;
	}

	void FOutput::StopSession()
	{
		if (!bActive.load() || !Thread)
		{
			return;
		}
		bStopRequested = true;
		WakeEvent->Trigger();
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
		bActive = false;
	}

	void FOutput::Write(const EFile File, FString&& Line)
	{
		if (bActive.load(std::memory_order_relaxed))
		{
			Queue.Enqueue(FQueued{File, MoveTemp(Line)});
		}
	}

	void FOutput::WriteSessionJson(FString&& Json)
	{
		if (bActive.load(std::memory_order_relaxed))
		{
			SessionJsonQueue.Enqueue(MoveTemp(Json));
		}
	}

	uint32 FOutput::Run()
	{
		FHostSampler Host;
		double NextHost = 0.0;
		double NextFlush = 0.0;
		while (!bStopRequested.load())
		{
			DrainQueue();
			const double Now = FPlatformTime::Seconds();
			if (bSampleHost && Now >= NextHost)
			{
				FString Row = Host.Sample(TimeColumns());
				if (!Row.IsEmpty())
				{
					WriteLine(EFile::Host, Row);
				}
				NextHost = Now + 1.0;
			}
			if (Now >= NextFlush)
			{
				for (FFileState& State : Files)
				{
					if (State.Handle)
					{
						State.Handle->Flush();
					}
				}
				NextFlush = Now + 1.0;
			}
			WakeEvent->Wait(200);
		}
		DrainQueue();
		CloseAll();
		return 0;
	}

	void FOutput::Stop()
	{
		bStopRequested = true;
		if (WakeEvent)
		{
			WakeEvent->Trigger();
		}
	}

	void FOutput::DrainQueue()
	{
		FString Json;
		while (SessionJsonQueue.Dequeue(Json))
		{
			const FString Path = SessionDir / TEXT("session.json");
			FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
		FQueued Item;
		while (Queue.Dequeue(Item))
		{
			WriteLine(Item.File, Item.Line);
		}
	}

	void FOutput::WriteLine(const EFile File, const FString& Line)
	{
		if (bDiskCapReached.load(std::memory_order_relaxed))
		{
			return;
		}
		FFileState& State = Files[static_cast<int32>(File)];
		const FTCHARToUTF8 Utf8(*Line);
		const int64 Bytes = Utf8.Length() + 1;
		if (State.Handle && State.PartBytes + Bytes > MaxPartBytes)
		{
			ClosePart(State, true);
			++State.Part;
		}
		if (!State.Handle && !OpenPart(State))
		{
			return;
		}
		State.Handle->Write(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		static const uint8 Newline = '\n';
		State.Handle->Write(&Newline, 1);
		State.PartBytes += Bytes;
		SessionBytes += Bytes;
		EnforceDiskCap();
	}

	bool FOutput::OpenPart(FFileState& State)
	{
		const FString Path = SessionDir / FString::Printf(TEXT("%s.%03d.%s"), *State.Base, State.Part, *State.Extension);
		State.Handle = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Path, false, true);
		State.PartBytes = 0;
		if (!State.Handle)
		{
			UE_LOG(LogTickProfiler, Warning, TEXT("Could not open %s"), *PathForLog(Path));
			return false;
		}
		if (!State.Header.IsEmpty())
		{
			const FTCHARToUTF8 Utf8(*(State.Header + TEXT("\n")));
			State.Handle->Write(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			State.PartBytes += Utf8.Length();
			SessionBytes += Utf8.Length();
		}
		return true;
	}

	void FOutput::ClosePart(FFileState& State, const bool bRememberForCap)
	{
		if (!State.Handle)
		{
			return;
		}
		State.Handle->Flush();
		delete State.Handle;
		State.Handle = nullptr;
		if (bRememberForCap)
		{
			const FString Path = SessionDir / FString::Printf(TEXT("%s.%03d.%s"), *State.Base, State.Part, *State.Extension);
			ClosedParts.Emplace(Path, State.PartBytes);
		}
	}

	void FOutput::CloseAll()
	{
		for (FFileState& State : Files)
		{
			ClosePart(State, false);
		}
	}

	void FOutput::ScanOldSessions()
	{
		OldSessions.Reset();
		OldSessionsBytes = 0;
		TArray<FString> Dirs;
		IFileManager::Get().FindFiles(Dirs, *(GetRootDir() / TEXT("*")), false, true);
		Dirs.Sort();
		for (const FString& Name : Dirs)
		{
			const FString Dir = GetRootDir() / Name;
			if (Dir == SessionDir)
			{
				continue;
			}
			const int64 Size = GetDirectorySize(Dir);
			OldSessions.Emplace(Dir, Size);
			OldSessionsBytes += Size;
		}
		EnforceDiskCap();
	}

	void FOutput::EnforceDiskCap()
	{
		while (OldSessionsBytes + SessionBytes.load() > DiskCapBytes)
		{
			if (OldSessions.Num() > 0)
			{
				const TPair<FString, int64> Oldest = OldSessions[0];
				OldSessions.RemoveAt(0);
				OldSessionsBytes -= Oldest.Value;
				IFileManager::Get().DeleteDirectory(*Oldest.Key, false, true);
				UE_LOG(LogTickProfiler, Display, TEXT("Disk cap: deleted the oldest session %s"), *FPaths::GetCleanFilename(Oldest.Key));
			}
			else if (ClosedParts.Num() > 0)
			{
				const TPair<FString, int64> Oldest = ClosedParts[0];
				ClosedParts.RemoveAt(0);
				SessionBytes -= Oldest.Value;
				IFileManager::Get().Delete(*Oldest.Key, false, true, true);
				UE_LOG(LogTickProfiler, Display, TEXT("Disk cap: deleted the oldest part %s"), *FPaths::GetCleanFilename(Oldest.Key));
			}
			else
			{
				if (!bDiskCapReached.exchange(true))
				{
					UE_LOG(LogTickProfiler, Warning, TEXT("Disk cap of %lld MB reached and nothing older is left to delete; data files stop here (log summaries continue)"), DiskCapBytes / (1024 * 1024));
				}
				return;
			}
		}
	}
}
