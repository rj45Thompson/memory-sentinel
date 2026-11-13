// Copyright (c) 2026 RJ Thompson. Released under the MIT License; see LICENSE.
#include "SSentinelPanel.h"

#include "LevelLeakTest.h"
#include "SentinelFindings.h"
#include "SentinelHeap.h"
#include "SentinelStyle.h"
#include "SSentinelDiagram.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

TFunction<void(const FString&, int32)> SSentinelPanel::OpenSourceHook;

namespace
{
	TSharedRef<SWidget> Label(const FText& Text, const FSlateFontInfo& Font, const FLinearColor& Color)
	{
		return SNew(STextBlock).Text(Text).Font(Font).ColorAndOpacity(Color).AutoWrapText(true);
	}

	TSharedRef<SWidget> ActionButton(const FString& Text, const FString& Tip, FOnClicked OnClicked, bool bPrimary = false)
	{
		return SNew(SButton)
			.ButtonColorAndOpacity(bPrimary ? SentinelStyle::Accent().CopyWithNewOpacity(0.9f) : SentinelStyle::Card())
			.ContentPadding(FMargin(12, 5))
			.ToolTipText(FText::FromString(Tip))
			.OnClicked(OnClicked)
			[
				SNew(STextBlock).Text(FText::FromString(Text)).Font(SentinelStyle::Font(true, 10))
					.ColorAndOpacity(SentinelStyle::Ink())
			];
	}

	FString DefaultMap()
	{
		if (GEngine)
		{
			for (const FWorldContext& C : GEngine->GetWorldContexts())
			{
				const UWorld* W = C.World();
				if (W && (C.WorldType == EWorldType::Editor || C.WorldType == EWorldType::Game || C.WorldType == EWorldType::PIE))
				{
					const FString P = W->GetOutermost()->GetName();
					if (!P.StartsWith(TEXT("/Temp/")) && P != TEXT("/Engine/Maps/Entry")) { return P; }
				}
			}
		}
		return TEXT("/Engine/Maps/Templates/Template_Default");
	}

