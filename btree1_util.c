#include "db.h"
#include "db_object.h"
#include "db_arena.h"
#include "db_map.h"
#include "btree1.h"

//	debug slot function

#ifdef DEBUG
BtreeSlot *btreeSlot(BtreePage *page, uint32_t idx)
{
	return slotptr(page, idx);
}

uint8_t *btreeKey(BtreePage *page, uint32_t idx)
{
	return keyptr(page, idx);
}

uint8_t *btreeAddr(BtreePage *page, uint32_t off)
{
	return keyaddr(page, off);
}

#undef keyptr
#undef keyaddr
#undef slotptr
#define keyptr(p,x) btreeKey(p,x)
#define keyaddr(p,o) btreeAddr(p,o)
#define slotptr(p,x) btreeSlot(p,x)
#endif

uint32_t Splits;

// split the root and raise the height of the btree
// call with key for smaller half and right page addr.

Status btreeSplitRoot(Handle *hndl, BtreeSet *root, DbAddr right, uint8_t *leftKey) {
BtreeIndex *btree = btreeIndex(hndl->map);
uint32_t keyLen, nxt = btree->pageSize;
BtreePage *leftPage, *rightPage;
BtreeSlot *slot;
uint64_t result;
uint8_t *ptr;
Status stat;
DbAddr left;

	//  Obtain an empty page to use, and copy the current
	//  root contents into it, e.g. lower keys

	if( (left.bits = btreeNewPage(hndl, root->page->lvl)) )
		leftPage = getObj(hndl->map, left);
	else
		return ERROR_outofmemory;

	//	copy in new smaller keys into left page
	//	(clear the latches)

	memcpy (leftPage->latch + 1, root->page->latch + 1, btree->pageSize - sizeof(*leftPage->latch));
	rightPage = getObj(hndl->map, right);
	rightPage->left.bits = left.bits;

	// preserve the page info at the bottom
	// of higher keys and set rest to zero

	memset(root->page+1, 0, btree->pageSize - sizeof(*root->page));

	// insert stopper key on root page
	// pointing to right half page 
	// and increase the root height

	nxt -= 1 + sizeof(uint64_t);
	slot = slotptr(root->page, 2);
	slot->type = Btree_stopper;
	slot->off = nxt;

	ptr = keyaddr(root->page, nxt);
	ptr[0] = store64(ptr + 1, 0, right.bits);

	// insert lower keys (left) fence key on newroot page as first key
	// reserve space for maximum sized key.

	keyLen = get64(leftKey + keypre(leftKey), keylen(leftKey), &result);
	nxt -= keyLen + sizeof(uint64_t) + 2;

	slot = slotptr(root->page, 1);
	slot->type = Btree_indexed;
	slot->off = nxt;

	//	construct lower (left) page key

	ptr = keyaddr(root->page, nxt);
	memcpy (ptr + 2, leftKey + keypre(leftKey), keyLen);
	keyLen = store64(ptr + 2, keyLen, left.bits);
	ptr[0] = keyLen / 256 | 0x80, ptr[1] = keyLen;
	
	root->page->right.bits = 0;
	root->page->min = nxt;
	root->page->cnt = 2;
	root->page->act = 2;
	root->page->lvl++;

	// release root page

	btreeUnlockPage(root->page, Btree_lockWrite);
	return OK;
}

//  split already locked full node
//	return unlocked.

