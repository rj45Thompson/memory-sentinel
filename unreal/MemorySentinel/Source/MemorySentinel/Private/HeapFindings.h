// Copyright (c) 2026 RJ Thompson. Released under the MIT License; see LICENSE.
// The heap findings writer (UC-3.4, 3.5, 3.7). Every frame it gives the sampler a check budget, asks the rotor for a new set
// of watch words every few frames, drains the sampler's events, pairs a watched write with the canary or quarantine failure
// of the same block, turns program counters into function, file and line, and writes a findings file when something new
// arrives and at exit.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"
#include "SentinelHeap.h"

class FHeapFindings
{
public:
	static void Start();
	static void Stop();
	/** Write now (the console command, the exit path and the fault handler use this). */
	static void WriteNow();
	static FString FilePath();

private:
	bool Tick(float DeltaTime);
	void Absorb(const TArray<MemorySentinel::FHeapEvent>& Events);
	void Write();
	FString Symbol(uint64 Pc);
	TArray<TSharedPtr<FJsonValue>> StackJson(const MemorySentinel::FHeapStack& Stack);
	bool FirstUserFrame(const MemorySentinel::FHeapStack& Stack, FString& OutFile, int32& OutLine, FString& OutFunction);

	struct FBlockRecord
	{
		MemorySentinel::FHeapEvent First;          // the canary, quarantine or guard event, or the write when it came first
		bool bHasCheck = false;
		bool bHasWrite = false;
		MemorySentinel::FHeapEvent Write;
	};
	TMap<uint64, FBlockRecord> Blocks;               // by block address
	TArray<uint64> Order;
	FTSTicker::FDelegateHandle TickHandle;
	uint64 Frame = 0;
	int32 MaxWatches = 0;      // the most words the data breakpoints watched at once, over the run
	bool bDirty = false;
	double LastWrite = 0.0;
	double Started = 0.0;
	FString Path;
	TMap<uint64, FString> Symbols;
	FCriticalSection WriteLock;
};