	/** Saved/MemorySentinel rather than the whole path: the project is known, its folder is not news. */
	FString ProjectRelative(FString Path)
	{
		FPaths::MakePathRelativeTo(Path, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		return Path;
	}

	bool HasGameWorld()
	{
		if (!GEngine) { return false; }
		for (const FWorldContext& C : GEngine->GetWorldContexts())
		{
			if ((C.WorldType == EWorldType::Game || C.WorldType == EWorldType::PIE) && C.World()) { return true; }
		}
		return false;
	}
}

void SSentinelPanel::Construct(const FArguments&)
{
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(SentinelStyle::Page())
		.Padding(16)
		[
			SNew(SVerticalBox)
			// title, summary and actions
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).Text(FText::FromString(TEXT("Memory Sentinel"))).Font(SentinelStyle::Font(true, 18)).ColorAndOpacity(SentinelStyle::Ink())
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).Text(this, &SSentinelPanel::SummaryText).Font(SentinelStyle::Font(false, 10)).ColorAndOpacity(SentinelStyle::Muted())
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0)
				[
					ActionButton(TEXT("Scan source"), TEXT("The static pass over this project's C++ (Source and project plugins)."),
						FOnClicked::CreateLambda([this]() { RunScan(); return FReply::Handled(); }), true)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 4, 0)
				[
					SNew(SBox).WidthOverride(260)
					[
						SAssignNew(MapBox, SEditableTextBox).Text(FText::FromString(DefaultMap()))
							.ToolTipText(FText::FromString(TEXT("The map the leak test opens, leaves and checks.")))
							.Font(SentinelStyle::Mono(9))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0)
				[
					ActionButton(TEXT("Leak test"), TEXT("Open the map, leave it, collect garbage twice, and list what survived with the chain that holds it."),
						FOnClicked::CreateLambda([this]() { RunLeakTest(); return FReply::Handled(); }))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0)
				[
					ActionButton(TEXT("Visual report"), TEXT("Merge every findings file into one HTML page and open it."),
						FOnClicked::CreateLambda([this]() { RunReport(); return FReply::Handled(); }))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 0, 0)
				[
					ActionButton(TEXT("Reload"), TEXT("Read the findings files again."),
						FOnClicked::CreateLambda([this]() { Reload(); return FReply::Handled(); }))
				]
			]
			// problem filter
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 10)
			[
				SAssignNew(ProblemChips, SHorizontalBox)
			]
			// list and details
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SNew(SSplitter).PhysicalSplitterHandleSize(3.f)
				+ SSplitter::Slot().Value(0.44f)
				[
					SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(SentinelStyle::Well()).Padding(4)
					[
						SAssignNew(List, SListView<TSharedPtr<FSentinelRow>>)
						.ListItemsSource(&Visible)
						.SelectionMode(ESelectionMode::Single)
						.OnGenerateRow(this, &SSentinelPanel::MakeRow)
						.OnSelectionChanged(this, &SSentinelPanel::OnSelect)
						.OnMouseButtonDoubleClick(this, &SSentinelPanel::OnOpen)
					]
				]
				+ SSplitter::Slot().Value(0.56f)
				[
					SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(SentinelStyle::Well()).Padding(14)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SNew(STextBlock).AutoWrapText(true).Font(SentinelStyle::Font(true, 13)).ColorAndOpacity(SentinelStyle::Ink())
							.Text_Lambda([this]() { return FText::FromString(Selected ? Selected->Title : TEXT("No finding selected")); })
						]
						+ SScrollBox::Slot().Padding(0, 4, 0, 10)
						[
							SNew(STextBlock).AutoWrapText(true).Font(SentinelStyle::Mono(9)).ColorAndOpacity(SentinelStyle::Muted())
							.Text_Lambda([this]() {
								if (!Selected) { return FText::GetEmpty(); }
								return FText::FromString(FString::Printf(TEXT("%s  |  %s  |  %s  |  %s"), *Selected->Severity, *Selected->Rule, *Selected->Id, *Selected->WhereText));
							})
						]
						+ SScrollBox::Slot()
						[
							SNew(SBox).Padding(FMargin(0, 0, 0, 12))
							[
								SAssignNew(Diagram, SSentinelDiagram)
							]
						]
						+ SScrollBox::Slot()[ Label(FText::FromString(TEXT("Why it matters")), SentinelStyle::Font(true, 10), SentinelStyle::Accent()) ]
						+ SScrollBox::Slot().Padding(0, 2, 0, 10)
						[
							SNew(STextBlock).AutoWrapText(true).Font(SentinelStyle::Font(false, 10)).ColorAndOpacity(SentinelStyle::Ink())
							.Text_Lambda([this]() { return FText::FromString(Selected ? Selected->Why : FString()); })
						]
						+ SScrollBox::Slot()[ Label(FText::FromString(TEXT("Fix")), SentinelStyle::Font(true, 10), SentinelStyle::Good()) ]
						+ SScrollBox::Slot().Padding(0, 2, 0, 10)
						[
							SNew(STextBlock).AutoWrapText(true).Font(SentinelStyle::Font(false, 10)).ColorAndOpacity(SentinelStyle::Ink())
							.Text_Lambda([this]() { return FText::FromString(Selected ? Selected->Fix : FString()); })
						]
						+ SScrollBox::Slot()[ Label(FText::FromString(TEXT("How it is known")), SentinelStyle::Font(true, 10), SentinelStyle::Muted()) ]
						+ SScrollBox::Slot().Padding(0, 2, 0, 10)
						[
							SNew(STextBlock).AutoWrapText(true).Font(SentinelStyle::Font(false, 9)).ColorAndOpacity(SentinelStyle::Muted())
							.Text_Lambda([this]() { return FText::FromString(Selected ? Selected->HowKnown + (Selected->Suppressed.IsEmpty() ? FString() : TEXT("\nSuppressed: ") + Selected->Suppressed) : FString()); })
						]
					]
				]
			]
			// heap sampler state and the last action
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
			[
				SNew(STextBlock).Text(this, &SSentinelPanel::StatusText).Font(SentinelStyle::Mono(9)).ColorAndOpacity(SentinelStyle::Muted()).AutoWrapText(true)
			]
		]
	];
	RegisterActiveTimer(0.25f, FWidgetActiveTimerDelegate::CreateSP(this, &SSentinelPanel::Poll));
	Reload();
}

SSentinelPanel::~SSentinelPanel()
{
	for (const TSharedPtr<FJob>& J : Jobs)
	{
		FPlatformProcess::ClosePipe(J->ReadPipe, J->WritePipe);
		FPlatformProcess::CloseProc(J->Proc);
	}
}

