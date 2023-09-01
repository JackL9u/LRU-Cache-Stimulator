#include "cachelab.h"
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*use double-linked list, lines connected to each other.*/
typedef struct line_t {
    bool valid;
    bool dirty;
    unsigned long tag;
    struct line_t *prev;
    struct line_t *next;
} line;

/*In each set, the head is the least-recently used,
the tail is the most=recently used*/
typedef struct set_t {
    line *head;
    line *tail;
} set;

unsigned long hits = 0, misses = 0, evictions = 0, dirty_bytes = 0,
              dirty_evictions = 0;

/*calculate 2^s, since we only need power of 2.*/
unsigned int twoPow(int s) {
    int temp = 1;
    for (int i = 0; i < s; i++) {
        temp = temp * 2;
    }
    return (unsigned int)temp;
}

/*build a single set, all the sets made up the cache*/
void buildSet(set **cache, int lineNum, unsigned int i) {
    cache[i] = (set *)malloc(sizeof(set));
    line *last, *current = NULL;

    for (int j = 0; j < lineNum; j++) {
        current = (line *)malloc(sizeof(line));
        current->valid = false;
        current->dirty = false;
        current->tag = 0;
        current->prev = (j > 0) ? last : NULL;
        current->next = NULL;
        if (j > 0) {
            last->next = current;
        } else {
            cache[i]->head = current;
        }
        last = current;
    }
    cache[i]->tail = current;
}

/*for any address, get it's set index.*/
unsigned long getSetIdx(long addre, int setBit, int blockBit) {
    if (setBit > 0) {
        unsigned long temp = (unsigned long)addre;
        return ((temp >> blockBit) << (64 - setBit)) >> (64 - setBit);
    }
    return 0;
}

/*for any address, get it's tag.*/
unsigned long getTag(long addre, int setBit, int blockBit) {
    unsigned long temp = (unsigned long)addre;
    return temp >> (setBit + blockBit);
}

/*for a given address, check if it's in the cache.
If so, returns the pointer to the line;
otherwise, return NULL.*/
line *isInCache(set **cache, unsigned long setIndex, unsigned long tag,
                int lineNum) {
    line *current = cache[setIndex]->head;
    for (int i = 0; i < lineNum; i++) {
        if ((current->tag == tag) && (current->valid)) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

/*this function is used when the targetLine is just used.
move the line to the tail of its associated set.*/
void moveBack(set **cache, int lineNum, unsigned long setIndex,
              line *targetLine) {
    if (lineNum == 1) {
        return;
    } else if (targetLine == cache[setIndex]->tail) {
        return;
    } else if (targetLine == cache[setIndex]->head) {
        cache[setIndex]->head = targetLine->next;
        cache[setIndex]->head->prev = NULL;
    } else {
        targetLine->prev->next = targetLine->next;
        targetLine->next->prev = targetLine->prev;
    }
    cache[setIndex]->tail->next = targetLine;
    targetLine->prev = cache[setIndex]->tail;
    targetLine->next = NULL;
    cache[setIndex]->tail = targetLine;
}

/*update the 5 parameters when a cache hit happens.
Only consider the case when the operation is Store,
since a load opeartion don't need to update,
except for hits, and hits is updated beforehand.*/
void hitAction(set **cache, int lineNum, unsigned long setIndex,
               unsigned int blockSize, line *targetLine, char O) {
    if (O == 'S') {
        if (!(targetLine->dirty)) {
            dirty_bytes += blockSize;
        }
        targetLine->dirty = true;
    }
    moveBack(cache, lineNum, setIndex, targetLine);
}

/*update the 5 parameters when a cache miss happens.
Note misses is updated beforehand*/
void missAction(set **cache, int lineNum, unsigned long setIndex,
                unsigned long tag, unsigned int blockSize, char O) {
    line *targetLine = cache[setIndex]->head;
    if (targetLine->valid) {
        evictions += 1;
    }
    switch (O) {
    case 'L':
        if (targetLine->valid && targetLine->dirty) {
            dirty_bytes -= blockSize;
            dirty_evictions += blockSize;
        }
        targetLine->dirty = false;
        break;
    case 'S':
        if (targetLine->valid) {
            if (targetLine->dirty) {
                dirty_evictions += blockSize;
            } else {
                dirty_bytes += blockSize;
            }
        } else {
            dirty_bytes += blockSize;
        }
        targetLine->dirty = true;
        break;
    }
    targetLine->valid = true;
    targetLine->tag = tag;
    moveBack(cache, lineNum, setIndex, targetLine);
}

/*the following 2 functions free the cache*/
void freeSet(set *S) {
    line *current = S->head;
    line *temp;
    while (current != S->tail) {
        temp = current->next;
        free(current);
        current = temp;
    }
    free(S->tail);
    free(S);
}

void freeCache(set **cache, unsigned int setNum) {
    for (unsigned int i = 0; i < setNum; i++) {
        freeSet(cache[i]);
    }
    free(cache);
}

int main(int argc, char *argv[]) {
    int setBit = 0, blockBit = 0, lineNum = 0, temp;
    char *traceName;
    FILE *fp;
    int ch;
    char h[100], dest[100];
    char operation;
    long addre;

    /*parse the input arguments.*/
    while ((ch = getopt(argc, argv, "s:E:b:t:")) != -1) {
        switch (ch) {
        case 's':
            setBit = atoi(optarg);
            break;
        case 'E':
            lineNum = atoi(optarg);
            break;
        case 'b':
            blockBit = atoi(optarg);
            break;
        case 't':
            traceName = optarg;
            break;
        }
    }
    /*finish parsing the input arguments.*/

    /*calculate the #of sets and size of the block*/
    unsigned int setNum = twoPow(setBit);
    unsigned int blockSize = twoPow(blockBit);

    /*setting up the cache*/
    set **cache = (set **)calloc(sizeof(set *), setNum);
    for (unsigned int i = 0; i < setNum; i++) {
        buildSet(cache, lineNum, i);
    }
    /*finish setting up the cache*/

    fp = fopen(traceName, "r");

    /*start stimulating the cache behaviour.*/
    while (fgets(h, 200, fp) != NULL) {

        /*for each line in the trace file,
        get its operation (L, S), and the address.
        don't consider the number of bytes loaded/stored
        since it's unnecessary.*/
        operation = h[0];
        temp = 0;
        while (h[temp] != ',') {
            temp++;
        }
        strncpy(dest, h + 2, temp - 2);
        dest[temp - 2] = '\0';
        addre = strtol(dest, NULL, 16);
        /************************************************/

        /*use the address to get set index and tag*/
        unsigned long setIndex = getSetIdx(addre, setBit, blockBit);
        unsigned long tag = getTag(addre, setBit, blockBit);
        /************************************************/

        line *targetLine = isInCache(cache, setIndex, tag, lineNum);

        if (targetLine != NULL) {
            hits += 1;
            hitAction(cache, lineNum, setIndex, blockSize, targetLine,
                      operation);
        } else {
            misses += 1;
            missAction(cache, lineNum, setIndex, tag, blockSize, operation);
        }
    }
    /*finish stimulating the cache behaviour.*/

    fclose(fp);

    freeCache(cache, setNum);

    const csim_stats_t stat = {hits, misses, evictions, dirty_bytes,
                               dirty_evictions};
    printSummary(&stat);

    return 0;
}
