
/*
 * $Id: store_client.cc,v 1.120 2003/01/23 00:37:26 robertc Exp $
 *
 * DEBUG: section 20    Storage Manager Client-Side Interface
 * AUTHOR: Duane Wessels
 *
 * SQUID Web Proxy Cache          http://www.squid-cache.org/
 * ----------------------------------------------------------
 *
 *  Squid is the result of efforts by numerous individuals from
 *  the Internet community; see the CONTRIBUTORS file for full
 *  details.   Many organizations have provided support for Squid's
 *  development; see the SPONSORS file for full details.  Squid is
 *  Copyrighted (C) 2001 by the Regents of the University of
 *  California; see the COPYRIGHT file for full details.  Squid
 *  incorporates software developed and/or copyrighted by other
 *  sources; see the CREDITS file for full details.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111, USA.
 *
 */

#include "squid.h"
#include "StoreClient.h"
#include "Store.h"
#include "HttpReply.h"
#include "MemObject.h"
#include "StoreMeta.h"
#include "StoreMetaUnpacker.h"

CBDATA_TYPE(store_client);

/*
 * NOTE: 'Header' refers to the swapfile metadata header.
 * 	 'OBJHeader' refers to the object header, with cannonical
 *	 processed object headers (which may derive from FTP/HTTP etc
 *	 upstream protocols
 *       'Body' refers to the swapfile body, which is the full
 *        HTTP reply (including HTTP headers and body).
 */
static STRCB storeClientReadBody;
static STRCB storeClientReadHeader;
static void storeClientCopy2(StoreEntry * e, store_client * sc);
static EVH storeClientCopyEvent;
static int CheckQuickAbort2(StoreEntry * entry);
static void CheckQuickAbort(StoreEntry * entry);

MemPool *store_client::pool = NULL;

void *
store_client::operator new (size_t byteCount)
{
    /* derived classes with different sizes must implement their own new */
    assert (byteCount == sizeof (store_client));
    CBDATA_INIT_TYPE(store_client);
    return cbdataAlloc(store_client);
}

void
store_client::operator delete (void *address)
{
    cbdataFree ((store_client *)address);
}

bool
store_client::memReaderHasLowerOffset(off_t anOffset) const
{
    return getType() == STORE_MEM_CLIENT && copyInto.offset < anOffset;
}

int
store_client::getType() const
{
    return type;
}

#if STORE_CLIENT_LIST_DEBUG
static store_client *
storeClientListSearch(const MemObject * mem, void *data)
{
    dlink_node *node;
    store_client *sc = NULL;
    for (node = mem->clients.head; node; node = node->next) {
	sc = node->data;
	if (sc->owner == data)
	    return sc;
    }
    return NULL;
}

int
storeClientIsThisAClient(store_client * sc, void *someClient)
{
    return sc->owner == someClient;
}
#endif

/* add client with fd to client list */
store_client *
storeClientListAdd(StoreEntry * e, void *data)
{
    MemObject *mem = e->mem_obj;
    store_client *sc;
    assert(mem);
#if STORE_CLIENT_LIST_DEBUG
    if (storeClientListSearch(mem, data) != NULL)
	/* XXX die! */
	assert(1 == 0);
#endif
    sc = new store_client (e);
    mem->addClient(sc);
    return sc;
}

void
store_client::callback(ssize_t sz, bool error)
{
    StoreIOBuffer result (sz, 0 ,copyInto.data);
    if (sz < 0) {
	result.flags.error = 1;
	result.length = 0;
    } else {
	result.flags.error = error ? 1 : 0;
    }
    result.offset = cmp_offset;
    assert(callbackPending());
    cmp_offset = copyInto.offset + sz;
    STCB *temphandler = _callback.callback_handler;
    void *cbdata = _callback.callback_data;
    _callback = Callback(NULL, NULL);
    copyInto.data = NULL;
    if (cbdataReferenceValid(cbdata))
	temphandler(cbdata, result);
    cbdataReferenceDone(cbdata);
}

static void
storeClientCopyEvent(void *data)
{
    store_client *sc = (store_client *)data;
    debug(20, 3)("storeClientCopyEvent: Running\n");
    assert (sc->flags.copy_event_pending);
    sc->flags.copy_event_pending = 0;
    if (!sc->callbackPending())
	return;
    storeClientCopy2(sc->entry, sc);
}