void SSentinelPanel::Reload()
{
	const FString Keep = Selected ? Selected->File + TEXT("|") + Selected->Id : FString();
	All.Reset();
	const FString Dir = MemorySentinelFiles::Dir();
	TArray<FString> Names;
	IFileManager::Get().FindFiles(Names, *(Dir / TEXT("*.json")), true, false);
	TArray<TPair<FDateTime, FString>> Files;
	for (const FString& N : Names) { Files.Emplace(IFileManager::Get().GetTimeStamp(*(Dir / N)), N); }
	Files.Sort([](const TPair<FDateTime, FString>& A, const TPair<FDateTime, FString>& B) { return A.Key > B.Key; });
	NumFiles = Files.Num();
	NewestFile = Files.Num() ? Files[0].Key : FDateTime();
	for (const TPair<FDateTime, FString>& F : Files)
	{
		FString Text;
		TSharedPtr<FJsonObject> Root;
		auto Problem = [&](const FString& Why) {
			TSharedPtr<FSentinelRow> R = MakeShared<FSentinelRow>();
			R->File = F.Value;
			R->Severity = TEXT("error");
			R->Problem = TEXT("crash");
			R->Title = FString::Printf(TEXT("%s: %s"), *F.Value, *Why);
			R->Why = Why;
			R->HowKnown = FString::Printf(TEXT("READ: %s"), *(Dir / F.Value));
			R->bFileProblem = true;
			All.Add(R);
		};
		if (!FFileHelper::LoadFileToString(Text, *(Dir / F.Value))) { Problem(TEXT("could not be read")); continue; }
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() || Root->GetStringField(TEXT("tool")) != TEXT("memsentinel"))
		{
			Problem(TEXT("is not a Memory Sentinel findings file (it may be being written)"));
			continue;
		}
		const FString Kind = Root->GetStringField(TEXT("kind"));
		if (!Root->GetBoolField(TEXT("ok")))
		{
			Problem(FString::Printf(TEXT("the %s did not run: %s"), *Kind, *Root->GetStringField(TEXT("failure"))));
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>* Findings;
		if (!Root->TryGetArrayField(TEXT("findings"), Findings)) { Problem(TEXT("has no findings list")); continue; }
		for (const TSharedPtr<FJsonValue>& V : *Findings)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			TSharedPtr<FSentinelRow> R = MakeShared<FSentinelRow>();
			R->File = F.Value;
			R->Kind = Kind;
			R->Id = O->GetStringField(TEXT("id"));
			R->Rule = O->GetStringField(TEXT("rule"));
			R->Problem = O->GetStringField(TEXT("problem"));
			R->Severity = O->GetStringField(TEXT("severity"));
			R->Title = O->GetStringField(TEXT("title"));
			R->Why = O->GetStringField(TEXT("why"));
			R->Fix = O->GetStringField(TEXT("fix"));
			R->HowKnown = O->GetStringField(TEXT("how_known"));
			O->TryGetStringField(TEXT("suppressed"), R->Suppressed);
			R->Evidence = O->GetObjectField(TEXT("evidence"));
			R->Where = O->GetObjectField(TEXT("where"));
			if (R->Where->TryGetStringField(TEXT("file"), R->SourceFile))
			{
				R->Line = R->Where->GetIntegerField(TEXT("line"));
				// the last three parts of the path are enough to read; the full path stays for opening it
				TArray<FString> Parts;
				R->SourceFile.Replace(TEXT("\\"), TEXT("/")).ParseIntoArray(Parts, TEXT("/"));
				const int32 Tail = FMath::Min(3, Parts.Num());
				R->WhereText = FString::Printf(TEXT("%s:%d"), *FString::Join(TArrayView<const FString>(Parts).Right(Tail), TEXT("/")), R->Line);
				if (R->Where->HasField(TEXT("object"))) { R->WhereText += TEXT("  ") + R->Where->GetStringField(TEXT("object")); }
			}
			else
			{
				R->Where->TryGetStringField(TEXT("object"), R->WhereText);
			}
			All.Add(R);
		}
	}
	ApplyFilter();
	RebuildProblemChips();
	if (!Keep.IsEmpty())
	{
		for (const TSharedPtr<FSentinelRow>& R : Visible)
		{
			if (R->File + TEXT("|") + R->Id == Keep) { List->SetSelection(R); break; }
		}
	}
}

void SSentinelPanel::ApplyFilter()
{
	Visible.Reset();
	for (const TSharedPtr<FSentinelRow>& R : All)
	{
		if (ProblemFilter.IsEmpty() || R->Problem == ProblemFilter) { Visible.Add(R); }
	}
	Visible.StableSort([](const TSharedPtr<FSentinelRow>& A, const TSharedPtr<FSentinelRow>& B) {
		auto Rank = [](const FSentinelRow& R) { return R.bFileProblem ? -1 : (R.Severity == TEXT("error") ? 0 : R.Severity == TEXT("warning") ? 1 : 2) + (R.Suppressed.IsEmpty() ? 0 : 3); };
		return Rank(*A) < Rank(*B);
	});
	if (List) { List->RequestListRefresh(); }
}

