#include "db.h"
#include "db_map.h"
#include "db_object.h"
#include "db_arena.h"
#include "db_handle.h"

extern DbAddr openMaps[1];
extern DbMap memMap[1];
extern DbMap *hndlMap;

// drop an arena and recursively its children

void dropArenaDef(DbMap *db, ArenaDef *arenaDef, ArenaDef *parentArena, bool dropDefs, char *path, uint32_t pathLen) {
uint64_t *skipPayLoad;
uint32_t childIdMax;
uint32_t idx, len;
DbAddr *next, addr;
RedBlack *entry;
SkipNode *node;
DbMap **map;
uint64_t id;

	childIdMax = arenaDef->childId;
	id = arenaDef->id;

	//	databases don't have parents

	if (parentArena) {
	  writeLock(parentArena->idList->lock);
	  addr.bits = skipDel(db, parentArena->idList->head, id); 
	  entry = getObj(db, addr);

	  //  delete our id from parent's child name & id list

	  if (dropDefs) {
		rbDel(db, parentArena->nameTree, entry); 
	  } else {
		arenaDef->id = atomicAdd64(&parentArena->childId, 1);
		skipPayLoad = skipAdd (db, parentArena->idList->head, arenaDef->id);
		*skipPayLoad = entry->addr.bits;
	  }

	  writeUnlock(parentArena->idList->lock);
	}

	//	drop the children

	do {
	  writeLock(arenaDef->idList->lock);
	  next = arenaDef->idList->head;

	  if (next->addr)
		node = getObj(db, *next);
	  else
		break;

	  if (*node->array->key <= childIdMax)
	  	addr.bits = *node->array->val;
	  else
		break;

	  entry = getObj(db, addr);
	  writeUnlock(arenaDef->idList->lock);

	  //  add child name to the path
	  //	and drop it.

	  len = addPath(path, pathLen, rbkey(entry), entry->keyLen, id);

	  dropArenaDef(db, (ArenaDef *)(entry + 1), arenaDef, dropDefs, path, pathLen + len);
	} while (true);

	writeUnlock(arenaDef->idList->lock);

	//  wait for handles to exit

	lockLatch(arenaDef->hndlCalls->latch);
	disableHndls(db, arenaDef->hndlCalls);
	unlockLatch(arenaDef->hndlCalls->latch);

	//	close map if we are open in this process
	//	otherwise try to delete it

	map = arrayElement(memMap, openMaps, arenaDef->mapIdx, sizeof(void *));
	path[pathLen] = 0;

	if (!*map)
		deleteMap(path);
	else if (!(*map)->openCnt[0])
		closeMap(*map), *map = NULL;
}

DbStatus dropMap(DbMap *map, bool dropDefs) {
ArenaDef *parentArena;
char path[MAX_path];
PathStk pathStk[1];
DbStatus stat;

	//	are we already dropped?

	if (dropDefs)
	  if (atomicOr8(map->arenaDef->dead, KILL_BIT) & KILL_BIT)
		return DB_ERROR_arenadropped;

	memcpy (path, map->arenaPath, map->pathLen);

	// when dropping database, purge from parent's child list

	if (dropDefs)
	  if (map->arena->type[0] == Hndl_database) {
		RedBlack *entry = getObj(hndlMap, *map->arena->redblack);
		Catalog *catalog = (Catalog *)(hndlMap + 1);
		lockLatch(catalog->dbList->latch);

		// advance database version number

		if ((entry = rbFind(hndlMap, catalog->dbList, rbkey(entry), entry->keyLen, pathStk))) {
			uint64_t *dbVer = (uint64_t *)(entry + 1);
			*dbVer += 1;
		}

		unlockLatch(catalog->dbList->latch);

		// no need to clean up database allocations

		dropDefs = false;
	  }

	if (map->arenaDef->parentAddr.addr)
	  parentArena = getObj(map->db, map->arenaDef->parentAddr);
	else
	  parentArena = NULL;

	dropArenaDef(map->db, map->arenaDef, parentArena, dropDefs, path, map->pathLen);
	return DB_OK;
}
