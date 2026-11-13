// Copyright (c) 2026 RJ Thompson. Released under the MIT License; see LICENSE.
#include "SSentinelDiagram.h"

#include "SSentinelPanel.h"
#include "SentinelStyle.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"

namespace
{
	const FSlateBrush* White() { return FCoreStyle::Get().GetBrush("WhiteBrush"); }

	void Box(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, FVector2f Pos, FVector2f Size, const FLinearColor& Fill)
	{
		FSlateDrawElement::MakeBox(Out, Layer, G.ToPaintGeometry(Size, FSlateLayoutTransform(Pos)), White(), ESlateDrawEffect::None, Fill);
	}

	void Frame(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, FVector2f Pos, FVector2f Size, const FLinearColor& C, float T = 1.5f)
	{
		TArray<FVector2f> P = { Pos, Pos + FVector2f(Size.X, 0), Pos + Size, Pos + FVector2f(0, Size.Y), Pos };
		FSlateDrawElement::MakeLines(Out, Layer, G.ToPaintGeometry(), P, ESlateDrawEffect::None, C, true, T);
	}

	void Line(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, FVector2f A, FVector2f B, const FLinearColor& C, float T = 2.f, bool bDashed = false)
	{
		if (!bDashed)
		{
			TArray<FVector2f> P = { A, B };
			FSlateDrawElement::MakeLines(Out, Layer, G.ToPaintGeometry(), P, ESlateDrawEffect::None, C, true, T);
			return;
		}
		const FVector2f D = B - A;
		const float Len = D.Size();
		const FVector2f U = Len > 0 ? D / Len : FVector2f(1, 0);
		for (float S = 0; S < Len; S += 12.f)
		{
			TArray<FVector2f> P = { A + U * S, A + U * FMath::Min(S + 7.f, Len) };
			FSlateDrawElement::MakeLines(Out, Layer, G.ToPaintGeometry(), P, ESlateDrawEffect::None, C, true, T);
		}
	}

	void Arrow(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, FVector2f A, FVector2f B, const FLinearColor& C, float T = 2.f, bool bDashed = false)
	{
		Line(Out, Layer, G, A, B, C, T, bDashed);
		const FVector2f D = (B - A).GetSafeNormal();
		const FVector2f N(-D.Y, D.X);
		TArray<FVector2f> Head = { B - D * 10.f + N * 5.f, B, B - D * 10.f - N * 5.f };
		FSlateDrawElement::MakeLines(Out, Layer, G.ToPaintGeometry(), Head, ESlateDrawEffect::None, C, true, T);
	}

	FVector2f Measure(const FString& S, const FSlateFontInfo& Font)
	{
		return FVector2f(FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(S, Font));
	}

	void Text(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, FVector2f Pos, const FString& S, const FSlateFontInfo& Font, const FLinearColor& C)
	{
		FSlateDrawElement::MakeText(Out, Layer, G.ToPaintGeometry(Measure(S, Font) + FVector2f(4, 2), FSlateLayoutTransform(Pos)), S, Font, ESlateDrawEffect::None, C);
	}

	FString Fit(const FString& S, int32 Max)
	{
		return S.Len() <= Max ? S : TEXT("...") + S.Right(Max - 3);
	}

	FString ViaLabel(FString Via)
	{
		if (Via.StartsWith(TEXT("AddReferencedObjects"))) { return TEXT("AddReferencedObjects"); }
		int32 At = Via.Find(TEXT(" (0x"));
		if (At != INDEX_NONE) { Via.LeftInline(At); }
		return Via;
	}

	/** A symbolized frame without its " [module path]" tail. */
	FString FrameLabel(FString Frame)
	{
		int32 At = Frame.Find(TEXT(" ["));
		if (At != INDEX_NONE) { Frame.LeftInline(At); }
		return Frame;
	}

	FString ShortObject(const FString& Path)
	{
		FString Tail = Path;
		int32 At;
		if (Tail.FindLastChar(TEXT('.'), At)) { Tail = Tail.Mid(At + 1); }
		if (Tail.FindLastChar(TEXT(':'), At)) { Tail = Tail.Mid(At + 1); }
		return Tail;
	}

