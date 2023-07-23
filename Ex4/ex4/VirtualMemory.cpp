#include "MemoryConstants.h"
#include "PhysicalMemory.h"

void initMemory(uint64_t addr) {
    for (uint64_t offset = 0; offset < PAGE_SIZE; offset++) {
        PMwrite(addr * PAGE_SIZE + offset, 0);
    }
}

void VMinitialize() {
    initMemory(0);
}

uint64_t getFrameShift(unsigned int depth) {
    return (TABLES_DEPTH - depth) * OFFSET_WIDTH;
}

void deleteFrameFromParent(word_t parent, uint64_t offset) {
    PMwrite(parent * PAGE_SIZE + (offset & ((1LL << OFFSET_WIDTH) - 1)), 0);
}

uint64_t calculateCyclicDist(uint64_t pageSwappedIn, uint64_t page) {
    uint64_t absDist = pageSwappedIn >= page ? pageSwappedIn - page : page - pageSwappedIn;
    uint64_t leftSide = NUM_PAGES - absDist;
    return leftSide < absDist ? leftSide : absDist;
}

void updateMaxDistance(uint64_t pageSwappedIn, uint64_t pathToPage, uint64_t& maxDist,
                       word_t& maxDistFrame, word_t& maxDistFrameParent,
                       word_t currFrame, uint64_t& finalPathToPage, word_t parentFrame) {

    uint64_t distance = calculateCyclicDist(pageSwappedIn, pathToPage);
    if (maxDist < distance) {
        maxDist = distance;
        maxDistFrame = currFrame;
        maxDistFrameParent = parentFrame;
        finalPathToPage = pathToPage;
    }
}

uint64_t treeTraverse(word_t currFrame, int depth, word_t& framesNum, word_t parentFrame,
                      uint64_t pathToPage, uint64_t pageSwappedIn, uint64_t& maxDist,
                      word_t& maxDistFrame, word_t& maxDistFrameParent, uint64_t sourceFrame,
                      uint64_t & finalPathToPage) {

    framesNum = framesNum < currFrame ? currFrame : framesNum;
    if (depth == TABLES_DEPTH) {
        updateMaxDistance(pageSwappedIn, pathToPage, maxDist, maxDistFrame,
                          maxDistFrameParent, currFrame, finalPathToPage, parentFrame);
        return 0;
    }

    word_t currValue;
    bool foundEmptyTable = true;

    for (unsigned int i = 0; i < PAGE_SIZE; i++) {
        PMread(currFrame * PAGE_SIZE + i, &currValue);
        if (currValue != 0) {
            foundEmptyTable = false;
            uint64_t newPathToPage = pathToPage + (i << ((TABLES_DEPTH - depth - 1) * OFFSET_WIDTH));
            uint64_t currChild = treeTraverse(currValue, depth + 1, framesNum, currFrame,
                                              newPathToPage, pageSwappedIn, maxDist, maxDistFrame,
                                              maxDistFrameParent, sourceFrame, finalPathToPage);
            if (currChild != 0) {
                return currChild;
            }
        }
    }

    if (foundEmptyTable && currFrame != (word_t)sourceFrame) {
        finalPathToPage = pathToPage >> ((TABLES_DEPTH - depth) * OFFSET_WIDTH);
        maxDistFrameParent = parentFrame;

        return currFrame;
    }
    return 0;
}

uint64_t findEmptyFrame(uint64_t pageSwappedIn, uint64_t sourceFrame) {
    uint64_t maxDist = 0, finalPathToPage = 0;
    word_t framesNum = 0, parentFrame = 0, pathToPage = 0, maxDistFrame = 0, maxDistFrameParent = 0;
    uint64_t foundFrame = treeTraverse(0, 0, framesNum, parentFrame, pathToPage, pageSwappedIn,
                                       maxDist, maxDistFrame, maxDistFrameParent, sourceFrame,
                                       finalPathToPage);

    if (foundFrame != 0 && foundFrame != sourceFrame) {
        deleteFrameFromParent(maxDistFrameParent, finalPathToPage);
        return foundFrame;
    }

    if (framesNum + 1 < NUM_FRAMES) {
        return framesNum + 1;
    }

    PMevict(maxDistFrame, finalPathToPage);
    deleteFrameFromParent(maxDistFrameParent, finalPathToPage);
    return maxDistFrame;
}

word_t getFrame(uint64_t virtualAddress) {
    uint64_t currAddr = virtualAddress, prevTable = 0, depth = 0, pageIndex = (virtualAddress >> OFFSET_WIDTH);
    word_t currValue = 0;
    bool emptyTable = false;

    while (depth < TABLES_DEPTH) {
        uint64_t shiftsNeeded = getFrameShift(depth);
        uint64_t currFrame = currAddr >> shiftsNeeded;
        currAddr = (currAddr & ((1LL << shiftsNeeded) - 1));

        if (!emptyTable) {
            PMread(prevTable * PAGE_SIZE + currFrame, &currValue);
        }

        if (emptyTable || currValue == 0) {
            emptyTable = true;
            currValue = (word_t) findEmptyFrame(pageIndex, prevTable);
            if (depth == TABLES_DEPTH - (uint64_t)1) {
                PMrestore(currValue, virtualAddress >> OFFSET_WIDTH);
            } else {
                initMemory(currValue);
            }
            PMwrite(prevTable * PAGE_SIZE + currFrame, currValue);
        }

        prevTable = currValue;
        depth++;
    }

    return currValue;
}

int VMread(uint64_t virtualAddress, word_t* value) {
    if (virtualAddress >= VIRTUAL_MEMORY_SIZE) {
        return 0;
    }

    PMread(getFrame(virtualAddress) * PAGE_SIZE + (virtualAddress % PAGE_SIZE), value);
    return 1;
}

int VMwrite(uint64_t virtualAddress, word_t value) {
    if (virtualAddress >= VIRTUAL_MEMORY_SIZE) {
        return 0;
    }

    PMwrite(getFrame(virtualAddress) * PAGE_SIZE + (virtualAddress % PAGE_SIZE), value);
    return 1;
}
