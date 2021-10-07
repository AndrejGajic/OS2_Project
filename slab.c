#include "slab.h"
#define _CRT_SECURE_NO_WARNINGS
typedef enum ErrorType { NO_ERR, ALLOC_ERROR, NULLPTR_ERROR, NO_MEMORY_ERROR, LIST_ERROR }ErrorType;

typedef struct Slab {
	struct Slab* next;
	kmem_cache_t* parentCache; // pokazivac na kes u kome se nalazi
	unsigned numOfFreeSlots; // broj slobodnih slotova u slabu
	unsigned* bitvector; // pokazivac na bitvektor, njegova adresa krece odmah nakon metapodataka
	void* data; // pokazivac na slobodan prostor, njegova adresa krece odmah nakon bitvektora
}Slab;

typedef struct SlabListHeader { // da ne bi pisali posebne operacije liste za svaku listu ponaosob (puna, prazna, delimicno puna)
	Slab* head;
}SlabListHeader;

typedef struct kmem_cache_s {
	struct kmem_cache_s* next;
	char name[30];
	unsigned sizeOfSlabInBlocks; // velicina jednog slaba u blokovima
	unsigned sizeOfObjects; // u bajtovima
	unsigned sizeOfBitvector; // velicina bitvektora
	unsigned numOfSlots; // broj slotova u slabovima ILI broj keseva ako je u pitanju glavni kes 
	unsigned freeFragmentSize; // velicina neiskoriscenog dela u svakom slabu
	void (*ctor)(void*); // konstruktor objekata ovog kesa
	void (*dtor)(void*); // destruktor objekata ovog kesa
	SlabListHeader fullSlabs; // lista punih slabova
	SlabListHeader emptySlabs; // lista praznih slabova
	SlabListHeader partialSlabs; // lista delimicno popunjenih slabova
	ErrorType error; // ako se desila neka greska cuva se u ovom polju
	HANDLE cacheMutex; // za sinhronizaciju
	unsigned differentAlignments;
}kmem_cache_t;

typedef struct BufferCache {
	size_t size;
	kmem_cache_t* cache;
}BufferCache;

typedef struct SlabAllocator {
	BuddyAllocator* myBuddyAllocator;
	kmem_cache_t* headCache; // glava liste svih keseva za korisnicke objekte
	BufferCache bufferCaches[SMALL_MEM_BUFFER_MAX_SIZE]; // niz keseva za male memorijske bafere
	unsigned metadataSize; // velicina metapodataka u slabovima
	HANDLE slabAllocatorMutex; // za sinhronizaciju
	HANDLE printMutex;
	HANDLE bufferCacheMutex;
	HANDLE listMutex;
	HANDLE objMutex;
}SlabAllocator;

SlabAllocator* slabAllocator;



unsigned calculateSlabSize(size_t objectSize) { // racuna zeljenu velicinu slaba
	unsigned size = BLOCK_SIZE;
	unsigned desiredObjectCount = DESIRED_OBJECT_COUNT;
	//if (objectSize >= 4096) desiredObjectCount >>= 1;
	if (objectSize >= 8192) desiredObjectCount >>= 1;
	if (objectSize >= 16384) desiredObjectCount >>= 1;
	if (objectSize >= 32768) desiredObjectCount >>= 1;
	if (objectSize >= 65536) desiredObjectCount >>= 1;
	while (size < objectSize * desiredObjectCount) {
		size <<= 1;
	}
	while (size > BLOCK_SIZE* BLOCK_NUMBER) {
		size >>= 1;
	}
	if (size < objectSize) return 0;
	return size;
}

void calculateSizes(kmem_cache_t* cache) {
	unsigned sizeWithoutMetadata = cache->sizeOfSlabInBlocks * BLOCK_SIZE - slabAllocator->metadataSize - 2;
	if (cache->sizeOfObjects > 100000) {
		printf("kurac");
	}
	unsigned potentialSlots = sizeWithoutMetadata / (cache->sizeOfObjects + 1); // svaki objekat ce imati svoj slot velicine sizeOfObjects i 1 bit u bitvektoru
	//if (cache->sizeOfObjects < 10) potentialSlots -= 8;
	unsigned sizeOfBitvector = potentialSlots;
	if (potentialSlots % 32 != 0) sizeOfBitvector = potentialSlots + 32 - potentialSlots % 32;
	while ((sizeOfBitvector + potentialSlots * cache->sizeOfObjects) > sizeWithoutMetadata) {
		potentialSlots--;
	}
	unsigned freeFragmentSize = sizeWithoutMetadata - sizeOfBitvector - potentialSlots * cache->sizeOfObjects;
	cache->sizeOfBitvector = sizeOfBitvector;
	cache->numOfSlots = potentialSlots;
	cache->freeFragmentSize = freeFragmentSize;
}