store_client::store_client(StoreEntry *e) : entry (e), type (e->storeClientType()), object_ok(true)
{
    cmp_offset = 0;
    flags.disk_io_pending = 0;
    entry->refcount++;
    if (getType() == STORE_DISK_CLIENT)
	/* assert we'll be able to get the data we want */
	/* maybe we should open swapin_fd here */
	assert(entry->swap_filen > -1 || storeSwapOutAble(entry));
#if DELAY_POOLS
    delayId = 0;
#endif
#if STORE_CLIENT_LIST_DEBUG
    owner = cbdataReference(data);
#endif
}

/* copy bytes requested by the client */
void
storeClientCopy(store_client * sc,
    StoreEntry * e,
    StoreIOBuffer copyInto,
    STCB * callback,
    void *data)
{
    assert (sc != NULL);
    sc->copy(e, copyInto,callback,data);
}

void
store_client::copy(StoreEntry * anEntry,
		       StoreIOBuffer copyRequest,
		           STCB * callback_fn,
			       void *data)
{
    assert (anEntry == entry);
    assert (callback_fn);
    assert (data);
    assert(!EBIT_TEST(entry->flags, ENTRY_ABORTED));
    debug(20, 3)("store_client::copy: %s, from %lu, for length %d, cb %p, cbdata %p",
	entry->getMD5Text(),
	(unsigned long) copyRequest.offset,
	(int) copyRequest.length,
	callback_fn,
	data);
#if STORE_CLIENT_LIST_DEBUG
    assert(this == storeClientListSearch(entry->mem_obj, data));
#endif
    assert(!callbackPending());
#if ONLYCONTIGUOUSREQUESTS
    assert(cmp_offset == copyRequest.offset);
#endif
    /* range requests will skip into the body */
    cmp_offset = copyRequest.offset;
    _callback = Callback (callback_fn, cbdataReference(data));
    copyInto.data = copyRequest.data;
    copyInto.length = copyRequest.length;
    copyInto.offset = copyRequest.offset;

    storeClientCopy2(entry, this);
}

/*
 * This function is used below to decide if we have any more data to
 * send to the client.  If the store_status is STORE_PENDING, then we
 * do have more data to send.  If its STORE_OK, then
 * we continue checking.  If the object length is negative, then we
 * don't know the real length and must open the swap file to find out.
 * If the length is >= 0, then we compare it to the requested copy
 * offset.
 */
static int
storeClientNoMoreToSend(StoreEntry * e, store_client * sc)
{
    ssize_t len;
    if (e->store_status == STORE_PENDING)
	return 0;
    if ((len = objectLen(e)) < 0)
	return 0;
    if (sc->copyInto.offset < len)
	return 0;
    return 1;
}

static void
storeClientCopy2(StoreEntry * e, store_client * sc)
{
    /* reentrancy not allowed  - note this could lead to
     * dropped events
     */
    if (sc->flags.copy_event_pending) {
	return;
    }
    if (EBIT_TEST(e->flags, ENTRY_FWD_HDR_WAIT)) {
	debug(20, 5)("storeClientCopy2: returning because ENTRY_FWD_HDR_WAIT set\n");
	return;
    }
    if (sc->flags.store_copying) {
	sc->flags.copy_event_pending = 1;
	debug(20, 3)("storeClientCopy2: Queueing storeClientCopyEvent()\n");
	eventAdd("storeClientCopyEvent", storeClientCopyEvent, sc, 0.0, 0);
	return;
    }
    debug(20, 3)("storeClientCopy2: %s\n", e->getMD5Text());
    assert(sc->callbackPending());
    /*
     * We used to check for ENTRY_ABORTED here.  But there were some
     * problems.  For example, we might have a slow client (or two) and
     * the server-side is reading far ahead and swapping to disk.  Even
     * if the server-side aborts, we want to give the client(s)
     * everything we got before the abort condition occurred.
     */
    /* Warning: doCopy may indirectly free itself in callbacks,
     * hence the cbdata reference to keep it active for the duration of
     * this function
     */
    cbdataReference(sc);
    assert (sc->flags.store_copying == 0);
    sc->doCopy(e);
    assert (sc->flags.store_copying == 0);
    cbdataReferenceDone(sc);
}

