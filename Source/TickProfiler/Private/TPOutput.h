// Data files. Everything is queued by the game thread and written by one background thread, so the game thread
// never waits on the disk. Files are flushed every second, so a crash loses at most about a second of data.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include <atomic>

class FRunnableThread;
class IFileHandle;
class FEvent;

namespace TickProfiler
{
	enum class EFile : uint8
	{
		Frames,
		Classes,
		Objects,
		Hitches,
		Host,
		Net,
		Snapshots,
		Marks,
		Count
	};

	class FOutput : public FRunnable
	{
	public:
		static FOutput& Get();

		/** Creates the session folder and starts the writer thread. Game thread. */
		bool StartSession(const FString& Role, int64 DiskCapBytes, bool bSampleHost);
		/** Flushes everything queued and stops the thread. */
		void StopSession();
		bool IsActive() const { return bActive.load(); }

		/** Queues one line (without newline). Any thread. Dropped when no session is active. */
		void Write(EFile File, FString&& Line);
		/** Writes session.json (replaces it). Any thread. */
		void WriteSessionJson(FString&& Json);

		FString GetSessionDir() const { return SessionDir; }
		FString GetRootDir() const;
		int64 GetSessionBytes() const { return SessionBytes.load(); }
		bool IsDiskCapReached() const { return bDiskCapReached.load(); }
		/** Seconds since the session started (shared time base for every file). */
		double GetUptime() const;
		/** "2026-11-18T12:34:56.789Z,123.456" for the first two columns of every CSV. */
		FString TimeColumns() const;

		// FRunnable
		virtual uint32 Run() override;
		virtual void Stop() override;

	private:
		struct FFileState
		{
			FString Base;
			FString Extension;
			FString Header;
			IFileHandle* Handle = nullptr;
			int32 Part = 0;
			int64 PartBytes = 0;
		};

		struct FQueued
		{
			EFile File;
			FString Line;
		};

		void WriteLine(EFile File, const FString& Line);
		bool OpenPart(FFileState& State);
		void ClosePart(FFileState& State, bool bRememberForCap);
		void EnforceDiskCap();
		void ScanOldSessions();
		void DrainQueue();
		void CloseAll();

		TQueue<FQueued, EQueueMode::Mpsc> Queue;
		TQueue<FString, EQueueMode::Mpsc> SessionJsonQueue;
		FFileState Files[static_cast<int32>(EFile::Count)];
		/** Closed parts of this session, oldest first, with sizes (deleted first-in when the cap is hit). */
		TArray<TPair<FString, int64>> ClosedParts;
		/** Earlier sessions, oldest first, with sizes. */
		TArray<TPair<FString, int64>> OldSessions;
		int64 OldSessionsBytes = 0;

		FString SessionDir;
		int64 DiskCapBytes = 0;
		int64 MaxPartBytes = 0;
		double SessionStartSeconds = 0.0;
		FDateTime SessionStartUtc;
		bool bSampleHost = true;

		std::atomic<bool> bActive{false};
		std::atomic<bool> bStopRequested{false};
		std::atomic<bool> bDiskCapReached{false};
		std::atomic<int64> SessionBytes{0};
		FRunnableThread* Thread = nullptr;
		FEvent* WakeEvent = nullptr;
	};

	/** CSV header for each file kind (set by the collector before the session starts). */
	void SetFileHeader(EFile File, const FString& Header);
}