unsigned calculateObjectIndexInSlab(Slab* slab, unsigned address) {
	unsigned index = address - (unsigned)slab->data;
	index /= slab->parentCache->sizeOfObjects;
	return index;
}

void initHeadCache() {
	kmem_cache_t* cache = (kmem_cache_t*)giveBlockForCaches();
	cache->next = 0;
	strcpy_s(cache->name, 30, "HeadCache");
	cache->sizeOfSlabInBlocks = 0;
	cache->sizeOfObjects = sizeof(kmem_cache_t);
	cache->sizeOfBitvector = 0;
	cache->numOfSlots = 0;
	cache->freeFragmentSize = 0;
	cache->ctor = 0;
	cache->dtor = 0;
	cache->fullSlabs.head = 0;
	cache->emptySlabs.head = 0;
	cache->partialSlabs.head = 0;
	cache->error = NO_ERR;
	cache->cacheMutex = CreateMutex(NULL, FALSE, NULL);
	cache->differentAlignments = 0;
	slabAllocator->headCache = cache;
}

void initBufferCaches() {
	unsigned size = 32;
	for (unsigned i = 0; i < SMALL_MEM_BUFFER_MAX_SIZE; i++, size <<= 1) {
		slabAllocator->bufferCaches[i].size = size;
		slabAllocator->bufferCaches[i].cache = 0;
	}
}

void insertIntoList(SlabListHeader* list, Slab* slab) {
	WaitForSingleObject(slabAllocator->listMutex, INFINITE);
	slab->next = 0;
	if (!list->head) list->head = slab;
	else {
		/*Slab* temp = list->head;
		while (temp->next) temp = temp->next;
		temp->next = slab;
		*/
		slab->next = list->head;
		list->head = slab;
	}
	ReleaseMutex(slabAllocator->listMutex);
}

void deallocateSlabsFromList(SlabListHeader* list) {
	WaitForSingleObject(slabAllocator->listMutex, INFINITE);
	Slab* toFree = list->head;
	list->head = 0;
	while (toFree) {
		Slab* nextToFree = toFree->next;
		toFree->next = 0;
		putBlock(toFree, toFree->parentCache->sizeOfSlabInBlocks);
		toFree = nextToFree;
	}
	ReleaseMutex(slabAllocator->listMutex);
}

int createNewSlab(kmem_cache_t* cache) {
	WaitForSingleObject(cache->cacheMutex, INFINITE);
	Slab* slab = getBlock(cache->sizeOfSlabInBlocks);
	if (!slab) {
		cache->error = NO_MEMORY_ERROR;
		ReleaseMutex(cache->cacheMutex);
		exit(-4);
		//return -1;
	}
	slab->parentCache = cache;
	slab->numOfFreeSlots = cache->numOfSlots;
	slab->next = 0;
	slab->bitvector = (unsigned*)((unsigned)slab + slabAllocator->metadataSize + 1);
	if (USING_L1_ALIGNMENT && cache->differentAlignments) {
		int additionalOffset = abs(rand() % cache->differentAlignments);
		slab->data = (void*)((unsigned)slab->bitvector + cache->sizeOfBitvector + 1 + additionalOffset);
	}
	else {
		slab->data = (void*)((unsigned)slab->bitvector + cache->sizeOfBitvector + 1);
	}
	/*printf("Pocetna adresa slaba: %d\n", (unsigned)slab);
	printf("Velicina metapodataka: %d\n", slabAllocator->metadataSize);
	printf("Pocetna adresa bitvektora: %d\n", (unsigned)slab->bitvector);
	printf("Velicina bitvektora: %d\n", cache->sizeOfBitvector);
	printf("Pocetna adresa podataka: %d\n", (unsigned)slab->data);
	*/
	for (int i = 0; i < cache->sizeOfBitvector / 32; i++) {
		slab->bitvector[i] = 0;
	}
	insertIntoList(&cache->emptySlabs, slab);
	ReleaseMutex(cache->cacheMutex);
	return 0;
}