void
store_client::doCopy(StoreEntry *anEntry)
{
    assert (anEntry == entry);
    flags.store_copying = 1;
    MemObject *mem = entry->mem_obj;

    debug(33, 5)("store_client::doCopy: co: %lu, hi: %ld\n", (unsigned long) copyInto.offset, (long int) mem->endOffset());

    if (storeClientNoMoreToSend(entry, this)) {
	/* There is no more to send! */
	callback(0);
	flags.store_copying = 0;
	return;
    }
    /* Check that we actually have data */
    if (anEntry->store_status == STORE_PENDING && copyInto.offset >= mem->endOffset()) {
	debug(20, 3)("store_client::doCopy: Waiting for more\n");
	flags.store_copying = 0;
	return;
    }
    /*
     * Slight weirdness here.  We open a swapin file for any
     * STORE_DISK_CLIENT, even if we can copy the requested chunk
     * from memory in the next block.  We must try to open the
     * swapin file before sending any data to the client side.  If
     * we postpone the open, and then can not open the file later
     * on, the client loses big time.  Its transfer just gets cut
     * off.  Better to open it early (while the client side handler
     * is clientCacheHit) so that we can fall back to a cache miss
     * if needed.
     */

    if (STORE_DISK_CLIENT == getType() && NULL == swapin_sio.getRaw()) {
	debug(20, 3)("store_client::doCopy: Need to open swap in file\n");
	/* gotta open the swapin file */
	if (storeTooManyDiskFilesOpen()) {
	    /* yuck -- this causes a TCP_SWAPFAIL_MISS on the client side */
	    fail();
	    flags.store_copying = 0;
	    return;
	} else if (!flags.disk_io_pending) {
	    /* Don't set store_io_pending here */
	    storeSwapInStart(this);
	    if (NULL == swapin_sio.getRaw()) {
		fail();
		flags.store_copying = 0;
		return;
	    }
	    /*
	     * If the open succeeds we either copy from memory, or
	     * schedule a disk read in the next block.
	     */
	} else {
	    debug (20, 1)("WARNING: Averted multiple fd operation (1)\n");
	    flags.store_copying = 0;
	    return;
	}
    }
    if (copyInto.offset >= mem->inmem_lo && copyInto.offset < mem->endOffset()) {
	/* What the client wants is in memory */
	/* Old style */
	debug(20, 3)("store_client::doCopy: Copying normal from memory\n");
	size_t sz = mem->data_hdr.copy(copyInto.offset, copyInto.data,
	    copyInto.length);
	callback(sz);
	flags.store_copying = 0;
	return;
    }
    /* What the client wants is not in memory. Schedule a disk read */
    assert(STORE_DISK_CLIENT == getType());
    assert(!flags.disk_io_pending);
    debug(20, 3)("store_client::doCopy: reading from STORE\n");
    fileRead();
    flags.store_copying = 0;
}

void
store_client::fileRead()
{
    MemObject *mem = entry->mem_obj;

    assert(callbackPending());
    assert(!flags.disk_io_pending);
    flags.disk_io_pending = 1;
    if (mem->swap_hdr_sz != 0)
	if (entry->swap_status == SWAPOUT_WRITING)
	    assert(mem->swapout.sio->offset() > copyInto.offset + (off_t)mem->swap_hdr_sz);
    storeRead(swapin_sio,
	      copyInto.data,
	      copyInto.length,
	      copyInto.offset + mem->swap_hdr_sz,
	      mem->swap_hdr_sz == 0 ? storeClientReadHeader
	      : storeClientReadBody,
	      this);
}

static void
storeClientReadBody(void *data, const char *buf, ssize_t len)
{
    store_client *sc = (store_client *)data;
    assert(sc->flags.disk_io_pending);
    sc->flags.disk_io_pending = 0;
    assert(sc->callbackPending());
    debug(20, 3)("storeClientReadBody: len %d", (int) len);
    if (sc->copyInto.offset == 0 && len > 0 && sc->entry->getReply()->sline.status == 0)
	/* Our structure ! */
	if (!httpReplyParse((HttpReply *)sc->entry->getReply(), sc->copyInto.data, headersEnd(sc->copyInto.data, len))) {
	    debug (20,0)("Could not parse headers from on disk object\n");
	}
    sc->callback(len);
}

void
store_client::fail() 
{
    object_ok = false;
    callback(0, true);
}

static void
storeClientReadHeader(void *data, const char *buf, ssize_t len)
{
    store_client *sc = (store_client *)data;
    sc->readHeader(buf, len);
}

