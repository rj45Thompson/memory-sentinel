// The findings schema in C++ (UC-2, UC-3.7, R10, R11). The same shape the Python side writes (memsentinel/findings.py):
// a findings file says what one detector checked, what it cannot see, and what it found, and every finding says how it is
// known. The panel and the visual report read both sides' files the same way.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct MEMORYSENTINEL_API FSentinelFinding
{
	FString Rule;
	FString Severity;      // error, warning, info
	FString Title;
	FString Why;
	FString Fix;
	FString HowKnown;
	TSharedPtr<FJsonObject> Where = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Evidence = MakeShared<FJsonObject>();
};

struct MEMORYSENTINEL_API FSentinelFindingsFile
{
	FString Kind;          // leaktest, heap
	bool bOk = true;
	FString Failure;
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Checked = MakeShared<FJsonObject>();
	TArray<FString> BlindSpots;
	TArray<FSentinelFinding> Findings;
	FString Made;          // ISO time; filled by Write when empty

	/** Writes the file; IdPrefix numbers the findings (LEAK-001...). Returns false and logs when the write fails. */
	bool Write(const FString& Path, const TCHAR* IdPrefix);
};

namespace MemorySentinelFiles
{
	/** <Project>/Saved/MemorySentinel, where every detector writes and the panel reads. */
	MEMORYSENTINEL_API FString Dir();
	/** The problem a rule belongs to: dangling, cycle, leak, stomp or crash (the same table as findings.py RULES). */
	MEMORYSENTINEL_API FString ProblemOf(const FString& Rule);
	/** A file name stamp: 20261110-142233. */
	MEMORYSENTINEL_API FString Stamp();
}
