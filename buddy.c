#include "buddy.h"

unsigned findPowerOf2(int num) {
	if (num == 0) return 0;
	int pow = 0;
	while (num) {
		pow++;
		num /= 2;
	}
	return pow - 1;
}

unsigned findMaxPowerOf2(int num) {
	if (num == 0) return 0;
	int pow = 0;
	while (num) {
		pow++;
		num /= 2;
	}
	return pow;
}

unsigned firstNextPower(int num) {
	if (num == 0) return 0;
	unsigned power = findPowerOf2(num);
	if (num != pow(2, power)) {
		power++;
		num = pow(2, power);
	}
	return num;
}

unsigned inline calculateIndexOfBlock(Block* block) {
	return ((unsigned)block - (unsigned)buddyAllocator->begin) / BLOCK_SIZE;
}


void printBuddyInfo() {
	for (int i = 0; i < buddyAllocator->maxPower; i++) {
		Block* temp = buddyAllocator->buddyArray[i].head;
		if (!temp) printf("Nema blokova stepena %d!\n", i);
		else {
			printf("Stepen %d: ", i);
			while (temp) {
				unsigned tempIndex = calculateIndexOfBlock(temp);
				unsigned endIndex = tempIndex + pow(2, i) - 1;
				printf("[%d - %d]", tempIndex, endIndex);
				temp = temp->next;
				if (temp) printf(", ");
			}
			printf("\n");
		}
	}
	printf("\n");
}

unsigned findBuddyIndex(unsigned blockIndex, int size) {
	unsigned buddyIndex;
	// trenutni blok je levi blok, buddy-ja trazimo levo
	if (!(blockIndex % (size * 2))) {
		buddyIndex = blockIndex + size;
	}
	// trenutni blok je desni blok, buddy-ja trazimo desno
	else buddyIndex = blockIndex - size;
	return buddyIndex;
}

Block* findBuddy(int buddyIndex, int power) {
	Block* temp = buddyAllocator->buddyArray[power].head;
	unsigned tempIndex;
	Block* prev = 0;
	while (temp) {
		tempIndex = calculateIndexOfBlock(temp);
		if (tempIndex == buddyIndex) {
			if (!prev) { // prvi element u listi
				if (temp == buddyAllocator->buddyArray[power].tail) { // jedini element u listi
					buddyAllocator->buddyArray[power].head = buddyAllocator->buddyArray[power].tail = 0;
				}
				else {
					buddyAllocator->buddyArray[power].head = temp->next;
				}
			}
			else { // nije prvi element u listi i nije jedini element
				if (temp == buddyAllocator->buddyArray[power].tail) { // poslednji element u listi
					buddyAllocator->buddyArray[power].tail = prev;
					prev->next = 0;
				}
				else {
					prev->next = temp->next;
				}
			}
			temp->next = 0;
			buddyAllocator->numOfFreeBlocks -= (unsigned)pow(2, power);
			return temp;
		}
		prev = temp;
		temp = temp->next;
	}
	return 0;
}

void putBlock(void* address, int size) {
	if (!address) return;
	WaitForSingleObject(buddyAllocator->buddyAllocatorMutex, INFINITE);
	Block* currBlock = (Block*)address;
	unsigned blockIndex = calculateIndexOfBlock(currBlock);
	unsigned indexInArray = findPowerOf2(size);
	if (indexInArray > buddyAllocator->maxPower) return;
	unsigned buddyIndex = findBuddyIndex(blockIndex, size);
	if (buddyAllocator->buddyArray[indexInArray].head == 0) {
		// sigurno nema parnjaka, mozemo samo da insertujemo blok na odgovarajuce mesto
		//printf("Blok %d nema badija\n", blockIndex);
		buddyAllocator->buddyArray[indexInArray].head = buddyAllocator->buddyArray[indexInArray].tail = currBlock;
		currBlock->next = 0;
		buddyAllocator->numOfFreeBlocks += size;
		ReleaseMutex(buddyAllocator->buddyAllocatorMutex);
		return;
	}
	Block* buddy = findBuddy(buddyIndex, indexInArray);
	buddyIndex = calculateIndexOfBlock(buddy);
	if (!buddy) {
		// nas blok nema parnjaka medju slobodnim blokovima, mozemo da ga ubacimo u niz
		//printf("Blok %d nema badija\n", blockIndex);
		currBlock->next = buddyAllocator->buddyArray[indexInArray].head;
		buddyAllocator->buddyArray[indexInArray].head = currBlock;
		buddyAllocator->numOfFreeBlocks += size;
	}
	else {
		//printf("Blok %d ima badija %d\n", currBlock->data.index, buddy->data.index);
		if (blockIndex < buddyIndex) putBlock((void*)currBlock, 2 * size, buddyAllocator);
		else putBlock((void*)buddy, 2 * size, buddyAllocator);
	}
	ReleaseMutex(buddyAllocator->buddyAllocatorMutex);
}


