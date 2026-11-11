#include "SentinelFindings.h"

#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogMemorySentinelFiles, Log, All);

FString MemorySentinelFiles::Dir()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MemorySentinel"));
}

FString MemorySentinelFiles::ProblemOf(const FString& Rule)
{
	static const TMap<FString, FString> Problems = {
		{ TEXT("uobject-member-unseen"), TEXT("dangling") }, { TEXT("static-uobject-pointer"), TEXT("dangling") },
		{ TEXT("subclassof-unseen"), TEXT("dangling") }, { TEXT("uobject-in-plain-type"), TEXT("dangling") },
		{ TEXT("fgcobject-missing-member"), TEXT("dangling") }, { TEXT("lambda-captures-uobject"), TEXT("dangling") },
		{ TEXT("void-hides-uobject"), TEXT("dangling") }, { TEXT("shared-cycle"), TEXT("cycle") },
		{ TEXT("shared-self-edge"), TEXT("cycle") }, { TEXT("shared-self-capture"), TEXT("cycle") },
		{ TEXT("level-survivor"), TEXT("leak") }, { TEXT("heap-overrun"), TEXT("stomp") },
		{ TEXT("write-after-free"), TEXT("stomp") }, { TEXT("double-free"), TEXT("stomp") },
		{ TEXT("guard-fault"), TEXT("stomp") }, { TEXT("run-crash"), TEXT("crash") },
	};
	const FString* P = Problems.Find(Rule);
	checkf(P, TEXT("Memory Sentinel: rule %s is not in the problem table"), *Rule);
	return *P;
}

FString MemorySentinelFiles::Stamp()
{
	return FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
}

bool FSentinelFindingsFile::Write(const FString& Path, const TCHAR* IdPrefix)
{
	if (Made.IsEmpty())
	{
		Made = FDateTime::UtcNow().ToIso8601();
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema"), 1);
	Root->SetStringField(TEXT("tool"), TEXT("memsentinel"));
	Root->SetStringField(TEXT("version"), TEXT("1.0"));
	Root->SetStringField(TEXT("kind"), Kind);
	Root->SetStringField(TEXT("made"), Made);
	Root->SetBoolField(TEXT("ok"), bOk);
	Root->SetStringField(TEXT("failure"), Failure);
	Root->SetObjectField(TEXT("target"), Target);
	Root->SetObjectField(TEXT("checked"), Checked);
	int32 Errors = 0, Warnings = 0, Infos = 0;
	TArray<TSharedPtr<FJsonValue>> List;
	for (int32 I = 0; I < Findings.Num(); ++I)
	{
		const FSentinelFinding& F = Findings[I];
		checkf(!F.HowKnown.IsEmpty(), TEXT("Memory Sentinel: finding %s has no how_known"), *F.Title);
		Errors += F.Severity == TEXT("error");
		Warnings += F.Severity == TEXT("warning");
		Infos += F.Severity == TEXT("info");
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), FString::Printf(TEXT("%s-%03d"), IdPrefix, I + 1));
		O->SetStringField(TEXT("rule"), F.Rule);
		O->SetStringField(TEXT("problem"), MemorySentinelFiles::ProblemOf(F.Rule));
		O->SetStringField(TEXT("severity"), F.Severity);
		O->SetStringField(TEXT("title"), F.Title);
		O->SetObjectField(TEXT("where"), F.Where);
		O->SetStringField(TEXT("why"), F.Why);
		O->SetStringField(TEXT("fix"), F.Fix);
		O->SetStringField(TEXT("how_known"), F.HowKnown);
		O->SetObjectField(TEXT("evidence"), F.Evidence);
		List.Add(MakeShared<FJsonValueObject>(O));
	}
	TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
	Counts->SetNumberField(TEXT("error"), Errors);
	Counts->SetNumberField(TEXT("warning"), Warnings);
	Counts->SetNumberField(TEXT("info"), Infos);
	Counts->SetNumberField(TEXT("suppressed"), 0);
	Root->SetObjectField(TEXT("counts"), Counts);
	TArray<TSharedPtr<FJsonValue>> Blind;
	for (const FString& B : BlindSpots)
	{
		Blind.Add(MakeShared<FJsonValueString>(B));
	}
	Root->SetArrayField(TEXT("blind_spots"), Blind);
	Root->SetArrayField(TEXT("findings"), List);

	FString Text;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		UE_LOG(LogMemorySentinelFiles, Error, TEXT("Memory Sentinel: could not serialize %s"), *Path);
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogMemorySentinelFiles, Error, TEXT("Memory Sentinel: could not write %s"), *Path);
		return false;
	}
	return true;
}
