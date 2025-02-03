// This is a custom pager implementation, which by passes journaling mechanism.
// The idea is to run SQLite directly on the SQLiteOS.
// A lot of the code is adapted from pager.c
// The code, still works on an OS other than SQLiteOS.

#include "sqliteInt.h"

// We don't need _ALL_ the pager states though. The below subset is, actually, enough.
#define PAGER_OPEN                  0
#define PAGER_READER                1
#define PAGER_WRITER                2
#define PAGER_WRITER_CACHEMOD       4
#define PAGER_WRITER_FINISHED       5
// #define PAGER_WRITER_LOCKED         2
// #define PAGER_WRITER_CACHEMOD       3
// #define PAGER_WRITER_DBMOD          4
// #define PAGER_WRITER_FINISHED       5
#define PAGER_ERROR                 6


#define UNKNOWN_LOCK                (EXCLUSIVE_LOCK+1)

// The pager structure is significantly simplified, partly because this implementation
// only is supposed to work on the specific case of SQLiteOS.

struct Pager {
    sqlite3_vfs *pVfs;
    u8 eState;
    u8 eLock;
    Pgno dbSize;
    Pgno dbOrigSize;
    Pgno dbFileSize;
    Pgno dbHintSize;
    int errCode;
    sqlite3_file *fd; // We don't have a journal here!
    char *zFilename;
    i64 pageSize;
    // Reserved size, set at the time of open.
    i64 nReserved;
    int (*xGet)(Pager*,Pgno,DbPage**,int);
    int intent;
    DbPage *dirtyHead;
};

// TODO: Enable this?.
int sqlite3PagerDirectReadOk(Pager *, Pgno){
    return 0; // Eh, whatever. 
}

/*
** Read a 32-bit integer from the given file descriptor.  Store the integer
** that is read in *pRes.  Return SQLITE_OK if everything worked, or an
** error code is something goes wrong.
**
** All values are stored on disk as big-endian.
*/
static int read32bits(sqlite3_file *fd, i64 offset, u32 *pRes){
  unsigned char ac[4];
  int rc = sqlite3OsRead(fd, ac, sizeof(ac), offset);
  if( rc==SQLITE_OK ){
    *pRes = sqlite3Get4byte(ac);
  }
  return rc;
}

#define put32bits(A,B)  sqlite3Put4byte((u8*)A,B)

/*
** Write a 32-bit integer into the given file descriptor.  Return SQLITE_OK
** on success or an error code is something goes wrong.
*/
static int write32bits(sqlite3_file *fd, i64 offset, u32 val){
  char ac[4];
  put32bits(ac, val);
  return sqlite3OsWrite(fd, ac, 4, offset);
}


static int pagerUnlockDb(Pager *pPager, int eLock){
    assert ( eLock == NO_LOCK || eLock == SHARED_LOCK );
    // I don't think there is actually a need 
}

void sqlite3PagerSetBusyHandler(Pager*, int(*)(void *), void *){
  // Do nothing.
}

int sqlite3PagerSetPagesize(Pager *pPager, u32 *pPageSize, int nReserve){

  int rc = SQLITE_OK;

  u32 pageSize = *pPageSize;
  assert( pageSize == -1 || pageSize==0 || (pageSize>=512 && pageSize<=SQLITE_MAX_PAGE_SIZE) );
  
  // I don't imagine currently needing this.
  assert ( !pageSize || pageSize == (u32)pPager->pageSize || pageSize == -1);

  if (pageSize == -1){
    pageSize = (u32)pPager->pageSize;
  }

  i64 nByte = 0;
  if (isOpen(pPager->fd)){
    rc = sqlite3OsFileSize(pPager->fd, &nByte);
  }

  if ( rc == SQLITE_OK ){
    // we can now simply set the dbSize
    pPager->dbSize = (Pgno)((nByte + pageSize - 1) / pageSize);
    pPager->pageSize = pageSize;
  }
  *pPageSize = pPager->pageSize;

  if ( nReserve < 0 ) nReserve = pPager->nReserve;
  pPager->nReserve = (i16)nReserve;
  return rc;
}

