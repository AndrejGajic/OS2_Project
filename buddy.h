#ifndef BUDDY_H
#define BUDDY_H

#include "defines.h"

typedef struct BuddyData {
	uchar data[BLOCK_SIZE - sizeof(unsigned)];
	unsigned index;
}Data;

typedef union BuddyBlock {
	union BuddyBlock* next; // ako je blok slobodan, za ulancavanje
	//Data data; // ako je blok zauzet
	uchar data[BLOCK_SIZE];  // ako je blok zauzet
}Block;

typedef struct BuddyArrayNode {
	Block* head;
	Block* tail;
}BuddyArrayNode;

typedef struct BuddyAllocator {
	Block* begin; // adresa prvog bloka u raspolozivoj memoriji
	unsigned numOfBlocks; // koliko ima ukupno blokova u sistemu
	unsigned numOfFreeBlocks; // koliko ima slobodnih blokova (blokova pod nadzorom alokatora)
	BuddyArrayNode buddyArray[MAX_BUDDY_POWER];  // niz listi sortiranih po stepenima dvojke (0 -> velicina 1, 1 -> velicina 2...)
	unsigned maxPower; // najveci stepen dvojke velicine bloka u nizu
	HANDLE buddyAllocatorMutex; // za sinhronizaciju
}BuddyAllocator;

BuddyAllocator* buddyAllocator;

BuddyAllocator* buddyInit(void* address, int size); // inicijalizuje alokator i deli blokove po velicini
void putBlock(void* address, int size);
Block* getBlock(int size);
void printBuddyInfo();
Block* giveSlabAllocationBlock();
Block* giveBlockForCaches();

#endif