Block* getBlock(int size) {
	WaitForSingleObject(buddyAllocator->buddyAllocatorMutex, INFINITE);
	size = firstNextPower(size);
	if (size > buddyAllocator->numOfFreeBlocks) {
		ReleaseMutex(buddyAllocator->buddyAllocatorMutex);
		return 0; // nema dovoljno slobodnih blokova
	}
	unsigned power = findPowerOf2(size);
	unsigned initialSize = size;
	unsigned initialPower = power;
	bool flag = false;
	bool change = false;
	Block* toGive = 0;
	while (!flag) {
		if (size > buddyAllocator->numOfFreeBlocks) return 0; // nije nadjen blok
		if (!buddyAllocator->buddyArray[power].head) { // nema blokova ove velicine, treba traziti vece blokove
			size *= 2;
			power++;
			change = true;
		}
		else { // pronadjen blok
			toGive = buddyAllocator->buddyArray[power].head;
			if (buddyAllocator->buddyArray[power].tail == toGive) { // jedini blok u ovoj listi
				buddyAllocator->buddyArray[power].head = buddyAllocator->buddyArray[power].tail = 0;
			}
			else {
				buddyAllocator->buddyArray[power].head = toGive->next;
			}
			toGive->next = 0;
			flag = true;
		}
	}
	if(change) {
		unsigned disp = 1 << initialPower;
		Block* toReturn = 0;
		for (int i = initialPower; i < power; i++) { // moramo da razlomimo blokove i da ih vratimo u listu
			toReturn = toGive + disp;
			toReturn->next = buddyAllocator->buddyArray[i].head;
			buddyAllocator->buddyArray[i].head = buddyAllocator->buddyArray[i].tail = toReturn;
			//printf("Vracen blok sa indeksom %d\n", toReturn->data.index);
			disp *= 2;
		}
	}
	buddyAllocator->numOfFreeBlocks -= initialSize;
	//printf("Uzet blok sa indeksom %d!\n\n", toGive->data.index);
	ReleaseMutex(buddyAllocator->buddyAllocatorMutex);
	return toGive;
}


BuddyAllocator* buddyInit(void* address, int size) {
	int pow = findMaxPowerOf2(size);
	if (pow > MAX_BUDDY_POWER || size < 4) return NULL;
	buddyAllocator = (BuddyAllocator*)address;
	buddyAllocator->begin = (Block*)address + 4; // prvi blok je za buddyAllocator, drugi blok je za slabAllocator, treci za keseve
	buddyAllocator->maxPower = pow;
	buddyAllocator->numOfBlocks = size - 4;
	buddyAllocator->numOfFreeBlocks = 0;
	// inicijalizacija head-ova liste na null vrednosti, jer je u pocetku niz blokova prazan
	for (int i = 0; i < buddyAllocator->maxPower; i++) {
		buddyAllocator->buddyArray[i].head = buddyAllocator->buddyArray[i].tail = 0;
	}
	// pocetno dodavanje raspolozive memorije u slobodni memorijski prostor
	for (int i = 0; i < buddyAllocator->numOfBlocks; i++) {
		putBlock(buddyAllocator->begin + i, 1, buddyAllocator);
	}
	buddyAllocator->buddyAllocatorMutex = CreateMutex(NULL, FALSE, NULL);
	return buddyAllocator;
}

Block* giveSlabAllocationBlock() {
	return (Block*)(buddyAllocator) + 1;
}

Block* giveBlockForCaches() {
	return (Block*)(buddyAllocator) + 2;
}