Pgno sqlite3PagerMaxPageCount(Pager *pPager, Pgno mxPage){
  if( mxPage>0 ){
    pPager->mxPgno = mxPage;
  }
  assert( pPager->eState!=PAGER_OPEN );      /* Called only by OP_MaxPgcnt */
  /* assert( pPager->mxPgno>=pPager->dbSize ); */
  /* OP_MaxPgcnt ensures that the parameter passed to this function is not
  ** less than the total number of valid pages in the database. But this
  ** may be less than Pager.dbSize, and so the assert() above is not valid */
  return pPager->mxPgno;
}

void sqlite3PagerSetCachesize(Pager *, int){
  // Don't do anything. We currently don't support cache. 
  // TODO: Become clever enough to do that.
}

int sqlite3PagerSetSpillsize(Pager *, int){
  assert(0); // Just crash
}

void sqlite3PagerSetMmapLimit(Pager *, sqlite3_int64){
  // Don't do anything.
}

void sqlite3PagerShrink(Pager *){
  // Don't do anything.
}

void sqlite3PagerSetFlags(Pager *, unsigned){
  // Don't do anything.
}

int sqlite3PagerLockingMode(Pager *, int){
  // Don't do anything.
}

int sqlite3PagerSetJournalMode(Pager *pPager, int){
  return PAGER_JOURNALMODE_DELETE;
}

int sqlite3PagerSetJournalMode(Pager *pPager, int){
  return PAGER_JOURNALMODE_DELETE;
}

int sqlite3PagerOkToChangeJournalMode(Pager *){
  return 0; // We don't even journal.
}

i64 sqlite3PagerJournalSizeLimit(Pager *, i64){
  assert(0);
}

sqlite3_backup **sqlite3PagerBackupPtr(Pager*){
  assert(0);
  return NULL;
}

int sqlite3PagerFlush(Pager *pPager){
  int rc;
  if (rc = pPager->errCode){
    return rc;
  }
  return SQLITE_OK;
}

DbPage *sqlite3PagerLookup(Pager *pPager, Pgno pgno){
  // We don't have a cache, so always force higher levels to rebuild mem structures
  return NULL;
}

void sqlite3PagerRef(DbPage*){
  return;
}

void sqlite3PagerUnref(DbPage*){
  return;
}

void sqlite3PagerUnrefNotNull(DbPage*){
  return;
}

void sqlite3PagerUnrefPageOne(DbPage*){
  return;
}


int sqliteOSBeginTransaction(int intent){
  intent = intent != 0; // cast anything not read as a write (1)
  // make syscall to begin transaction.
}

// A fake upgrader.
static int pagerLockDb(Pager *pPager, int eLock){
  int rc = SQLITE_OK;

  assert ( eLock==SHARED_LOCK || eLock == RESERVED_LOCK || eLock == EXCLUSIVE_LOCK );
  if (pPager->eLock < eLock || pPager->eLock == UNKNOWN_LOCK ){
    rc = SQLITE_OK;
    if (pPager->eLock != UNKNOWN_LOCK || eLock == EXCLUSIVE_LOCK){
      pPager->eLock = (u8)eLock;
    }
  }
  return rc;
}

static int pagerPagecount(Pager *pPager, Pgno *pnPage){
  Pgno nPage;

  assert( pPager->eState == PAGER_OPEN );
  assert( pPager->eLock >= SHARED_LOCK );
  assert( isOpen(pPager->fd) );

}