void
store_client::unpackHeader(char const *buf, ssize_t len)
{
    debug(20, 3)("store_client::unpackHeader: len %d", (int) len);
    if (len < 0) {
	debug(20, 3)("store_client::unpackHeader: %s", xstrerror());
	fail();
	return;
    }
    int swap_hdr_sz = 0;
    StoreMetaUnpacker aBuilder(buf, len, &swap_hdr_sz);
    if (!aBuilder.isBufferSane()) {
	/* oops, bad disk file? */
	debug(20, 1) ("WARNING: swapfile header inconsistent with available data\n");
	fail();
	return;
    }
    tlv *tlv_list = aBuilder.createStoreMeta ();
    if (tlv_list == NULL) {
	debug(20, 1) ("WARNING: failed to unpack meta data\n");
	fail();
	return;
    }
    /*
     * Check the meta data and make sure we got the right object.
     */
    for (tlv *t = tlv_list; t; t = t->next) {
	if (!t->checkConsistency(entry)) {
	    storeSwapTLVFree(tlv_list);
	    fail();
	    return;
	}
    }
    storeSwapTLVFree(tlv_list);

    entry->mem_obj->swap_hdr_sz = swap_hdr_sz;
    entry->mem_obj->object_sz = entry->swap_file_sz - swap_hdr_sz;

}

void
store_client::readHeader(char const *buf, ssize_t len)
{
    MemObject *const mem = entry->mem_obj;
    
    assert(flags.disk_io_pending);
    flags.disk_io_pending = 0;
    assert(callbackPending());

    unpackHeader (buf, len);
    if (!object_ok)
	return;
    
    /*
     * If our last read got some data the client wants, then give
     * it to them, otherwise schedule another read.
     */
    size_t body_sz = len - mem->swap_hdr_sz;
    if (static_cast<size_t>(copyInto.offset) < body_sz) {
	/*
	 * we have (part of) what they want
	 */
	size_t copy_sz = XMIN(copyInto.length, body_sz);
	debug(20, 3) ("storeClientReadHeader: copying %d bytes of body\n",
	    (int) copy_sz);
	xmemmove(copyInto.data, copyInto.data + mem->swap_hdr_sz, copy_sz);
       if (copyInto.offset == 0 && len > 0 && entry->getReply()->sline.status == 0)
           /* Our structure ! */
           if (!httpReplyParse((HttpReply *)entry->getReply(), copyInto.data,
               headersEnd(copyInto.data, copy_sz))) {
               debug (20,0)("could not parse headers from on disk structure!\n");
           }

        callback(copy_sz);
	return;
    }
    /*
     * we don't have what the client wants, but at least we now
     * know the swap header size.
     */
    fileRead();
}

int
storeClientCopyPending(store_client * sc, StoreEntry * e, void *data)
{
#if STORE_CLIENT_LIST_DEBUG
    assert(sc == storeClientListSearch(e->mem_obj, data));
#endif
#ifndef SILLY_CODE
    assert(sc);
#endif
    assert(sc->entry == e);
#if SILLY_CODE
    if (sc == NULL)
	return 0;
#endif
    if (!sc->callbackPending())
	return 0;
    return 1;
}

/*
 * This routine hasn't been optimised to take advantage of the
 * passed sc. Yet.
 */
int
storeUnregister(store_client * sc, StoreEntry * e, void *data)
{
    MemObject *mem = e->mem_obj;
#if STORE_CLIENT_LIST_DEBUG
    assert(sc == storeClientListSearch(e->mem_obj, data));
#endif
    if (mem == NULL)
	return 0;
    debug(20, 3) ("storeUnregister: called for '%s'\n", e->getMD5Text());
    if (sc == NULL)
	return 0;
    if (mem->clients.head == NULL)
	return 0;
    if (sc == mem->clients.head->data) {
	/*
	 * If we are unregistering the _first_ client for this
	 * entry, then we have to reset the client FD to -1.
	 */
	mem->fd = -1;
    }
    dlinkDelete(&sc->node, &mem->clients);
    mem->nclients--;
    if (e->store_status == STORE_OK && e->swap_status != SWAPOUT_DONE)
	storeSwapOut(e);
    if (sc->swapin_sio.getRaw()) {
	storeClose(sc->swapin_sio);
	sc->swapin_sio = NULL;
	statCounter.swap.ins++;
    }
    if (sc->callbackPending()){
	/* callback with ssize = -1 to indicate unexpected termination */
	debug(20, 3) ("storeUnregister: store_client for %s has a callback\n",
	    mem->url);
	sc->fail();
    }
#if DELAY_POOLS
    delayUnregisterDelayIdPtr(&sc->delayId);
#endif
#if STORE_CLIENT_LIST_DEBUG
    cbdataReferenceDone(sc->owner);
#endif
    delete sc;
    assert(e->lock_count > 0);
    if (mem->nclients == 0)
	CheckQuickAbort(e);
    return 1;
}

off_t
storeLowestMemReaderOffset(const StoreEntry * entry)
{
    return entry->mem_obj->lowestMemReaderOffset();
}