void* allocateObject(Slab* slab) { // alocira objekat na prvom slobodnom slotu u slabu
	WaitForSingleObject(slabAllocator->objMutex, INFINITE);
	for (int i = 0; i < slab->parentCache->sizeOfBitvector / 32; i++) {
		unsigned temp1 = slab->bitvector[i];
		for (int j = 0; j < 32; j++) {
			if (i * 32 + j > slab->parentCache->numOfSlots - 1) {
				ReleaseMutex(slabAllocator->objMutex);
				return 0;
			}
			unsigned mask = 1 << j;
			if (!(temp1 & mask)) {
				slab->bitvector[i] |= mask;
				ReleaseMutex(slabAllocator->objMutex);
				return (void*)((unsigned)slab->data + (i * 32 + j) * slab->parentCache->sizeOfObjects);
			}
		}
	}
	ReleaseMutex(slabAllocator->objMutex);
	return 0;
}

Slab* findAndDeallocateObject(kmem_cache_t* cache, void* obj) { // pronalazi slab u kojem se nalazi alocirani objekat obj i dealocira ga
	WaitForSingleObject(cache->cacheMutex, INFINITE);
	unsigned address = (unsigned)obj;
	Slab* slab = cache->partialSlabs.head;
	while (slab) {
		unsigned slabAddress = (unsigned)slab;
		if (address > slabAddress && address < slabAddress + cache->sizeOfSlabInBlocks * BLOCK_SIZE) { // objekat se nalazi u ovom slabu
			unsigned index = calculateObjectIndexInSlab(slab, address);
			unsigned mask = 1 << (index % 32);
			unsigned temp = slab->bitvector[index / 32];
			if (!(temp & mask)) return 0; // poslat je pokazivac na slobodan prostor, nista ne treba dealocirati!
			mask = ~(1 << (index % 32));
			slab->bitvector[index / 32] &= mask; // podesen odgovarajuci bit u bitvektoru na 0, sto znaci da je taj prostor proglasen slobodnim
			ReleaseMutex(cache->cacheMutex);
			return slab;
		}
		slab = slab->next;
	}
	slab = cache->fullSlabs.head;
	while (slab) {
		unsigned slabAddress = (unsigned)slab;
		if (address > slabAddress&& address < slabAddress + cache->sizeOfSlabInBlocks * BLOCK_SIZE) {
			unsigned index = calculateObjectIndexInSlab(slab, address);
			unsigned mask = 1 << (index % 32);
			unsigned temp = slab->bitvector[index / 32];
			if (!(temp & mask)) return 0;
			mask = ~(1 << (index % 32));
			slab->bitvector[index / 32] &= mask;
			ReleaseMutex(cache->cacheMutex);
			return slab;
		}
		slab = slab->next;
	}
	ReleaseMutex(cache->cacheMutex);
	return 0;
}

kmem_cache_t* createBufferCache(unsigned size, unsigned pow) {
	WaitForSingleObject(slabAllocator->bufferCacheMutex, INFINITE);
	kmem_cache_t* cache;
	unsigned disp = pow - 5;
	unsigned address = (unsigned)slabAllocator + sizeof(SlabAllocator) + 1 + (unsigned)((sizeof(kmem_cache_t) + 1) * disp);
	cache = (kmem_cache_t*)address;
	cache->next = 0;
	sprintf_s(cache->name, 20, "size%d", pow);
	cache->sizeOfSlabInBlocks = calculateSlabSize(size) / BLOCK_SIZE;
	cache->sizeOfObjects = size;
	cache->freeFragmentSize = 0;
	cache->ctor = 0;
	cache->dtor = 0;
	cache->fullSlabs.head = 0;
	cache->emptySlabs.head = 0;
	cache->partialSlabs.head = 0;
	cache->error = NO_ERR;
	cache->cacheMutex = CreateMutex(NULL, FALSE, NULL);
	calculateSizes(cache);
	cache->differentAlignments = cache->freeFragmentSize / CACHE_L1_LINE_SIZE;
	ReleaseMutex(slabAllocator->bufferCacheMutex);
	return cache;
}