Status btreeSplitPage (Handle *hndl, BtreeSet *set) {
uint8_t leftKey[Btree_maxkey], rightKey[Btree_maxkey];
BtreeIndex *btree = btreeIndex(hndl->map);
uint32_t cnt = 0, idx = 0, max, nxt, off;
BtreeSlot librarian, *source, *dest;
BtreePageType type = Btree_leafPage;
uint32_t size = btree->pageSize;
BtreePage *frame, *rightPage;
uint8_t lvl = set->page->lvl;
uint32_t totLen, keyLen;
DbAddr right, addr;
uint8_t *key;
bool stopper;
Status stat;

#ifdef DEBUG
	atomicAdd32(&Splits, 1);
#endif

	librarian.bits = 0;
	librarian.type = Btree_librarian;
	librarian.dead = 1;

	if( !set->page->lvl )
		size <<= btree->leafXtra;
	else
		type = Btree_interior;

	//	get new page and write higher keys to it.

	if( (right.bits = btreeNewPage(hndl, lvl)) )
		rightPage = getObj(hndl->map, right);
	else
		return ERROR_outofmemory;

	max = set->page->cnt;
	cnt = max / 2;
	nxt = size;
	idx = 0;

	source = slotptr(set->page, cnt);
	dest = slotptr(rightPage, 0);

	while( source++, cnt++ < max ) {
		if( source->dead )
			continue;

		key = keyaddr(set->page, source->off);
		totLen = keylen(key) + keypre(key);
		nxt -= totLen;

		memcpy (keyaddr(rightPage, nxt), key, totLen);
		rightPage->act++;

		//	add librarian slot

		if (cnt < max) {
			(++dest)->bits = librarian.bits;
			dest->off = nxt;
			idx++;
		}

		//  add actual slot

		(++dest)->bits = source->bits;
		dest->off = nxt;
		idx++;
	}

	//	remember right fence key for larger page
	//	extend right leaf fence key with
	//	the right page number on leaf page.

	stopper = dest->type == Btree_stopper;

	if( set->page->lvl)
		keyLen = keylen(key) - 2 - (key[totLen - 1] & 0x7);	// strip off pageNo
	else
		keyLen = keylen(key);	// length w/o pageNo

	if( keyLen + sizeof(uint64_t) < 128 )
		off = 1;
	else
		off = 2;

	//	copy key and add pageNo

	memcpy (rightKey + off, key + keypre(key), keyLen);
	keyLen = store64(rightKey + off, keyLen, right.bits);

	if (off == 1)
		rightKey[0] = keyLen;
	else
		rightKey[0] = keyLen / 256 | 0x80, rightKey[1] = keyLen;

	rightPage->min = nxt;
	rightPage->cnt = idx;
	rightPage->lvl = lvl;

	// link right node

	if( set->pageNo.type != Btree_rootPage ) {
		rightPage->right.bits = set->page->right.bits;
		rightPage->left.bits = set->pageNo.bits;

		if( !lvl && rightPage->right.bits ) {
			BtreePage *farRight = getObj(hndl->map, rightPage->right);
			btreeLockPage (farRight, Btree_lockLink);
			farRight->left.bits = right.bits;
			btreeUnlockPage (farRight, Btree_lockLink);
		}
	}

	//	copy lower keys from temporary frame back into old page

	if( (addr.bits = btreeNewPage(hndl, lvl)) )
		frame = getObj(hndl->map, addr);
	else
		return ERROR_outofmemory;

	memcpy (frame, set->page, size);
	memset (set->page+1, 0, size - sizeof(*set->page));

	set->page->garbage = 0;
	set->page->act = 0;
	nxt = size;
	max /= 2;
	cnt = 0;
	idx = 0;

	//  ignore librarian max key

	if( slotptr(frame, max)->type == Btree_librarian )
		max--;

	source = slotptr(frame, 0);
	dest = slotptr(set->page, 0);

	//  assemble page of smaller keys from temporary frame copy

	while( source++, cnt++ < max ) {
		if( source->dead )
			continue;

		key = keyaddr(frame, source->off);
		totLen = keylen(key) + keypre(key);
		nxt -= totLen;

		memcpy (keyaddr(set->page, nxt), key, totLen);

		//	add librarian slot, except before fence key

		if (cnt < max) {
			(++dest)->bits = librarian.bits;
			dest->off = nxt;
			idx++;
		}

		//	add actual slot

		(++dest)->bits = source->bits;
		dest->off = nxt;
		idx++;

		set->page->act++;
	}

	set->page->right.bits = right.bits;
	set->page->min = nxt;
	set->page->cnt = idx;

	//	remember left fence key for smaller page
	//	extend left leaf fence key with
	//	the left page number.

	if( set->page->lvl)
		keyLen = keylen(key) - 2 - (key[totLen - 1] & 0x7);	// strip off pageNo
	else
		keyLen = keylen(key);	// length w/o pageNo

	if( keyLen + sizeof(uint64_t) < 128 )
		off = 1;
	else
		off = 2;

	//	copy key and add pageNo

	memcpy (leftKey + off, key + keypre(key), keyLen);
	keyLen = store64(leftKey + off, keyLen, set->pageNo.bits);

	if (off == 1)
		leftKey[0] = keyLen;
	else
		leftKey[0] = keyLen / 256 | 0x80, leftKey[1] = keyLen;

	//  return temporary frame

	freeNode(hndl->map, hndl->list, addr);

	// if current page is the root page, split it

	if( set->pageNo.type == Btree_rootPage )
		return btreeSplitRoot (hndl, set, right, leftKey);

	// insert new fences in their parent pages

	btreeLockPage (rightPage, Btree_lockParent);
	btreeLockPage (set->page, Btree_lockParent);
	btreeUnlockPage (set->page, Btree_lockWrite);

	// insert new fence for reformulated left block of smaller keys

	if( (stat = btreeInsertKey(hndl, leftKey + keypre(leftKey), keylen(leftKey), lvl+1, Btree_indexed) ))
		return stat;

	// switch fence for right block of larger keys to new right page

	if( (stat = btreeFixKey(hndl, rightKey, lvl+1, stopper) ))
		return stat;

	btreeUnlockPage (set->page, Btree_lockParent);
	btreeUnlockPage (rightPage, Btree_lockParent);
	return OK;
}