// This is messy, and actually a change in behavior than the previous pager.
// Generally, this function also gets called to get the first page, which happens
// before we get a chance to acquire an exlcusing/reserved lock. In that case,
// we would't obey transactional semantics (since SQLiteOS does not allow read upgrades to writes.)
int sqlite3PagerSharedLock(Pager *pPager, int lockIntent) {
  int rc = SQLITE_OK;
  // We actually start our transaction right here. That is, if the lockintent is read,
  // we start a read transaction against the operating system. Otherwise, we start a write
  // transaction.
  // we also don't handle the case where the hotjournal persists (since we don't have that kind of journaling)
  assert( pPager->eState == PAGER_OPEN || pPager->eState == PAGER_READER );
  if (pPager->eState == PAGER_OPEN){
    // This will start the necessary transaction.
    // We still don't need to necessary to pager state, since it is possible that the query in write intent
    // transaction are still readonly.
    sqliteOSBeginTransaction(lockIntent);
    // NOTE: we still make this shared lock because that's what it really is.
    // Effectivelly, locking is useless (and unnecessary) in SQLiteOS since kernel-level locking occurs.
    pagerLockDb(pPager, SHARED_LOCK);
  }
  if ( pPager->eState == PAGER_OPEN && rc == SQLITE_OK ){
    assert( sqlite3PagerSetPagesize(pPager, -1, -1) == SQLITE_OK );
  }
  
  assert ( rc == SQLITE_OK );
  pPager->eState = PAGER_READER;
  pPager->intent = lockIntent;
  return rc;
}

static int getPageNormal(
  Pager *pPager,
  Pgno pgno,
  // This is a bit wacky. We actually don't need any header and can just directly return
  // the data page. But, we need to be backwards compatible.
  DbPage **ppPage,
  int flags
){
  int rc = SQLITE_OK;
  PgHdr *pPg;

  // We actually don't care about nocontent pages. We, do, however need to zero them out
  // Since we don't do any journaling, the no-content optimization doesn't apply to us.
  u8 noContent;
  
  assert ( pPager->errCode == SQLITE_OK );
  assert ( pPager->eState >= PAGER_READER );

  if ( pgno == 0 ) return SQLITE_CORRUPT_BKPT;

  // Allocate the entire memory at once.
  i64 totalSize = pPager->pageSize + pPager->nReserved + sizeof(*pPg);

  // Create the page object.
  // We actually don't have any specific format here,
  // so this is actually just raw data + stuff from nReserved.
  // Typically, SQLite orders the database page buffer _before_ the mempage and others.
  // I'm not sure if we need to mimic that, so I am.
  // Apparently, SQLite docs is actually wrong about this.
  void *pAll = sqlite3MallocZero(totalSize);


  // In my case, the page layut is like:
  // [ DatabasePage | MemPage (or whatever is extra) | PgHdr ]
  // Eh, need to byte maniplulatin
  pPg = (PgHdr*)&(u8*)pAll[pPager->pageSize + pPager->nReserved];
  pPg->pData = pAll; // whatever, assign the data to the first chunk.
  pPg->pExtra = &(u8*)pAll[pPager->pageSize];

  // we don't have a caching mechanisn, we just go ahead and read from the file system.
  pPg->pPager = pPager;
  // We technically don't need this, right?
  memset(pPg->pExtra, 0, 8);
  // We also don't care about handling the case of noContent, since it is 
  // useful for I/O optimization, which we don't bother yet.
  // It'll be a nice addition here though.
  pPg->pgno = pgno;
  pPg->pCache = NULL;
  // I don't think we'd be using the flag too...
  pPg->flags = PGHDR_CLEAN;
  if (!pPg){
    return SQLITE_NOMEM_BKPT;
  }
  // We now actually read the file.
  rc = readDbPage(pPg);
  
  if ( rc != SQLITE_OK ){
    goto pager_get_err;
  }

  return rc;

pager_get_err:
  assert( rc != SQLITE_OK );
  if (pAll){
    sqlite_free(pAll);
  }
  *ppPage = NULL;
  return rc;
}

static int readDbPage(PgHdr *pPg){
  // TODO: Test if we can actually omit storing pointer to pager.
  Pager *pPager = pPg->pPager;

  assert( pPager->eState>= PAGER_READER );
  assert( isOpen(pPager->fd) );

  i64 iOffset = (pPg->pgno - 1)*(i64)(pPager->pageSize);
  int rc = sqlite3OsRead(pPager->fd, pPg->pData, pPager->pageSize, iOffset);

  if ( rc == SQLITE_IOERR_SHORT_READ ){
    rc = SQLITE_OK;
  }
  return rc;
}

// TODO: Is this a complicated design pattern?
static int getPageError(
  Pager *pPager,
  Pgno pgno,
  DbPage **ppPage,
  int flags
){
  UNUSED_PARAMETER(pgno);
  UNUSED_PARAMETER(flags);
  assert( pPager->errCode!=SQLITE_OK );
  *ppPage = 0;
  return pPager->errCode;
}


