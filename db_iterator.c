#include "db.h"
#include "db_object.h"
#include "db_handle.h"
#include "db_arena.h"
#include "db_map.h"
#include "db_api.h"
#include "db_iterator.h"

//
// increment a segmented ObjId
//

bool incrObjId(Iterator *it, DbMap *map) {
ObjId start = it->docId;

	while (it->docId.seg <= map->arena->objSeg) {
		if (++it->docId.idx <= map->arena->segs[it->docId.seg].nextId.idx)
			return true;

		it->docId.idx = 0;
		it->docId.seg++;
	}

	it->docId = start;
	return false;
}

//
// decrement a segmented recordId
//

bool decrObjId(Iterator *it, DbMap *map) {
ObjId start = it->docId;

	while (true) {
		if (it->docId.idx) {
			if (--it->docId.idx)
				return true;
		}

		if (it->docId.seg) {
			it->docId.seg--;
			it->docId.idx = map->arena->segs[it->docId.seg].nextId.idx + 1;
			continue;
		}

		it->docId = start;
		return false;
	}
}

//
//  advance iterator forward
//

DbAddr *iteratorNext(Handle *itHndl) {
Iterator *it;

	it = (Iterator *)(itHndl + 1);

	while (incrObjId(it, itHndl->map)) {
	  DbAddr *slot = fetchIdSlot(itHndl->map, it->docId);
	  if (slot->bits) {
		it->state = IterPosAt;
		return slot;
	  }
	}

	it->state = IterRightEof;
	return NULL;
}

//
//  advance iterator backward
//

DbAddr *iteratorPrev(Handle *itHndl) {
Iterator *it;

	it = (Iterator *)(itHndl + 1);

	while (decrObjId(it, itHndl->map)) {
	  DbAddr *slot = fetchIdSlot(itHndl->map, it->docId);
	  if (slot->bits) {
		it->state = IterPosAt;
		return slot;
	  }
	}

	it->state = IterLeftEof;
	return NULL;
}

//
//  set iterator to specific objectId
//

DbAddr *iteratorSeek(Handle *itHndl, IteratorOp op, ObjId docId) {
Iterator *it;

	it = (Iterator *)(itHndl + 1);

	switch (op) {
	  case IterNext:
		return iteratorNext(itHndl);

	  case IterPrev:
		return iteratorPrev(itHndl);

	  case IterBegin:
		it->docId.bits = 0;
		it->state = IterLeftEof;
		break;

	  case IterEnd:
		it->docId.bits = itHndl->map->arena->segs[itHndl->map->arena->currSeg].nextId.bits;
		it->state = IterRightEof;
		break;

	  case IterSeek: {
		DbAddr *slot = fetchIdSlot(itHndl->map, docId);

		it->docId.bits = docId.bits;

		if (slot->bits) {
			it->state = IterPosAt;
			return slot;
		} else
			it->state = IterNone;

		break;
	  }

	  default:
		break;
	}

	return NULL;
}