void SSentinelPanel::RebuildProblemChips()
{
	if (!ProblemChips) { return; }
	ProblemChips->ClearChildren();
	const TCHAR* Order[] = { TEXT(""), TEXT("dangling"), TEXT("cycle"), TEXT("leak"), TEXT("stomp"), TEXT("crash") };
	for (const TCHAR* P : Order)
	{
		const FString Problem = P;
		int32 N = 0;
		for (const TSharedPtr<FSentinelRow>& R : All) { N += (Problem.IsEmpty() || R->Problem == Problem) && R->Suppressed.IsEmpty(); }
		if (!Problem.IsEmpty() && N == 0 && Problem == TEXT("crash")) { continue; }
		const FLinearColor C = Problem.IsEmpty() ? SentinelStyle::Ink() : SentinelStyle::Problem(Problem);
		const bool bOn = ProblemFilter == Problem;
		ProblemChips->AddSlot().AutoWidth().Padding(0, 0, 8, 0)
		[
			SNew(SButton)
			.ButtonColorAndOpacity(bOn ? C.CopyWithNewOpacity(0.35f) : SentinelStyle::Card())
			.ContentPadding(FMargin(10, 4))
			.OnClicked(FOnClicked::CreateLambda([this, Problem]() { ProblemFilter = Problem; ApplyFilter(); RebuildProblemChips(); return FReply::Handled(); }))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[
					SNew(SBox).WidthOverride(8).HeightOverride(8)
					[
						SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(C)
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Font(SentinelStyle::Font(true, 9)).ColorAndOpacity(SentinelStyle::Ink())
					.Text(FText::FromString(FString::Printf(TEXT("%s  %d"), Problem.IsEmpty() ? TEXT("All") : *SentinelStyle::ProblemName(Problem), N)))
				]
			]
		];
	}
}

TSharedRef<ITableRow> SSentinelPanel::MakeRow(TSharedPtr<FSentinelRow> Row, const TSharedRef<STableViewBase>& Owner)
{
	const FLinearColor Edge = Row->bFileProblem ? SentinelStyle::Severity(TEXT("error")) : SentinelStyle::Problem(Row->Problem);
	const float Fade = Row->Suppressed.IsEmpty() ? 1.f : 0.45f;
	return SNew(STableRow<TSharedPtr<FSentinelRow>>, Owner).Padding(FMargin(0, 2))
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SBox).WidthOverride(4)
			[
				SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(Edge.CopyWithNewOpacity(Fade))
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).Padding(10, 6)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(FText::FromString(Row->Severity.ToUpper())).Font(SentinelStyle::Font(true, 8))
					.ColorAndOpacity(SentinelStyle::Severity(Row->Severity).CopyWithNewOpacity(Fade))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(STextBlock).Text(FText::FromString(Row->Title)).Font(SentinelStyle::Font(true, 10))
					.ColorAndOpacity(SentinelStyle::Ink().CopyWithNewOpacity(Fade)).AutoWrapText(true)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SNew(STextBlock).Text(FText::FromString(Row->WhereText.IsEmpty() ? Row->File : Row->WhereText)).Font(SentinelStyle::Mono(8))
				.ColorAndOpacity(SentinelStyle::Muted().CopyWithNewOpacity(Fade))
			]
		]
	];
}

void SSentinelPanel::OnSelect(TSharedPtr<FSentinelRow> Row, ESelectInfo::Type)
{
	Selected = Row;
	if (Diagram) { Diagram->SetRow(Row); }
}

void SSentinelPanel::OnOpen(TSharedPtr<FSentinelRow> Row)
{
	if (!Row || Row->SourceFile.IsEmpty()) { return; }
	FString Path = Row->SourceFile;
	if (FPaths::IsRelative(Path)) { Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Path); }
	if (OpenSourceHook)
	{
		OpenSourceHook(Path, Row->Line);
		LastMessage = FString::Printf(TEXT("Opened %s:%d"), *Path, Row->Line);
	}
	else
	{
		// a running game has no IDE link: hand the location over on the clipboard, and say so
		FPlatformApplicationMisc::ClipboardCopy(*FString::Printf(TEXT("%s:%d"), *Path, Row->Line));
		LastMessage = FString::Printf(TEXT("Copied %s:%d to the clipboard (no IDE link in a running game)"), *Path, Row->Line);
	}
}