static void setGetterMethod(Pager *pPager){
  if ( pPager->errCode ){
    pPager->xGet = getPageError;
  }else{
    pPager->xGet = getPageNormal;
  }
}

int sqlite3PagerGet(
  Pager *pPager,
  Pgno pgno,
  DbPage **pPage,
  int flags
){
  return pPager->xGet(pPager, pgno, pPage, flags);
}

int sqlite3PagerBegin(Pager *pPager, int exFlag, int subjInMemory){
  int rc = SQLITE_OK;
  if ( pPager->errCode ) return pPager->errCode;
  assert( pPager->eState >= PAGER_READER && pPager->eState < PAGER_ERROR);
  if ( pPager->eState == PAGER_READER ){
    pPager->eState = PAGER_WRITER;
  }
  return rc;
}

// Quite simple compared to what we see in the pager.c
int sqlite3PagerWrite(PgHdr *pPg){
  Pager *pPager = pPg->pPager;
  // Assert that called sharedLock acquiring function
  assert( pPager->intent != 0 );
  if ( (pPg->flags & PGHDR_WRITEABLE) == 0) {
    // the value was not set already writable. So, we just add it to the dirty page list.
    PgHdr *dirty = pPager->dirtyHead;
    // I don't see the value yet of making it double-linked. So, this is actually single linked.
    pPg->pDirtyNext = pPager->dirtyHead;
    pPager->dirtyHead = pPg;
  }
  pPg->flags |= PGHDR_WRITEABLE;
  // Even if we are in a savepoint or something, we still don't need to do anything special.
  if ( pPager->dbSize < pPg->pgno ){
    pPager->dbSize = pPg->pgno;
  }
  return SQLITE_OK;
}

void sqlite3PagerDontWrite(DbPage*){
  // TODO: We don't perform this optimization.
  // Looks promising though.
}

int sqlite3PagerMovepage(Pager *pPager, DbPage *pPg, Pgno pgno, int isCommit){
  // I don't think this will be called, since we don't do the autovacuum....
  return SQLITE_MISUSE_BKPT;
}

int sqlite3PagerPageRefcount(DbPage*){
  // Eh
  return 1;
}

void *sqlite3PagerGetData(DbPage *pPg){
  return pPg->pData;
}

void *sqlite3PagerGetExtra(DbPage *pPg){
  return pPg->pExtra;
}

void sqlite3PagerPagecount(Pager *pPager, int *pnPage){
  assert( pPager->eState>=PAGER_READER );
  *pnPage = (int)pPager->dbSize;
}

static int pager_write_pagelist(Pager *pPager){
  int rc = SQLITE_OK;
  PgHdr *pList = pPager->dirtyHead;
  while ( rc == SQLITE_OK && pList ){
    Pgno pgno = pList->pgno;
    if ( pgno <= pPager->dbSize ){
      i64 offset = (pgno - 1)*(i64)(pPager->pageSize);
      char *pData;
      pData = pList->pData;
      rc = sqlite3OsWrite(pPager->fd, pData, pPager->pageSize, offset);
      // Remember, we don't use version info for anything.
      // After page write, the pager.c version also updates the version info
      // we don't need that. It is fine if is just empty (since it is also used for eviction purposes, which we again don't support)
      if (pgno > pPager->dbFileSize){
        pPager->dbFileSize = pgno;
      }
    }
    pList = pList->pDirtyNext;
  }
  return rc;
}

// This is complicated.
int sqlite3PagerCommitPhaseOne(
  Pager *pPager,                  /* Pager object */
  const char *zSuper,            /* If not NULL, the super-journal name */
  int noSync                      /* True to omit the xSync on the db file */
){
  int rc = SQLITE_OK;
  assert( pPager->eState == PAGER_READER );
  if( NEVER(pPager->errCode) ) return pPager->errCode;
  
  // We technically don't care if the pages are unsored by pageno
  // since we don't do any serial I/O optimization.....
  // I guess this could be a micro-optimization, ignoring it for now.
  PgHdr *pList = pPager->dirtyHead;
} 