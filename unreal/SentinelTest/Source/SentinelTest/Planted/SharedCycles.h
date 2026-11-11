// PLANTED BUGS: shared pointer cycles. A quest log and its nodes hold each other; a panel holds its host through an
// interface the host implements; a downloader stores a lambda that holds the downloader.
#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

struct FQuestNode;

struct FQuestLog
{
	TArray<TSharedPtr<FQuestNode>> Nodes;           // PLANTED: shared-cycle
};

struct FQuestNode
{
	TSharedPtr<FQuestLog> Log;
	TSharedPtr<FQuestNode> Next;                    // PLANTED: shared-self-edge
};

class IPanelHost
{
public:
	virtual ~IPanelHost() = default;
};

class FPanel
{
public:
	TSharedPtr<IPanelHost> Host;
};

class FHud : public IPanelHost
{
public:
	TArray<TSharedRef<FPanel>> Panels;              // PLANTED: shared-cycle
};

class FDownloader : public TSharedFromThis<FDownloader>
{
public:
	FSimpleDelegate OnDone;
	void Start()
	{
		OnDone.BindLambda([Self = AsShared()]() { Self->Finish(); });   // PLANTED: shared-self-capture
	}
	void Finish() {}
};
