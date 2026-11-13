// Copyright (c) 2026 RJ Thompson. Released under the MIT License; see LICENSE.
// The panel (UC-4.1, 4.2): every findings file under Saved/MemorySentinel, newest first, filterable by problem; the
// selected finding's why, fix and how it is known; and a diagram of its evidence. The editor docks it as a tab; a running
// game shows it as an overlay with `ms.Show`. Its buttons run the static pass, a leak test and the visual report.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SSentinelDiagram;
class SEditableTextBox;
class SVerticalBox;
class SHorizontalBox;
class ITableRow;
class STableViewBase;

/** One finding as the panel shows it, read from a findings file. */
struct FSentinelRow
{
	FString File;          // the findings file it came from
	FString Kind;          // scan, leaktest, heap
	FString Id;
	FString Rule;
	FString Problem;
	FString Severity;
	FString Title;
	FString Why;
	FString Fix;
	FString HowKnown;
	FString WhereText;     // file:line or an object path
	FString SourceFile;    // set when the finding points at a source line
	int32 Line = 0;
	FString Suppressed;
	TSharedPtr<FJsonObject> Evidence;
	TSharedPtr<FJsonObject> Where;
	bool bFileProblem = false;   // the file itself could not be read, or its detector failed
};

class MEMORYSENTINEL_API SSentinelPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSentinelPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SSentinelPanel() override;

	/** Read every findings file again. */
	void Reload();
	/** Select the first finding of a rule (or of any rule when empty). Returns false when there is none. */
	bool SelectFirst(const FString& Rule);

	/** Opens a source file at a line. The editor module sets it to the IDE; a running game has none. */
	static TFunction<void(const FString&, int32)> OpenSourceHook;

private:
	TSharedRef<ITableRow> MakeRow(TSharedPtr<FSentinelRow> Row, const TSharedRef<STableViewBase>& Owner);
	void OnSelect(TSharedPtr<FSentinelRow> Row, ESelectInfo::Type);
	void OnOpen(TSharedPtr<FSentinelRow> Row);
	void ApplyFilter();
	void RebuildProblemChips();
	EActiveTimerReturnType Poll(double Now, float Delta);
	FText StatusText() const;
	FText SummaryText() const;
	void RunScan();
	void RunLeakTest();
	void RunReport();
	bool RunTool(const TArray<FString>& Args, const FString& What, TFunction<void(int32, const FString&)> Done);

	TArray<TSharedPtr<FSentinelRow>> All;
	TArray<TSharedPtr<FSentinelRow>> Visible;
	TSharedPtr<FSentinelRow> Selected;
	TSharedPtr<SListView<TSharedPtr<FSentinelRow>>> List;
	TSharedPtr<SSentinelDiagram> Diagram;
	TSharedPtr<SHorizontalBox> ProblemChips;
	TSharedPtr<SEditableTextBox> MapBox;
	FString ProblemFilter;
	int32 NumFiles = 0;
	FDateTime NewestFile;
	FString Busy;
	FString LastMessage;

	struct FJob
	{
		FProcHandle Proc;
		void* ReadPipe = nullptr;
		void* WritePipe = nullptr;
		FString Output;
		FString What;
		TFunction<void(int32, const FString&)> Done;
	};
	TArray<TSharedPtr<FJob>> Jobs;
};
