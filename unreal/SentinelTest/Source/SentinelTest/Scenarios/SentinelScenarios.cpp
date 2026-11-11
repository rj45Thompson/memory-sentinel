// PLANTED HEAP BUGS (runtime). Each lives in its own function, never inlined, so the stack Memory Sentinel reports for the
// bad write names it: that name in the findings file is what the headless test checks.
#include "CoreMinimal.h"

FORCENOINLINE void SentinelPlant_HeapOverrun(uint8* Block, int32 Size)
{
	volatile uint8* P = Block;
	for (int32 I = Size; I < Size + 16; ++I)
	{
		P[I] = uint8(I);   // 16 bytes past the end of a Size-byte block
	}
}

FORCENOINLINE void SentinelPlant_WriteAfterFree(uint8* Block)
{
	volatile uint64* P = reinterpret_cast<volatile uint64*>(Block);
	P[0] = 0xDEADBEEFull;   // the block was freed a second ago
}

FORCENOINLINE void SentinelPlant_DoubleFree(uint8* Block)
{
	FMemory::Free(Block);
	FMemory::Free(Block);
	UE_LOG(LogTemp, Display, TEXT("SentinelTest: freed the block twice"));   // keeps the second free from being a tail call
}

FORCENOINLINE void SentinelPlant_GuardOverrun(uint8* Block, int32 Size)
{
	volatile uint8* P = Block;
	P[Size + 24] = 0x42;   // 24 bytes past the end: on the guard page
}