	/** A labelled node: a filled box with a bold first line and a muted second line. Returns its size. */
	FVector2f Node(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, FVector2f Pos, const FString& Top, const FString& Bottom,
		const FLinearColor& Edge, float MinWidth = 120.f)
	{
		const FSlateFontInfo Bold = SentinelStyle::Font(true, 10);
		const FSlateFontInfo Small = SentinelStyle::Font(false, 9);
		const float W = FMath::Max3(MinWidth, Measure(Top, Bold).X + 20.f, Measure(Bottom, Small).X + 20.f);
		const FVector2f Size(W, Bottom.IsEmpty() ? 30.f : 44.f);
		Box(Out, Layer, G, Pos, Size, SentinelStyle::Card());
		Box(Out, Layer + 1, G, Pos, FVector2f(4.f, Size.Y), Edge);
		Frame(Out, Layer + 1, G, Pos, Size, Edge.CopyWithNewOpacity(0.55f), 1.f);
		Text(Out, Layer + 2, G, Pos + FVector2f(12, 6), Top, Bold, SentinelStyle::Ink());
		if (!Bottom.IsEmpty()) { Text(Out, Layer + 2, G, Pos + FVector2f(12, 24), Bottom, Small, SentinelStyle::Muted()); }
		return Size;
	}
}

void SSentinelDiagram::SetRow(TSharedPtr<FSentinelRow> InRow)
{
	Row = InRow;
	Invalidate(EInvalidateWidgetReason::LayoutAndVolatility | EInvalidateWidgetReason::Paint);
}