//	check page for space available,
//	clean if necessary and return
//	false - page needs splitting
//	true  - ok to insert

Status btreeCleanPage(Handle *hndl, BtreeSet *set, uint32_t totKeyLen) {
BtreeIndex *btree = btreeIndex(hndl->map);
BtreeSlot librarian, *source, *dest;
uint32_t size = btree->pageSize;
BtreePage *page = set->page;
uint32_t max = page->cnt;
uint32_t len, cnt, idx;
uint32_t newslot = max;
BtreePageType type;
BtreePage *frame;
uint8_t *key;
DbAddr addr;

	librarian.bits = 0;
	librarian.type = Btree_librarian;
	librarian.dead = 1;

	if( !page->lvl ) {
		size <<= btree->leafXtra;
		type = Btree_leafPage;
	} else {
		type = Btree_interior;
	}

	if( page->min >= (max+1) * sizeof(BtreeSlot) + sizeof(*page) + totKeyLen )
		return OK;

	//	skip cleanup and proceed directly to split
	//	if there's not enough garbage
	//	to bother with.

	if( page->garbage < size / 5 )
		return BTREE_needssplit;

	if( (addr.bits = allocNode(hndl->map, hndl->list, type, size, false)) )
		frame = getObj(hndl->map, addr);
	else
		return ERROR_outofmemory;

	memcpy (frame, page, size);

	// skip page info and set rest of page to zero

	memset (page+1, 0, size - sizeof(*page));
	page->garbage = 0;
	page->act = 0;

	cnt = 0;
	idx = 0;

	source = slotptr(frame, cnt);
	dest = slotptr(page, idx);

	// clean up page first by
	// removing deleted keys

	while( source++, cnt++ < max ) {
		if( cnt == set->slotIdx )
			newslot = idx + 2;

		if( source->dead )
			continue;

		// copy the active key across

		key = keyaddr(frame, source->off);
		len = keylen(key) + keypre(key);
		size -= len;

		memcpy ((uint8_t *)page + size, key, len);

		// make a librarian slot

		if (cnt < max) {
			(++dest)->bits = librarian.bits;
			++idx;
		}

		// set up the slot

		(++dest)->bits = source->bits;
		dest->off = size;
		idx++;

		page->act++;
	}

	page->min = size;
	page->cnt = idx;

	//  return temporary frame

	freeNode(hndl->map, hndl->list, addr);

	//	see if page has enough space now, or does it still need splitting?

	if( page->min >= (idx+1) * sizeof(BtreeSlot) + sizeof(*page) + totKeyLen )
		return OK;

	return BTREE_needssplit;
}

