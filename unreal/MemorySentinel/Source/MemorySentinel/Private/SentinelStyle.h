// The panel's colours and fonts, in one place. Dark, so it reads the same over a game viewport and docked in the editor;
// each problem keeps one colour everywhere (the chips, the list's edge, the diagrams, and the visual report).
#pragma once

#include "CoreMinimal.h"
#include "Styling/CoreStyle.h"

namespace SentinelStyle
{
	inline FLinearColor Hex(const TCHAR* H) { return FLinearColor::FromSRGBColor(FColor::FromHex(H)); }

	inline FLinearColor Page() { return Hex(TEXT("0E1116")); }
	inline FLinearColor Well() { return Hex(TEXT("141920")); }
	inline FLinearColor Card() { return Hex(TEXT("1B222B")); }
	inline FLinearColor Line() { return Hex(TEXT("2A3340")); }
	inline FLinearColor Ink() { return Hex(TEXT("E7EAEE")); }
	inline FLinearColor Muted() { return Hex(TEXT("94A0AF")); }
	inline FLinearColor Accent() { return Hex(TEXT("5CC8FF")); }
	inline FLinearColor Good() { return Hex(TEXT("3DD68C")); }

	inline FLinearColor Severity(const FString& S)
	{
		if (S == TEXT("error")) { return Hex(TEXT("FF5C5C")); }
		if (S == TEXT("warning")) { return Hex(TEXT("FFB547")); }
		return Hex(TEXT("7A8BFF"));
	}

	inline FLinearColor Problem(const FString& P)
	{
		if (P == TEXT("dangling")) { return Hex(TEXT("FF8A5C")); }
		if (P == TEXT("cycle")) { return Hex(TEXT("B58CFF")); }
		if (P == TEXT("leak")) { return Hex(TEXT("2FD1C0")); }
		if (P == TEXT("stomp")) { return Hex(TEXT("FF5C9A")); }
		return Hex(TEXT("FF5C5C"));
	}

	inline FString ProblemName(const FString& P)
	{
		if (P == TEXT("dangling")) { return TEXT("Dangling pointers"); }
		if (P == TEXT("cycle")) { return TEXT("Reference cycles"); }
		if (P == TEXT("leak")) { return TEXT("Outlived their level"); }
		if (P == TEXT("stomp")) { return TEXT("Memory stomps"); }
		return TEXT("Crashes");
	}

	inline FSlateFontInfo Font(bool bBold, int32 Size) { return FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size); }
	inline FSlateFontInfo Mono(int32 Size) { return FCoreStyle::GetDefaultFontStyle("Mono", Size); }
}