bool SSentinelPanel::SelectFirst(const FString& Rule)
{
	for (const TSharedPtr<FSentinelRow>& R : Visible)
	{
		if (!R->bFileProblem && (Rule.IsEmpty() || R->Rule == Rule))
		{
			List->SetSelection(R);
			List->RequestScrollIntoView(R);
			OnSelect(R, ESelectInfo::Direct);
			return true;
		}
	}
	return false;
}

FText SSentinelPanel::SummaryText() const
{
	int32 E = 0, W = 0, I = 0;
	for (const TSharedPtr<FSentinelRow>& R : All)
	{
		if (!R->Suppressed.IsEmpty()) { continue; }
		E += R->Severity == TEXT("error");
		W += R->Severity == TEXT("warning");
		I += R->Severity == TEXT("info");
	}
	return FText::FromString(FString::Printf(TEXT("%d findings files  |  %d errors, %d warnings, %d info  |  %s"),
		NumFiles, E, W, I, *ProjectRelative(MemorySentinelFiles::Dir())));
}

FText SSentinelPanel::StatusText() const
{
	using namespace MemorySentinel;
	FString Heap;
	if (FHeapSampler::IsInstalled())
	{
		const FHeapStats S = FHeapSampler::Stats();
		Heap = FString::Printf(TEXT("Heap sampler on: 1 in %u sampled, %llu of %llu allocations, %llu live, %llu in quarantine, %llu checks, %llu rotations, watching %d word(s)%s"),
			S.SampleEvery, S.Sampled + S.GuardSampled, S.Allocations, S.Live, S.Quarantined, S.CanaryChecks, S.Rotations, S.NumWatches,
			S.bRotor ? TEXT("") : *FString::Printf(TEXT(" (rotor off: %s)"), *FHeapSampler::WhyNoRotor()));
	}
	else
	{
		Heap = FString::Printf(TEXT("Heap sampler off: %s"), *FHeapSampler::WhyNotInstalled());
	}
	if (!Busy.IsEmpty()) { Heap += TEXT("\n") + Busy; }
	if (!LastMessage.IsEmpty()) { Heap += TEXT("\n") + LastMessage; }
	return FText::FromString(Heap);
}

EActiveTimerReturnType SSentinelPanel::Poll(double, float)
{
	for (int32 I = Jobs.Num() - 1; I >= 0; --I)
	{
		FJob& J = *Jobs[I];
		J.Output += FPlatformProcess::ReadPipe(J.ReadPipe);
		if (FPlatformProcess::IsProcRunning(J.Proc)) { continue; }
		J.Output += FPlatformProcess::ReadPipe(J.ReadPipe);
		int32 Code = -1;
		FPlatformProcess::GetProcReturnCode(J.Proc, &Code);
		FPlatformProcess::ClosePipe(J.ReadPipe, J.WritePipe);
		FPlatformProcess::CloseProc(J.Proc);
		TSharedPtr<FJob> Keep = Jobs[I];
		Jobs.RemoveAt(I);
		Busy.Reset();
		Keep->Done(Code, Keep->Output);
	}
	// reload when a detector wrote something new (the heap file is rewritten while a game runs)
	TArray<FString> Names;
	const FString Dir = MemorySentinelFiles::Dir();
	IFileManager::Get().FindFiles(Names, *(Dir / TEXT("*.json")), true, false);
	FDateTime Newest;
	for (const FString& N : Names) { Newest = FMath::Max(Newest, IFileManager::Get().GetTimeStamp(*(Dir / N))); }
	if (Names.Num() != NumFiles || Newest > NewestFile) { Reload(); }
	return EActiveTimerReturnType::Continue;
}