kmem_cache_t* findBufferCache(void* obj, Slab** retSlab) {
	WaitForSingleObject(slabAllocator->bufferCacheMutex, INFINITE);
	for (int i = 0; i < SMALL_MEM_BUFFER_MAX_SIZE; i++) {
		kmem_cache_t* cache = slabAllocator->bufferCaches[i].cache;
		if (!cache) continue;
		Slab* slab = findAndDeallocateObject(cache, obj);
		if (!slab) continue;
		*retSlab = slab;
		ReleaseMutex(slabAllocator->bufferCacheMutex);
		return cache;
	}
	ReleaseMutex(slabAllocator->bufferCacheMutex);
	return 0;
}

void kmem_init(void* space, int block_num) {
	if (!space || block_num < 5) return;
	buddyAllocator = buddyInit(space, block_num);
	slabAllocator = (SlabAllocator*)giveSlabAllocationBlock();
	slabAllocator->myBuddyAllocator = buddyAllocator;
	slabAllocator->metadataSize = sizeof(Slab*) + sizeof(kmem_cache_t*) + sizeof(unsigned) + sizeof(unsigned*) + sizeof(uchar*);
	slabAllocator->slabAllocatorMutex = CreateMutex(NULL, FALSE, NULL);
	WaitForSingleObject(slabAllocator->slabAllocatorMutex, INFINITE);
	slabAllocator->listMutex = CreateMutex(NULL, FALSE, NULL);
	slabAllocator->printMutex = CreateMutex(NULL, FALSE, NULL);
	slabAllocator->bufferCacheMutex = CreateMutex(NULL, FALSE, NULL);
	slabAllocator->objMutex = CreateMutex(NULL, FALSE, NULL);
	initHeadCache();
	initBufferCaches();
	ReleaseMutex(slabAllocator->slabAllocatorMutex);
}



kmem_cache_t* kmem_cache_create(const char* name, size_t size, void (*ctor)(void*), void (*dtor)(void*)) {
	//WaitForSingleObject(slabAllocator->slabAllocatorMutex, INFINITE);
	unsigned slabSize = calculateSlabSize(size);
	unsigned cnt = 1;
	kmem_cache_t* temp = slabAllocator->headCache;
	while (temp->next) {
		temp = temp->next;
		cnt++;
	}
	unsigned addressOfCache = (unsigned)slabAllocator->headCache + cnt * sizeof(kmem_cache_t);
	kmem_cache_t* cache = (kmem_cache_t*)addressOfCache;
	temp->next = cache;
	cache->next = 0;
	strcpy_s(cache->name, 30, name);
	cache->sizeOfSlabInBlocks = slabSize / BLOCK_SIZE;
	cache->sizeOfObjects = size;
	cache->freeFragmentSize = 0;
	cache->ctor = ctor;
	cache->dtor = dtor;
	cache->fullSlabs.head = 0;
	cache->emptySlabs.head = 0;
	cache->partialSlabs.head = 0;
	cache->error = NO_ERR;
	cache->cacheMutex = CreateMutex(NULL, FALSE, NULL);
	calculateSizes(cache);
	cache->differentAlignments = cache->freeFragmentSize / CACHE_L1_LINE_SIZE;
	//ReleaseMutex(slabAllocator->slabAllocatorMutex);
}

int kmem_cache_shrink(kmem_cache_t* cachep) {
	if (!cachep->emptySlabs.head) return 0;
	WaitForSingleObject(cachep->cacheMutex, INFINITE);
	Slab* toFree = cachep->emptySlabs.head;
	cachep->emptySlabs.head = 0;
	int ret = 0;
	while (toFree) {
		Slab* nextToFree = toFree->next;
		toFree->next = 0;
		putBlock(toFree, cachep->sizeOfSlabInBlocks);
		ret += cachep->sizeOfSlabInBlocks;
		toFree = nextToFree;
	}
	ReleaseMutex(cachep->cacheMutex);
	return ret;
}