/* Call handlers waiting for  data to be appended to E. */
void
InvokeHandlers(StoreEntry * e)
{
    /* Commit what we can to disk, if appropriate */
    storeSwapOut (e);
    int i = 0;
    MemObject *mem = e->mem_obj;
    store_client *sc;
    dlink_node *nx = NULL;
    dlink_node *node;

    debug(20, 3) ("InvokeHandlers: %s\n", e->getMD5Text());
    /* walk the entire list looking for valid callbacks */
    for (node = mem->clients.head; node; node = nx) {
	sc = (store_client *)node->data;
	nx = node->next;
	debug(20, 3) ("InvokeHandlers: checking client #%d\n", i++);
	if (!sc->callbackPending())
	    continue;
	if (sc->flags.disk_io_pending)
	    continue;
	storeClientCopy2(e, sc);
    }
}

int
storePendingNClients(const StoreEntry * e)
{
    MemObject *mem = e->mem_obj;
    int npend = NULL == mem ? 0 : mem->nclients;
    debug(20, 3) ("storePendingNClients: returning %d\n", npend);
    return npend;
}

/* return 1 if the request should be aborted */
static int
CheckQuickAbort2(StoreEntry * entry)
{
    size_t curlen;
    size_t minlen;
    size_t expectlen;
    MemObject * const mem = entry->mem_obj;
    assert(mem);
    debug(20, 3) ("CheckQuickAbort2: entry=%p, mem=%p\n", entry, mem);
    if (mem->request && !mem->request->flags.cachable) {
	debug(20, 3) ("CheckQuickAbort2: YES !mem->request->flags.cachable\n");
	return 1;
    }
    if (EBIT_TEST(entry->flags, KEY_PRIVATE)) {
	debug(20, 3) ("CheckQuickAbort2: YES KEY_PRIVATE\n");
	return 1;
    }
    expectlen = entry->getReply()->content_length + entry->getReply()->hdr_sz;
    assert (entry->getReply()->content_length + entry->getReply()->hdr_sz >= 0);
    curlen = (size_t) mem->endOffset ();
    minlen = (size_t) Config.quickAbort.min << 10;
    if (minlen < 0) {
	debug(20, 3) ("CheckQuickAbort2: NO disabled\n");
	return 0;
    }
    if (curlen > expectlen) {
	debug(20, 3) ("CheckQuickAbort2: YES bad content length\n");
	return 1;
    }
    if ((expectlen - curlen) < minlen) {
	debug(20, 3) ("CheckQuickAbort2: NO only little more left\n");
	return 0;
    }
    if ((expectlen - curlen) > (Config.quickAbort.max << 10)) {
	debug(20, 3) ("CheckQuickAbort2: YES too much left to go\n");
	return 1;
    }
    if (expectlen < 100) {
	debug(20, 3) ("CheckQuickAbort2: NO avoid FPE\n");
	return 0;
    }
    if ((curlen / (expectlen / 100)) > (size_t)Config.quickAbort.pct) {
	debug(20, 3) ("CheckQuickAbort2: NO past point of no return\n");
	return 0;
    }
    debug(20, 3) ("CheckQuickAbort2: YES default, returning 1\n");
    return 1;
}

static void
CheckQuickAbort(StoreEntry * entry)
{
    assert (entry);
    if (storePendingNClients(entry) > 0)
	return;
    if (entry->store_status != STORE_PENDING)
	return;
    if (EBIT_TEST(entry->flags, ENTRY_SPECIAL))
	return;
    if (CheckQuickAbort2(entry) == 0)
	return;
    storeAbort(entry);
}

void
store_client::dumpStats(StoreEntry * output, int clientNumber) const
{
    if (callbackPending())
	return;
    storeAppendPrintf(output, "\tClient #%d, %p\n", clientNumber, _callback.callback_data);
    storeAppendPrintf(output, "\t\tcopy_offset: %lu\n",
	(unsigned long) copyInto.offset);
    storeAppendPrintf(output, "\t\tcopy_size: %d\n",
	(int) copyInto.length);
    storeAppendPrintf(output, "\t\tflags:");
    if (flags.disk_io_pending)
	storeAppendPrintf(output, " disk_io_pending");
    if (flags.store_copying)
	storeAppendPrintf(output, " store_copying");
    if (flags.copy_event_pending)
	storeAppendPrintf(output, " copy_event_pending");
    storeAppendPrintf(output, "\n");
}

bool
store_client::callbackPending() const
{
    return _callback.callback_handler && _callback.callback_data;
}

store_client::Callback::Callback(STCB *function, void *data) : callback_handler(function), callback_data (data) {}
