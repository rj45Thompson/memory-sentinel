// Copyright (c) 2026 RJ Thompson. Released under the MIT License; see LICENSE.
// A finding's evidence as a picture (UC-4.2), painted directly:
//   level-survivor   the reference chain, root first, each link labelled with what holds the next one
//   shared-cycle     the ring of types, each arrow a member; the side to weaken in orange
//   heap stomps      the bytes after the block's end: canary bytes that held in green, changed ones in red
//   source findings  the owner, the pointer the collector cannot see, and the object it points at
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"

struct FSentinelRow;

class SSentinelDiagram : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SSentinelDiagram) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&) {}
	void SetRow(TSharedPtr<FSentinelRow> InRow);

	virtual FVector2D ComputeDesiredSize(float) const override;
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& Culling,
		FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle& Style, bool bParentEnabled) const override;

private:
	void PaintChain(const FGeometry& G, FSlateWindowElementList& Out, int32 Layer) const;
	void PaintCycle(const FGeometry& G, FSlateWindowElementList& Out, int32 Layer) const;
	void PaintBytes(const FGeometry& G, FSlateWindowElementList& Out, int32 Layer) const;
	void PaintPointer(const FGeometry& G, FSlateWindowElementList& Out, int32 Layer) const;

	TSharedPtr<FSentinelRow> Row;
};