void* kmem_cache_alloc(kmem_cache_t* cachep) {
	WaitForSingleObject(cachep->cacheMutex, INFINITE);
	if (!cachep->emptySlabs.head && !cachep->partialSlabs.head) {
		if (createNewSlab(cachep) == -1) {
			cachep->error = NO_MEMORY_ERROR;
			ReleaseMutex(cachep->cacheMutex);
			return 0;
		}
	}
	void* obj = 0;
	Slab* slab;
	if (cachep->partialSlabs.head) {
		slab = cachep->partialSlabs.head;
		obj = allocateObject(slab);
		if (!obj) {
			cachep->error = ALLOC_ERROR;
			ReleaseMutex(cachep->cacheMutex);
			exit(-2);
			//return 0;
		}
		slab->numOfFreeSlots--;
		if (slab->numOfFreeSlots == 0) {
			cachep->partialSlabs.head = slab->next;
			slab->next = 0;
			insertIntoList(&cachep->fullSlabs, slab);
		}
	}
	else if (cachep->emptySlabs.head) {
		slab = cachep->emptySlabs.head;
		obj = allocateObject(slab);
		if (!obj) {
			cachep->error = ALLOC_ERROR;
			ReleaseMutex(cachep->cacheMutex);
			exit(-3);
			//return 0;
		}
		slab->numOfFreeSlots--;
		cachep->emptySlabs.head = slab->next;
		slab->next = 0;
		insertIntoList(&cachep->partialSlabs, slab);
	}
	if (cachep->ctor) cachep->ctor(obj);
	//printf("Objekat alociran na pocetnoj adresi: %d\n", (unsigned)obj);
	ReleaseMutex(cachep->cacheMutex);
	return obj;
}

void kmem_cache_free(kmem_cache_t* cachep, void* objp) {
	WaitForSingleObject(cachep->cacheMutex, INFINITE);
	Slab* slab = findAndDeallocateObject(cachep, objp);
	if (!slab) {
		cachep->error = ALLOC_ERROR;
		ReleaseMutex(cachep->cacheMutex);
		return;
	}
	slab->numOfFreeSlots++;
	//printf("Objekat dealociran sa adrese: %d\n", (unsigned)objp);
	if (slab->numOfFreeSlots == 1) { // bio pun, postao delimicno popunjen
		if (slab == cachep->fullSlabs.head) {
			cachep->fullSlabs.head = slab->next;
		}
		else {
			Slab* temp = cachep->fullSlabs.head;
			while (temp->next && temp->next != slab) temp = temp->next;
			if (!temp->next) {
				cachep->error = LIST_ERROR;
				ReleaseMutex(cachep->cacheMutex);
				return;
			}
			temp->next = slab->next;
		}
		slab->next = 0;
		insertIntoList(&cachep->partialSlabs, slab);
	}
	else if (slab->numOfFreeSlots == cachep->numOfSlots) { // postao skroz prazan
		if (slab == cachep->partialSlabs.head) {
			cachep->partialSlabs.head = slab->next;
		}
		else {
			Slab* temp = cachep->partialSlabs.head;
			while (temp->next && temp->next != slab) temp = temp->next;
			if (!temp->next) {
				cachep->error = LIST_ERROR;
				ReleaseMutex(cachep->cacheMutex);
				return;
			}
			temp->next = slab->next;
		}
		slab->next = 0;
		insertIntoList(&cachep->emptySlabs, slab);
	}
	if (cachep->dtor) cachep->dtor(objp);
	//if (cachep->ctor) cachep->ctor(objp);
	ReleaseMutex(cachep->cacheMutex);
}

void* kmalloc(size_t size) {
	WaitForSingleObject(slabAllocator->bufferCacheMutex, INFINITE);
	if (size < 32) size = 32;
	else if (size > pow(2, SMALL_MEM_BUFFER_MAX_SIZE + 4)) size = pow(2, SMALL_MEM_BUFFER_MAX_SIZE + 4);
	unsigned temp = 1, pow = 0;
	while (temp < size) {
		temp <<= 1;
		pow++;
	}
	kmem_cache_t* cache = slabAllocator->bufferCaches[pow - 5].cache;
	if (!cache) {
		cache = createBufferCache(size, pow);
		slabAllocator->bufferCaches[pow - 5].cache = cache;
	}
	ReleaseMutex(slabAllocator->bufferCacheMutex);
	return kmem_cache_alloc(cache);
}