bool SSentinelPanel::RunTool(const TArray<FString>& Args, const FString& What, TFunction<void(int32, const FString&)> Done)
{
	const FString Python = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Win64/python.exe"));
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MemorySentinel"));
	const FString Tool = Plugin ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Content/Python/ms.py")) : FString();
	if (!FPaths::FileExists(Python)) { LastMessage = FString::Printf(TEXT("%s could not run: the engine's Python is not at %s"), *What, *Python); return false; }
	if (!FPaths::FileExists(Tool)) { LastMessage = FString::Printf(TEXT("%s could not run: %s is missing"), *What, *Tool); return false; }
	FString Params = FString::Printf(TEXT("\"%s\""), *Tool);
	for (const FString& A : Args) { Params += FString::Printf(TEXT(" \"%s\""), *A); }
	TSharedPtr<FJob> J = MakeShared<FJob>();
	FPlatformProcess::CreatePipe(J->ReadPipe, J->WritePipe);
	J->Proc = FPlatformProcess::CreateProc(*Python, *Params, false, true, true, nullptr, 0, nullptr, J->WritePipe, nullptr);
	if (!J->Proc.IsValid())
	{
		FPlatformProcess::ClosePipe(J->ReadPipe, J->WritePipe);
		LastMessage = FString::Printf(TEXT("%s could not start %s"), *What, *Python);
		return false;
	}
	J->What = What;
	J->Done = MoveTemp(Done);
	Jobs.Add(J);
	Busy = What + TEXT("...");
	return true;
}

void SSentinelPanel::RunScan()
{
	const FString Out = MemorySentinelFiles::Dir() / TEXT("scan.json");
	RunTool({ TEXT("scan"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), TEXT("-o"), Out }, TEXT("Scanning the source"),
		[this](int32 Code, const FString& Output) {
			FString LastLine;
			TArray<FString> Lines;
			Output.ParseIntoArrayLines(Lines);
			for (const FString& L : Lines) { if (L.Contains(TEXT("error,"))) { LastLine = L.TrimStartAndEnd(); } }
			LastMessage = Code == 2 ? FString::Printf(TEXT("The scan could not run: %s"), *Output.Right(300))
				: FString::Printf(TEXT("Scan done: %s"), *LastLine);
			Reload();
		});
}

void SSentinelPanel::RunLeakTest()
{
	const FString Map = MapBox ? MapBox->GetText().ToString().TrimStartAndEnd() : FString();
	if (HasGameWorld())
	{
		FLevelLeakTest::FOptions O;
		O.Map = Map;
		FString Why;
		LastMessage = FLevelLeakTest::Start(O, Why) ? FString::Printf(TEXT("Leak test running on %s"), *Map) : FString::Printf(TEXT("Leak test not started: %s"), *Why);
		return;
	}
	// in the editor: a separate, windowless game process runs the test, so the editor's own world is never touched
	const FString Exe = FPlatformProcess::ExecutablePath();
	const FString Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	const FString Params = FString::Printf(TEXT("\"%s\" /Engine/Maps/Entry -game -RenderOffScreen -unattended -nosplash -DeterministicAudio -nosound -ExecCmds=\"ms.LeakTest %s -quit\""), *Project, *Map);
	TSharedPtr<FJob> J = MakeShared<FJob>();
	FPlatformProcess::CreatePipe(J->ReadPipe, J->WritePipe);
	J->Proc = FPlatformProcess::CreateProc(*Exe, *Params, false, true, true, nullptr, 0, nullptr, J->WritePipe, nullptr);
	if (!J->Proc.IsValid())
	{
		FPlatformProcess::ClosePipe(J->ReadPipe, J->WritePipe);
		LastMessage = FString::Printf(TEXT("Leak test could not start %s"), *Exe);
		return;
	}
	J->What = TEXT("Leak test");
	J->Done = [this, Map](int32 Code, const FString&) {
		LastMessage = Code == 0 ? FString::Printf(TEXT("Leak test on %s: nothing survived"), *Map)
			: Code == 1 ? FString::Printf(TEXT("Leak test on %s: objects survived the unload"), *Map)
			: FString::Printf(TEXT("Leak test on %s did not finish (exit code %d); see its findings file"), *Map, Code);
		Reload();
	};
	Jobs.Add(J);
	Busy = FString::Printf(TEXT("Leak test running on %s in a separate game process..."), *Map);
}

void SSentinelPanel::RunReport()
{
	const FString Dir = MemorySentinelFiles::Dir();
	const FString Out = Dir / TEXT("report.html");
	RunTool({ TEXT("report"), Dir, TEXT("-o"), Out }, TEXT("Building the visual report"), [this, Out](int32 Code, const FString& Output) {
		if (Code == 0)
		{
			FPlatformProcess::LaunchURL(*(TEXT("file:///") + Out.Replace(TEXT("\\"), TEXT("/"))), nullptr, nullptr);
			LastMessage = FString::Printf(TEXT("Opened %s"), *Out);
		}
		else
		{
			LastMessage = FString::Printf(TEXT("The report could not be built: %s"), *Output.Right(300));
		}
	});
}