FVector2D SSentinelDiagram::ComputeDesiredSize(float) const
{
	if (!Row.IsValid() || !Row->Evidence.IsValid()) { return FVector2D(420, 40); }
	if (Row->Rule == TEXT("level-survivor"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Chain;
		const int32 N = Row->Evidence->TryGetArrayField(TEXT("chain"), Chain) ? Chain->Num() : 0;
		return FVector2D(460, FMath::Max(1, N) * 74.f + 20.f);
	}
	if (Row->Rule == TEXT("shared-cycle") || Row->Rule == TEXT("shared-self-edge")) { return FVector2D(460, 300); }
	if (Row->Problem == TEXT("stomp")) { return FVector2D(560, 190); }
	return FVector2D(520, 150);
}

int32 SSentinelDiagram::OnPaint(const FPaintArgs&, const FGeometry& G, const FSlateRect&, FSlateWindowElementList& Out,
	int32 Layer, const FWidgetStyle&, bool) const
{
	Box(Out, Layer, G, FVector2f(0, 0), FVector2f(G.GetLocalSize()), SentinelStyle::Well());
	if (!Row.IsValid() || !Row->Evidence.IsValid())
	{
		Text(Out, Layer + 1, G, FVector2f(12, 12), TEXT("Select a finding to see its evidence."), SentinelStyle::Font(false, 10), SentinelStyle::Muted());
		return Layer + 3;
	}
	if (Row->Rule == TEXT("level-survivor")) { PaintChain(G, Out, Layer + 1); }
	else if (Row->Rule == TEXT("shared-cycle") || Row->Rule == TEXT("shared-self-edge")) { PaintCycle(G, Out, Layer + 1); }
	else if (Row->Problem == TEXT("stomp")) { PaintBytes(G, Out, Layer + 1); }
	else { PaintPointer(G, Out, Layer + 1); }
	return Layer + 6;
}

void SSentinelDiagram::PaintChain(const FGeometry& G, FSlateWindowElementList& Out, int32 Layer) const
{
	const TArray<TSharedPtr<FJsonValue>>* Chain;
	if (!Row->Evidence->TryGetArrayField(TEXT("chain"), Chain) || Chain->Num() == 0)
	{
		const FString Why = Row->Evidence->GetBoolField(TEXT("rooted")) ? TEXT("In the root set: no chain leads to it.") : TEXT("The reference search found no chain.");
		Node(Out, Layer, G, FVector2f(16, 16), Row->Evidence->GetStringField(TEXT("class")), Why, SentinelStyle::Problem(TEXT("leak")), 300.f);
		return;
	}
	float Y = 12.f;
	for (int32 I = 0; I < Chain->Num(); ++I)
	{
		const TSharedPtr<FJsonObject> L = (*Chain)[I]->AsObject();
		const bool bLast = I == Chain->Num() - 1;
		const FString Cls = L->GetStringField(TEXT("class"));
		const FString Obj = Fit(ShortObject(L->GetStringField(TEXT("object"))), 48);
		const FLinearColor Edge = bLast ? SentinelStyle::Severity(TEXT("error")) : (I == 0 ? SentinelStyle::Accent() : SentinelStyle::Muted());
		const FVector2f Size = Node(Out, Layer, G, FVector2f(28, Y), Cls + (I == 0 ? TEXT("   (root)") : bLast ? TEXT("   (survivor)") : TEXT("")), Obj, Edge, 260.f);
		if (!bLast)
		{
			const TSharedPtr<FJsonObject> Next = (*Chain)[I + 1]->AsObject();
			const FVector2f A(44, Y + Size.Y + 2), B(44, Y + 72);
			Arrow(Out, Layer, G, A, B, SentinelStyle::Muted(), 2.f);
			Text(Out, Layer + 2, G, FVector2f(56, Y + Size.Y + 6), Fit(ViaLabel(Next->GetStringField(TEXT("via"))), 60), SentinelStyle::Font(false, 9), SentinelStyle::Accent());
		}
		Y += 74.f;
	}
}

void SSentinelDiagram::PaintCycle(const FGeometry& G, FSlateWindowElementList& Out, int32 Layer) const
{
	const TArray<TSharedPtr<FJsonValue>>* Cycle;
	const TArray<TSharedPtr<FJsonValue>>* Edges;
	if (!Row->Evidence->TryGetArrayField(TEXT("cycle"), Cycle) || !Row->Evidence->TryGetArrayField(TEXT("edges"), Edges)) { return; }
	const FString Weaken = Row->Evidence->HasField(TEXT("weaken")) ? Row->Evidence->GetStringField(TEXT("weaken")) : FString();
	const int32 N = FMath::Max(1, Cycle->Num() - 1);
	const FVector2f Center(230.f, 150.f);
	const float R = N == 1 ? 0.f : 100.f;
	TArray<FVector2f> At;
	for (int32 I = 0; I < N; ++I)
	{
		const float A = (N == 2 ? PI : -PI / 2) + 2 * PI * I / N;
		At.Add(Center + FVector2f(FMath::Cos(A), FMath::Sin(A)) * R);
	}
	for (int32 I = 0; I < Edges->Num() && I < N; ++I)
	{
		const TSharedPtr<FJsonObject> E = (*Edges)[I]->AsObject();
		const bool bWeak = E->GetStringField(TEXT("member")) == Weaken;
		const FLinearColor C = bWeak ? SentinelStyle::Severity(TEXT("warning")) : SentinelStyle::Problem(TEXT("cycle"));
		FVector2f A = At[I], B = At[(I + 1) % N];
		if (N == 1)
		{
			// a type that holds its own type: a loop above it
			const FVector2f P0 = A + FVector2f(-30, -24), P1 = A + FVector2f(-30, -70), P2 = A + FVector2f(30, -70), P3 = A + FVector2f(30, -24);
			Line(Out, Layer, G, P0, P1, C);
			Line(Out, Layer, G, P1, P2, C);
			Arrow(Out, Layer, G, P2, P3, C);
			Text(Out, Layer + 2, G, P1 + FVector2f(0, -18), E->GetStringField(TEXT("member")), SentinelStyle::Font(false, 9), C);
			continue;
		}
		const FVector2f D = (B - A).GetSafeNormal();
		if (N != 2) { Arrow(Out, Layer, G, A + D * 44.f, B - D * 44.f, C, bWeak ? 3.f : 2.f, bWeak); }
		const float Bow = N == 2 ? 30.f : 16.f;
		const FVector2f Side(-D.Y, D.X);
		const FVector2f Mid = (A + B) * 0.5f + Side * Bow;
		if (N == 2)
		{
			// two arrows between the same pair: bow each to its own side so they do not overlap
			Line(Out, Layer, G, A + D * 46.f, Mid, C, bWeak ? 3.f : 2.f, bWeak);
			Arrow(Out, Layer, G, Mid, B - D * 46.f, C, bWeak ? 3.f : 2.f, bWeak);
		}
		const FString Member = Fit(E->GetStringField(TEXT("member")), 34);
		const float W = Measure(Member, SentinelStyle::Font(false, 9)).X;
		Text(Out, Layer + 2, G, Mid + Side * 14.f - FVector2f(W * 0.5f, 8), Member, SentinelStyle::Font(false, 9), C);
	}
	for (int32 I = 0; I < N; ++I)
	{
		const FString Name = (*Cycle)[I]->AsString();
		const FVector2f Size(FMath::Max(90.f, Measure(Name, SentinelStyle::Font(true, 10)).X + 24.f), 30.f);
		Node(Out, Layer + 3, G, At[I] - Size * 0.5f, Name, FString(), SentinelStyle::Problem(TEXT("cycle")), Size.X);
	}
	if (!Weaken.IsEmpty())
	{
		Text(Out, Layer + 2, G, FVector2f(12, 276), FString::Printf(TEXT("dashed: make %s a TWeakPtr"), *Weaken), SentinelStyle::Font(false, 9), SentinelStyle::Severity(TEXT("warning")));
	}
}

void SSentinelDiagram::PaintBytes(const FGeometry& G, FSlateWindowElementList& Out, int32 Layer) const
{
	const TSharedPtr<FJsonObject>& E = Row->Evidence;
	const int64 Size = int64(E->GetNumberField(TEXT("size")));
	const int64 Offset = int64(E->GetNumberField(TEXT("offset")));
	const bool bPast = Offset >= Size;
	TArray<FString> Found, Expected;
	E->GetStringField(TEXT("found")).ParseIntoArray(Found, TEXT(" "));
	E->GetStringField(TEXT("expected")).ParseIntoArray(Expected, TEXT(" "));
	const FSlateFontInfo Mono = SentinelStyle::Mono(9);
	const float Cell = 30.f;
	const FVector2f Origin(16, 54);
	const int32 Before = 4;   // cells of the block shown before the first bad byte
	Text(Out, Layer + 2, G, FVector2f(16, 10), bPast ? (Offset == Size ? FString::Printf(TEXT("%lld-byte block: the first byte past its end changed"), Size)
			: FString::Printf(TEXT("%lld-byte block: the first changed byte is %lld past its end"), Size, Offset - Size))
		: FString::Printf(TEXT("freed %lld-byte block: the first changed byte is at offset %lld"), Size, Offset), SentinelStyle::Font(true, 10), SentinelStyle::Ink());
	for (int32 I = 0; I < Before + 16; ++I)
	{
		const int64 At = Offset - Before + I;
		if (At < 0) { continue; }
		const FVector2f P = Origin + FVector2f(I * (Cell + 2), 0);
		const int32 K = I - Before;
		FLinearColor Fill = SentinelStyle::Card();
		FString Label = TEXT("..");
		if (K >= 0 && K < Found.Num())
		{
			const bool bBad = Found[K] != Expected[K];
			Fill = bBad ? SentinelStyle::Severity(TEXT("error")).CopyWithNewOpacity(0.85f) : SentinelStyle::Good().CopyWithNewOpacity(0.55f);
			Label = Found[K];
		}
		else if (bPast && At >= Size)
		{
			Fill = SentinelStyle::Good().CopyWithNewOpacity(0.25f);
		}
		Box(Out, Layer, G, P, FVector2f(Cell, Cell), Fill);
		Text(Out, Layer + 2, G, P + FVector2f(7, 8), Label, Mono, SentinelStyle::Ink());
		if (At == Size && bPast)
		{
			Line(Out, Layer + 3, G, P + FVector2f(-1, -14), P + FVector2f(-1, Cell + 14), SentinelStyle::Ink(), 2.f);
			Text(Out, Layer + 3, G, P + FVector2f(-60, -30), TEXT("end of block"), SentinelStyle::Font(false, 9), SentinelStyle::Muted());
		}
	}
	Text(Out, Layer + 2, G, Origin + FVector2f(0, Cell + 16), bPast ? TEXT("green: canary byte that held   red: canary byte that changed")
		: TEXT("green: freed-pattern byte that held   red: byte written after the free"), SentinelStyle::Font(false, 9), SentinelStyle::Muted());
	const FString Caught = E->GetStringField(TEXT("caught_by"));
	const TArray<TSharedPtr<FJsonValue>>* WStack;
	FString Writer;
	if (E->TryGetArrayField(TEXT("write_stack"), WStack) && WStack->Num())
	{
		for (const TSharedPtr<FJsonValue>& V : *WStack)
		{
			const FString S = V->AsString();
			if (!S.Contains(TEXT("[UnrealEditor-Core]")) && !S.Contains(TEXT("MemorySentinelHeap")) && !S.Contains(TEXT("ntdll"))) { Writer = FrameLabel(S); break; }
		}
	}
	Text(Out, Layer + 2, G, Origin + FVector2f(0, Cell + 40), FString::Printf(TEXT("caught by: %s"), *Caught), SentinelStyle::Font(true, 9), SentinelStyle::Accent());
	if (!Writer.IsEmpty())
	{
		Text(Out, Layer + 2, G, Origin + FVector2f(0, Cell + 60), FString::Printf(TEXT("writer: %s"), *Fit(Writer, 86)), SentinelStyle::Mono(9), SentinelStyle::Severity(TEXT("error")));
	}
}

void SSentinelDiagram::PaintPointer(const FGeometry& G, FSlateWindowElementList& Out, int32 Layer) const
{
	const TSharedPtr<FJsonObject>& E = Row->Evidence;
	const FString Owner = E->HasField(TEXT("owner")) ? E->GetStringField(TEXT("owner")) : FString(TEXT("owner"));
	const FString Holds = E->HasField(TEXT("holds")) ? E->GetStringField(TEXT("holds"))
		: E->HasField(TEXT("capture")) ? FString::Printf(TEXT("captures %s"), *E->GetStringField(TEXT("capture")))
		: E->HasField(TEXT("value")) ? FString::Printf(TEXT("%s as void*"), *E->GetStringField(TEXT("value"))) : Row->Rule;
	const FString Member = E->HasField(TEXT("member")) ? E->GetStringField(TEXT("member")) : E->HasField(TEXT("call")) ? E->GetStringField(TEXT("call")) : FString();
	const FLinearColor Red = SentinelStyle::Severity(Row->Severity);
	const FVector2f A = Node(Out, Layer, G, FVector2f(16, 28), Owner, Row->Kind == TEXT("scan") ? TEXT("holds the pointer") : FString(), SentinelStyle::Accent(), 150.f);
	const FVector2f BPos(330, 28);
	Node(Out, Layer, G, BPos, TEXT("the object"), TEXT("can be collected"), Red, 150.f);
	Arrow(Out, Layer, G, FVector2f(16 + A.X + 6, 50), BPos + FVector2f(-6, 22), Red, 2.f, true);
	Text(Out, Layer + 2, G, FVector2f(16 + A.X + 12, 28), Fit(Member, 26), SentinelStyle::Mono(9), Red);
	Text(Out, Layer + 2, G, FVector2f(16 + A.X + 12, 56), TEXT("not seen by the GC"), SentinelStyle::Font(false, 9), Red);
	Text(Out, Layer + 2, G, FVector2f(16, 96), Fit(Holds, 80), SentinelStyle::Mono(9), SentinelStyle::Ink());
	if (Row->Where.IsValid() && Row->Where->HasField(TEXT("code")))
	{
		Text(Out, Layer + 2, G, FVector2f(16, 118), Fit(Row->Where->GetStringField(TEXT("code")), 86), SentinelStyle::Mono(9), SentinelStyle::Muted());
	}
}