void kfree(const void* objp) {
	WaitForSingleObject(slabAllocator->bufferCacheMutex, INFINITE);
	Slab* slab;
	kmem_cache_t* cachep = findBufferCache(objp, &slab);
	ReleaseMutex(slabAllocator->bufferCacheMutex);
	if (!cachep) {
		return;
	}
	WaitForSingleObject(cachep->cacheMutex, INFINITE);
	slab->numOfFreeSlots++;
	//printf("Objekat dealociran sa adrese: %d\n", (unsigned)objp);
	if (slab->numOfFreeSlots == 1) { // bio pun, postao delimicno popunjen
		if (slab == cachep->fullSlabs.head) {
			cachep->fullSlabs.head = slab->next;
		}
		else {
			Slab* temp = cachep->fullSlabs.head;
			while (temp->next && temp->next != slab) temp = temp->next;
			if (!temp->next) {
				cachep->error = LIST_ERROR;
				ReleaseMutex(cachep->cacheMutex);
				return;
			}
			temp->next = slab->next;
		}
		slab->next = 0;
		insertIntoList(&cachep->partialSlabs, slab);
	}
	else if (slab->numOfFreeSlots == cachep->numOfSlots) { // postao skroz prazan
		if (slab == cachep->partialSlabs.head) {
			cachep->partialSlabs.head = slab->next;
		}
		else {
			Slab* temp = cachep->partialSlabs.head;
			while (temp->next && temp->next != slab) temp = temp->next;
			if (!temp->next) {
				cachep->error = LIST_ERROR;
				ReleaseMutex(cachep->cacheMutex);
				return;
			}
			temp->next = slab->next;
		}
		slab->next = 0;
		insertIntoList(&cachep->emptySlabs, slab);
	}
	ReleaseMutex(cachep->cacheMutex);
}

void kmem_cache_destroy(kmem_cache_t* cachep) {
	if (!cachep) return;
	WaitForSingleObject(cachep->cacheMutex, INFINITE);
	deallocateSlabsFromList(&cachep->emptySlabs);
	deallocateSlabsFromList(&cachep->partialSlabs);
	deallocateSlabsFromList(&cachep->fullSlabs);
	if (cachep == slabAllocator->headCache) {
		slabAllocator->headCache = cachep->next;
	}
	else {
		kmem_cache_t* temp = slabAllocator->headCache;
		while (temp->next && temp->next != cachep) temp = temp->next;
		if (!temp->next) return;
		temp->next = cachep->next;
	}
	cachep->next = 0;
	ReleaseMutex(cachep->cacheMutex);
	CloseHandle(cachep->cacheMutex);
}

void kmem_cache_info(kmem_cache_t* cachep) {
	if (!cachep) return;
	WaitForSingleObject(slabAllocator->printMutex, INFINITE);
	unsigned numOfSlabs = 0, numOfFreeSlots = 0;
	Slab* temp = cachep->emptySlabs.head;
	while (temp) {
		numOfSlabs++;
		numOfFreeSlots += cachep->numOfSlots;
		temp = temp->next;
	}
	temp = cachep->partialSlabs.head;
	while (temp) {
		numOfSlabs++;
		numOfFreeSlots += temp->numOfFreeSlots;
		temp = temp->next;
	}
	temp = cachep->fullSlabs.head;
	while (temp) {
		numOfSlabs++;
		temp = temp->next;
	}
	unsigned numOfSlots = numOfSlabs * cachep->numOfSlots;
	printf("\n");
	printf("CACHE INFO: \n");
	printf("Name: %s\n", cachep->name);
	printf("Object size: %dB\n", cachep->sizeOfObjects);
	printf("Size of cache in blocks: %d\n", cachep->sizeOfSlabInBlocks * numOfSlabs);
	printf("Number of slabs in cache: %d\n", numOfSlabs);
	printf("Number of objects in slab: %d\n", cachep->numOfSlots);
	printf("Cache occupancy: %.3f%c", (double)(numOfSlots - numOfFreeSlots) / numOfSlots * 100, '%');
	ReleaseMutex(slabAllocator->printMutex);
}

int kmem_cache_error(kmem_cache_t* cachep) {
	WaitForSingleObject(slabAllocator->printMutex, INFINITE);
	return (int)cachep->error;
	ReleaseMutex(slabAllocator->printMutex);
}