//  compare two keys, return > 0, = 0, or < 0
//  =0: all key fields are same
//  -1: key2 > key1
//  +1: key2 < key1

int btreeKeyCmp (uint8_t *key1, uint8_t *key2, uint32_t len2)
{
uint32_t len1 = keylen(key1);
int ans;

	key1 += keypre(key1);

	if( ans = memcmp (key1, key2, len1 > len2 ? len2 : len1) )
		return ans;

	if( len1 > len2 )
		return 1;
	if( len1 < len2 )
		return -1;

	return 0;
}

//  find slot in page for given key at a given level

uint32_t btreeFindSlot (BtreePage *page, uint8_t *key, uint32_t keyLen, bool stopper)
{
uint32_t diff, higher = page->cnt, low = 1, slot;
uint32_t good = 0;

	assert(higher > 0);

	//	are we being asked for the stopper(fence) key?

	if (stopper)
		return higher;

	//	  make stopper key an infinite fence value

	if( page->right.bits )
		higher++;
	else
		good++;

	//	low is the lowest candidate.
	//  loop ends when they meet

	//  higher is already
	//	tested as .ge. the passed key.

	while( (diff = higher - low) ) {
		slot = low + diff / 2;
		if( btreeKeyCmp (keyptr(page, slot), key, keyLen) < 0 )
			low = slot + 1;
		else
			higher = slot, good++;
	}

	//	return zero if key is on next right page

	return good ? higher : 0;
}

//  find and load page at given level for given key
//	leave page rd or wr locked as requested

Status btreeLoadPage(Handle *hndl, BtreeSet *set, uint8_t *key, uint32_t keyLen, uint8_t lvl, BtreeLock lock, bool stopper) {
BtreeIndex *btree = btreeIndex(hndl->map);
uint8_t drill = 0xff, *ptr;
BtreePage *prevPage = NULL;
BtreeLock mode, prevMode;
DbAddr prevPageNo;

  set->pageNo.bits = btree->root.bits;
  prevPageNo.bits = 0;

  //  start at our idea of the root level of the btree and drill down

  do {
	// determine lock mode of drill level

	mode = (drill == lvl) ? lock : Btree_lockRead; 
	set->page = getObj(hndl->map, set->pageNo);

	//	release parent or left sibling page

	if( prevPageNo.bits ) {
	  btreeUnlockPage(prevPage, prevMode);
	  prevPageNo.bits = 0;
	}

 	// obtain mode lock

	btreeLockPage(set->page, mode);

	if( set->page->free )
		return ERROR_btreestruct;

	// re-read and re-lock root after determining actual level of root

	if( set->page->lvl != drill) {
		assert(drill == 0xff);
		drill = set->page->lvl;

		if( lock != Btree_lockRead && drill == lvl ) {
		  btreeUnlockPage(set->page, mode);
		  continue;
		}
	}

	assert(lvl <= set->page->lvl);

	prevPageNo.bits = set->pageNo.bits;
	prevPage = set->page;
	prevMode = mode;

	//  find key on page at this level
	//  and descend to requested level

	if( !set->page->kill )
	 if( (set->slotIdx = btreeFindSlot (set->page, key, keyLen, stopper)) ) {
	  if( drill == lvl )
		return OK;

	  // find next non-dead slot -- the fence key if nothing else

	  while( slotptr(set->page, set->slotIdx)->dead )
		if( set->slotIdx++ < set->page->cnt )
		  continue;
		else
  		  return ERROR_btreestruct;

	  // get next page down

	  assert(drill > 0);
	  drill--;
	  ptr = keyptr(set->page, set->slotIdx);
	  get64(ptr + keypre(ptr), keylen(ptr), &set->pageNo.bits);
	  continue;
	 }

	//  or slide right into next page

	set->pageNo.bits = set->page->right.bits;
  } while( set->pageNo.bits );

  // return error on end of right chain

  return ERROR_btreestruct;
}
