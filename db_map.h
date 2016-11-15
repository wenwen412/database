#pragma once

#include "db_object.h"
#include "db_malloc.h"

/**
 *	map allocations
 */

uint64_t allocMap(DbMap *map, uint32_t size);
uint64_t allocBlk(DbMap *map, uint32_t size, bool zeroit);
uint64_t allocObj(DbMap *map, DbAddr *free, DbAddr *tail, int type, uint32_t size, bool zeroit);
uint64_t allocObjId(DbMap *map, DbAddr *free, DbAddr *tail, uint16_t idx);

void *fetchIdSlot (DbMap *map, ObjId objId);
void *getObj(DbMap *map, DbAddr addr); 
void freeBlk(DbMap *map, DbAddr addr);
void freeId(DbMap *map, ObjId objId);

uint64_t getFreeFrame(DbMap *map);
uint64_t allocFrame(DbMap *map);

/**
 * spin latches
 */

void lockAddr(volatile uint64_t* bits);
void unlockAddr(volatile uint64_t* bits);
void lockLatch(volatile char* latch);
void unlockLatch(volatile char* latch);
void waitNonZero(volatile char *zero);
void waitNonZero32(volatile int32_t *zero);
void waitNonZero64(volatile int64_t *zero);
void waitZero(volatile char *zero);
void waitZero32(volatile int32_t *zero);
void waitZero64(volatile int64_t *zero);
void art_yield();

/**
 * atomic integer ops
 */

void kill_slot(volatile char* latch);

int64_t atomicAdd64(volatile int64_t *value, int64_t amt);
int32_t atomicAdd32(volatile int32_t *value, int32_t amt);
int64_t atomicOr64(volatile int64_t *value, int64_t amt);
int32_t atomicOr32(volatile int32_t *value, int32_t amt);
uint64_t atomicExchange(uint64_t *target, uint64_t value);
uint64_t compareAndSwap(uint64_t* target, uint64_t compare_val, uint64_t swap_val);
int8_t atomicOr8(volatile char *value, char amt);

void closeMap(DbMap *map);
void deleteMap(char *path);
void lockArena (DbMap *map);
void unlockArena (DbMap *map);
bool fileExists(char *path);
void yield();
