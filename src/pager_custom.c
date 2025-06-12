#include "sqliteInt.h"
#include "pager.h"
#include "wal.h"

int sqlite3NormalErrorBkpt(int val){
    return 1;
}

#define pagerUseWal(x) ((x)->wal!=0)

#ifdef SQLITE_ERROR
#undef SQLITE_ERROR
#endif

// This is very very very very bad.
// But, it works.
#define SQLITE_ERROR sqlite3NormalErrorBkpt(__LINE__)

#define CUSTOM_PAGE_SIZE   4096

#ifdef SQLITE_DEBUG
#define UNEXPECTED() \
    printf("Did not expect at line %d, file %s\n", __LINE__, __FILE__)
#else
#define UNEXPECTED()
#endif

// Multiple pager can refer to the same underlying file.
struct CustomSQLiteFile {
    u8 **ppData;    // This is the file data. It consists of headers
    u64  size;       // This is the file size
    char *fileName; // This is the file name (the file gets identified by this)
};

typedef struct CustomSQLiteFile CustomSQLiteFile;

typedef struct CustomNode CustomNode;

struct CustomNode {
  struct CustomNode *next;
};

typedef PgHdr CustomJournalPage;

struct PagerSavepoint {
    i64 iOffset;
    Bitvec *pInSavepoint;
    Pgno nOrig;
    Pgno iSubRec;
    // TODO: Try implementing the below too. It's actually not essential to the logic itself
    // but is instead a performance optimization.
    int bTruncateOnRelease;
};

typedef struct PagerSavepoint PagerSavepoint;

struct Pager {
    CustomSQLiteFile *pFile;
    u8 eState;
    int nExtra;
    PgHdr *mainJournal; // The main journal
    PgHdr *mainJournalTail; // The main journal tail
    PgHdr *subJournal;  // The sub journal
    PgHdr *subJournalTail; // The main journal tail
    PgHdr *dirtyList;   // Contains the dirty list
    PgHdr *cachedList;  // Contains the cached list of pages
    int mainJournalOffset;
    int subJournalOffset;
    void (*xReinit)(DbPage *);
    int pageSize;
    int nRefSum;
    Bitvec *pInJournal;
    PagerSavepoint *aSavepoint;
    int nSavepoint;
    u64 dbSize;
    u64 dbOrigSize;
    u64 nSubRec;
    u8 *tmpSpace;
    int journal_mode;
    // TBH, I don't like this.
    // I think it should be a void pointer.
    // This way, pager can be completely opaque of the WAL.
    Wal *wal;
};

struct CustomSQLiteDir {
    CustomSQLiteFile **dir;
    int numFile;
    u8 isInitialized;
};

typedef struct CustomSQLiteDir CustomSQLiteDir;

CustomSQLiteDir *customSQLiteDir = NULL;

CustomSQLiteFile * customFindFile(const char *fileName){
    int ind = customSQLiteDir->numFile;
    while (1){
       CustomSQLiteFile * customFile = customSQLiteDir->dir[ind];
       if (customFile == NULL) return NULL;
       if (strcmp(customFile->fileName, fileName) == 0) return customFile;
       ind--;
    }
}

int initializeCustomDir(){
    // Allocate the region for just one item
    if (customSQLiteDir != NULL) return SQLITE_OK;
    customSQLiteDir = malloc(sizeof(CustomSQLiteDir));
    if (customSQLiteDir == 0) return SQLITE_NOMEM;
    memset(customSQLiteDir, 0, sizeof(CustomSQLiteDir));
    customSQLiteDir->dir = malloc(sizeof(CustomSQLiteFile*));
    customSQLiteDir->dir[0] = NULL;
    customSQLiteDir->numFile = 0;
    customSQLiteDir->isInitialized = 1;
    return SQLITE_OK;
}

int addFileToCustomDir(const char* fileName, CustomSQLiteFile **csFile){
    // Need to add 1 because of the fileName str.
    size_t fileStrSize = strlen(fileName) + 1;
    size_t sizeStruct = sizeof(CustomSQLiteFile) + fileStrSize;
    CustomSQLiteFile *customFile = malloc(sizeStruct);
    if (customFile == 0) return SQLITE_NOMEM;
    memset(customFile, 0, sizeStruct);
    customFile->fileName = (char *)(&customFile->fileName + sizeof(char *));
    memcpy(customFile->fileName, fileName, strlen(fileName));
    customSQLiteDir->dir = realloc(customSQLiteDir->dir, sizeof(CustomSQLiteFile*) * (customSQLiteDir->numFile + 1));
    customSQLiteDir->dir[++customSQLiteDir->numFile] = customFile;
    *csFile = customFile;
    return SQLITE_OK;
}

PgHdr *findInDirty(Pager *pPager, u64 pgNo){
    // Check if a page is in the dirty list.
    PgHdr *dirtyPage1 = pPager->dirtyList;
    while (dirtyPage1 != NULL){
        if (dirtyPage1->pgno == pgNo) return dirtyPage1;
        dirtyPage1 = dirtyPage1->pDirtyNext;
    }
    return NULL;
}

PgHdr *findInCached(Pager *pPager, u64 pgNo){
    // Check if the page has already been cached. This also includes the dirty pages.
    // The page can only belong in two lists (this, or the dirty). They are, currently,
    // navigated through the next, and prev. This list is navigated through "prev", while
    // the dirty list is navigated through "next".
    PgHdr *cachedPage1 = pPager->cachedList;
    while (cachedPage1 != NULL){
        if (cachedPage1->pgno == pgNo) {
            cachedPage1->nRef++;
            pPager->nRefSum++;
            return cachedPage1;
        }
        cachedPage1 = cachedPage1->pDirtyPrev;
    }
    return NULL;
}

void *addInCached(Pager *pPager, PgHdr *newHeader){
    // Set the previous of the old head to the new header.
    newHeader->pDirtyPrev = pPager->cachedList;
    // Reset the cached list to the new head.
    pPager->cachedList = newHeader;
}

int add_page_to_file(Pager *pager, Pgno pgno, struct CustomSQLiteFile *c_file){
    // Adjusts the file to contain pgno number of pages.
    // Does not increase the page size.
    if (c_file->size >= pgno) return SQLITE_OK;
    u8 *new_page = malloc(pager->pageSize);
    int rc = SQLITE_OK;
    if (!new_page){
        rc = SQLITE_NOMEM;
        goto error_out;
    }
    u8 **new_data;
    u8 **old_data = c_file->ppData;
    if (c_file->ppData == NULL){
        new_data = malloc(pgno * sizeof(u8*)); 
    }else{
        new_data = realloc(c_file->ppData, pgno * sizeof(u8*));
    }
    if (!new_data){
        rc = SQLITE_NOMEM;
        c_file->ppData = new_data;
        goto error_out;
    }
    c_file->ppData = new_data;
    memset(new_page, 0, pager->pageSize);
    c_file->ppData[pgno-1] = new_page;
    return SQLITE_OK;

error_out:
    if (new_page) free(new_page);
    return rc;
}

PgHdr *getPage(struct CustomSQLiteFile *cSFile, u64 pgNo, Pager *pPager, int flags){
    PgHdr *cachedHeader;
    // If the page is found in the cached list, return it first.
    if ((cachedHeader = findInCached(pPager, pgNo))) return cachedHeader;

    int rc = SQLITE_OK;
    // Eh, handle error better.
    rc = add_page_to_file(pPager, pgNo, cSFile);

    size_t allocSize = pPager->pageSize + sizeof(PgHdr) + ROUND8(pPager->nExtra);
    u8 *allocPtr = malloc(allocSize);
    PgHdr *allocHdr = (PgHdr*)&allocPtr[pPager->pageSize];

    // Copy the page contents -- this is important.
    u8 noContent = (flags & PAGER_GET_NOCONTENT) != 0;
    // if the noContent is set, then we don't care about the actual contents of the page, so,
    // we can skip copy from the main file.
    if (!noContent){
        u8 *buff_to_read = NULL;
        int frame_no = 0;
        if (pagerUseWal(pPager)){
            // Need to check if the page is in the WAL.
            rc = sqlite3WalFindFrame(pPager->wal, pgNo, &frame_no);
            if (frame_no){
                rc = sqlite3WalReadFrame(pPager->wal, frame_no, pPager->pageSize, allocPtr);
            }
        }
        // The read could have been a miss.
        if (frame_no == 0){
            memcpy(allocPtr, cSFile->ppData[pgNo-1], pPager->pageSize);
        }
    }else{
        memset(allocPtr, 0, pPager->pageSize);
    }
    allocHdr->pData = allocPtr;
    // This works since allocHdr is of type PgHdr * (so, using 1 just heads to the extra space)
    allocHdr->pExtra = (u8*)(&allocHdr[1]);
    allocHdr->pgno = pgNo;
    allocHdr->nRef = 1; // This is the case where we have the first reference.
    pPager->nRefSum++;
    allocHdr->pPager = pPager;
    // I don't more fields are needed to initialize at this point.
    // Add the allocated header to the cached list now.
    addInCached(pPager, allocHdr);
    return allocHdr;
}


int sqlite3PagerOpen(
  sqlite3_vfs *pVfs,       /* The virtual file system to use */
  Pager **ppPager,         /* OUT: Return the Pager structure here */
  const char *zFilename,   /* Name of the database file to open */
  int nExtra,              /* Extra bytes append to each in-memory page */
  int flags,               /* flags controlling this file */
  int vfsFlags,            /* flags passed through to sqlite3_vfs.xOpen() */
  void (*xReinit)(DbPage*) /* Function to reinitialize pages */
){
    initializeCustomDir();
    CustomSQLiteFile *customFile = customFindFile(zFilename); // Find the file
    int rc = SQLITE_OK;
    if (customFile == NULL){
        if((rc = addFileToCustomDir(zFilename, &customFile)) != SQLITE_OK){
            return rc;
        }
    }
    Pager *pAlloc = malloc(sizeof(Pager) + CUSTOM_PAGE_SIZE); // Just allocate the pager here.
    if (pAlloc == NULL) return SQLITE_NOMEM_BKPT;
    memset(pAlloc, 0, sizeof(Pager));
    pAlloc->pFile = customFile;
    pAlloc->xReinit = xReinit;
    pAlloc->nExtra = nExtra;
    pAlloc->pageSize = CUSTOM_PAGE_SIZE;
    pAlloc->tmpSpace = (u8*)&pAlloc[1];
    *ppPager = pAlloc;
    return SQLITE_OK;
}

int sqlite3PagerGet(Pager *pPager, Pgno pgno, DbPage **ppPage, int clrFlag){
    PgHdr *page = getPage(pPager->pFile, pgno, pPager, clrFlag);
    *ppPage = page;
    // TODO: Guard against mem failures better.
    return SQLITE_OK;
}

DbPage* sqlite3PagerLookup(Pager *pPager, Pgno pgno){
    return (DbPage*)(findInCached(pPager, pgno));
}

void sqlite3PagerRef(DbPage *pPg){
    pPg->nRef++;
    pPg->pPager->nRefSum++;
}

void sqlite3PagerUnref(DbPage *pPg){
    if (pPg) {
        pPg->nRef--;
        pPg->pPager->nRefSum--;
    }
}

void sqlite3PagerUnrefPageOne(DbPage *pPg){
  sqlite3PagerUnref(pPg);
}

void sqlite3PagerUnrefNotNull(DbPage *pPg){
    sqlite3PagerUnref(pPg);
}

// Checks if the subjournal is required.
// It is required if there is any savepoint with a origSize lesser than the pageNo.
int subjournalIsRequired(PgHdr *pPg){
    Pager *pPager = pPg->pPager;
    PagerSavepoint *pSavepoint = pPager->aSavepoint;
    for (int i = 0; i < pPager->nSavepoint; i++){
        PagerSavepoint current = pSavepoint[i];
        if (current.nOrig >= pPg->pgno && sqlite3BitvecTest(current.pInSavepoint, pPg->pgno) == 0){
            // We'd now need to sub journal this page.
            return 1;
        }
    }
    // No sub-journaling required.
    return 0;
}

void addListEntry(PgHdr **phead, PgHdr **ptail, PgHdr *node);

int addPageToJournal(PgHdr **phead, PgHdr **ptail, PgHdr *pPg, int *offset){
    Pager *pPager = pPg->pPager;
    u8 *jAlloc = malloc(sizeof(PgHdr) + pPager->pageSize);
    PgHdr *jEntry = (PgHdr*)jAlloc;
    jEntry->pData = &jEntry[1];
    addListEntry(phead, ptail, jEntry);
    jEntry->pgno = pPg->pgno;
    memcpy(jEntry->pData, pPg->pData, pPager->pageSize);
    *offset = *offset + 1;
    return SQLITE_OK;
}

void addListEntry(PgHdr **phead, PgHdr **ptail, PgHdr *node){
    int isInit = 0;
    if (!(*phead)){
        *phead = node;
        *ptail = node;
        isInit = 1;
    }
    (*ptail)->pDirtyNext = node;
    if (!isInit) *ptail = node;
}

static int addPageToSavepointBitvecs(Pager *pPager, PgHdr *pPg);

int subJournalPage(PgHdr *pPg){
    Pager *pPager = pPg->pPager;
    addPageToJournal(&pPager->subJournal, &pPager->subJournalTail, pPg, &pPager->subJournalOffset);
    addPageToSavepointBitvecs(pPager, pPg);
    return SQLITE_OK;
}

int subjournalPageIfRequired(PgHdr *pPg){
    if (subjournalIsRequired(pPg)){
        return subJournalPage(pPg);
    }else{
        return SQLITE_OK;
    }
}

static int addPageToSavepointBitvecs(Pager *pPager, PgHdr *pPg){
    for (int i = 0; i < pPager->nSavepoint; i++){
        PagerSavepoint ps = pPager->aSavepoint[i];
        sqlite3BitvecSet(ps.pInSavepoint, pPg->pgno);
    }
    return SQLITE_OK;
}


int checkIfExists(Pager *pPager, PgHdr *pPg){

    PgHdr *dirtyHead = pPager->dirtyList;
    while (dirtyHead){
        if (dirtyHead->pgno == pPg->pgno) return 1;
        dirtyHead = dirtyHead->pDirtyNext;
    }

    return 0;

}
int sqlite3PagerWrite(PgHdr *pPg){
    Pager *pPager = pPg->pPager;
    if ((pPg->flags & PGHDR_WRITEABLE) != 0 && pPager->dbSize >= pPg->pgno){
        if (pPager->nSavepoint) {
            return subjournalPageIfRequired(pPg);
        }
        // In this case, we don't have anything to do,
        return SQLITE_OK;
    }
    // We now follow the usual journal procedure (the value is copied to the main journal)
    // What's nice here is that we don't need to try sub-journaling the page again 
    // (any page written to main journal is, essentially, available for all the savepoints)
    // If the page is using the 
    if (!pagerUseWal(pPager)){
        if (!pPager->pInJournal){
            pPager->pInJournal = sqlite3BitvecCreate(pPager->dbOrigSize);
        }

        if ((sqlite3BitvecTestNotNull(pPager->pInJournal, pPg->pgno) == 0)){
            if (pPg->pgno <= pPager->dbOrigSize){
                #ifdef SQLITE_DEBUG
                printf("Journaling page: %d\n", pPg->pgno);
                #endif
                // Now, we'd need to add the page to the main journal.
                addPageToJournal(&pPager->mainJournal, &pPager->mainJournalTail, pPg, &pPager->mainJournalOffset);
                addPageToSavepointBitvecs(pPager, pPg);
                sqlite3BitvecSet(pPager->pInJournal, pPg->pgno);
            }

        }
    }
    pPg->flags |= PGHDR_WRITEABLE;

    if (pPager->dbSize < pPg->pgno){
        pPager->dbSize = pPg->pgno;
    }

    if (!checkIfExists(pPager, pPg)){
        // Need to update the dirty list.
        pPg->pDirtyNext = pPager->dirtyList;
        pPager->dirtyList = pPg;
    }


    return SQLITE_OK;
}

void sqlite3PagerDontWrite(DbPage *pPg){
    // TODO: Add this. Currently, we skip this optimization.
    return;
}

int sqlite3PagerPageRefcount(DbPage *pPage){
    return pPage->nRef;
}

void *sqlite3PagerGetData(DbPage *pPg){
    return pPg->pData;
}

void *sqlite3PagerGetExtra(DbPage *pPg){
  return pPg->pExtra;
}

void sqlite3PagerPagecount(Pager *pPager, int *pnPage){
    *pnPage = (int)pPager->dbSize;
}

int sqlite3PagerBegin(Pager *pPager, int exFlag, int subjInMemory){
    // Prepare the database for the write transaction.
    pPager->mainJournalOffset = 0;
    pPager->dbOrigSize = pPager->dbSize;
    return SQLITE_OK;
}

int sqlite3PagerCommitPhaseOne(
  Pager *pPager,                  /* Pager object */
  const char *zSuper,            /* If not NULL, the super-journal name */
  int noSync                      /* True to omit the xSync on the db file */
){
    // It's kinda simple enough for us.
    // Just need to put the values from the dirty list into to the file
    int rc = SQLITE_OK;
    if (pagerUseWal(pPager)){
        rc = sqlite3WalFrames(
            pPager->wal,
            pPager->pageSize,
            pPager->dirtyList,
            pPager->dbSize,
            1,
            1
        );
    } else {
        PgHdr *dirtyHead = pPager->dirtyList;
        CustomSQLiteFile *cSF = pPager->pFile;
        while (dirtyHead){
            #ifdef SQLITE_DEBUG
            printf("Writing dirty head: %p, page no: %d\n", dirtyHead, dirtyHead->pgno);
            #endif
            memcpy(cSF->ppData[dirtyHead->pgno-1], dirtyHead->pData, pPager->pageSize);
            dirtyHead = dirtyHead->pDirtyNext;
        }
        // This is actually kinda interesting.
        // The data file has only be written in the rollback journal mode.
        // So, the size of the database is not changed when we're in WAL.
        cSF->size = pPager->dbSize;
    }
    
    pPager->cachedList = NULL;
    pPager->dirtyList = NULL;
    return SQLITE_OK;
}


void resetPager(Pager *pPager){
    pPager->mainJournal = NULL;
    pPager->mainJournalTail = NULL;
    pPager->subJournal = NULL;
    pPager->subJournalTail = NULL;
    pPager->nSavepoint = 0;
    pPager->pInJournal = NULL;
    pPager->mainJournalOffset = 0;
    pPager->subJournalOffset = 0;
    pPager->aSavepoint = NULL;
    pPager->cachedList = NULL;
    pPager->dirtyList = NULL;
}

int sqlite3PagerCommitPhaseTwo(Pager *pPager){
    resetPager(pPager);
    return SQLITE_OK;
}

int sqlite3PagerExclusiveLock(Pager*, DbPage *pPage1, Pgno*){
    return SQLITE_OK;
}

int sqlite3PagerSync(Pager *pPager, const char *zSuper){
    return SQLITE_OK;
}

int sqlite3PagerRollback(Pager* pPager){
    PgHdr *journalNode = pPager->mainJournal;
    do {
        // This branch can never be taken when we have WAL, since journal is not used.
        if (!journalNode) break;
        // Copy the original file contents to the database.
        memcpy(pPager->pFile->ppData[journalNode->pgno-1], journalNode->pData, pPager->pageSize);
        journalNode = journalNode->pDirtyNext;

        // This stop criterion is quite iffy
    } while (journalNode != pPager->mainJournal);
    pPager->pFile->size = pPager->dbOrigSize;
    pPager->dbSize = pPager->dbOrigSize;
    resetPager(pPager);
    return SQLITE_OK;
}

static int pagerOpenSavepoint(Pager *pPager, int nSavepoint){
    // It's actually pretty simple.

    if (pPager->nSavepoint == 0){
        // Need to create array from scratch first.
        pPager->aSavepoint = malloc(nSavepoint * sizeof(PagerSavepoint));
    } else{
        pPager->aSavepoint = realloc(pPager->aSavepoint, nSavepoint * sizeof(PagerSavepoint));
    }

    for (int i = pPager->nSavepoint; i < nSavepoint; i++){
        // We'd now initialize stuff necessary for the savepoints.
        PagerSavepoint *ps = &pPager->aSavepoint[i];
        ps->iOffset = pPager->mainJournalOffset;
        ps->iSubRec = pPager->subJournalOffset;
        ps->nOrig = pPager->dbSize; // This needs to be dbSize because it's a savepoint
        ps->pInSavepoint = sqlite3BitvecCreate(pPager->dbSize);
    }

    pPager->nSavepoint = nSavepoint;
    return SQLITE_OK;
}

int sqlite3PagerOpenSavepoint(Pager *pPager, int nSavepoint){
    if (nSavepoint > pPager->nSavepoint){
        return pagerOpenSavepoint(pPager, nSavepoint);
    }else{
        return SQLITE_OK;
    }
}

static int writeToDirtyList(PgHdr *journalPage, Pager *pPager){
    Pgno jPgno = journalPage->pgno;
    PgHdr *dirtyHead = pPager->dirtyList;
    while (dirtyHead != NULL){
        if (dirtyHead->pgno == jPgno){
            // Ugh. This is kinda weird, but we need to increment the reference here.
            // This needs to be done, because, previously we'd have done a lookup in normal pager
            // which would have incremented the reference
            sqlite3PagerRef(dirtyHead);
            memcpy(dirtyHead->pData, journalPage->pData, pPager->pageSize);
            pPager->xReinit(dirtyHead);
            return 1;
        }
        dirtyHead = dirtyHead->pDirtyNext;
    }
    return 0;
}

static void pagerPlaybackJournalOffset(PgHdr *jHead, PgHdr *jTail, int jOffset, Pager *pPager, Bitvec *writtenBack, int maxSize){
    int tempOffset = 0;
    PgHdr *journalNode = jHead;
    // We'd need to actually write it to the dirtyList
    do {
        if (journalNode == NULL) break;
        Pgno jPgno = journalNode->pgno;
        if (tempOffset >= jOffset && (maxSize == -1 || jPgno <= maxSize)){
            if (sqlite3BitvecTestNotNull(writtenBack, jPgno) == 0){
                if (writeToDirtyList(journalNode, pPager)){
                    sqlite3BitvecSet(writtenBack, jPgno);
                }
            }
        }
        journalNode = journalNode->pDirtyNext;
        tempOffset++;
    } while (journalNode != jTail);
}

static int pagerPlaybackSavepoint(PagerSavepoint *pSavepoint, Pager *pPager){
    // Similar to SQLite's usual savepoint playback. If pSavepoint is NULL, then rollback the entire file.

    // Start from the journalOffset, and just start writing back the data.
    // If pSavepoint is null, then that'd be the case where journalOffset is 0

    int jOffset = 0;
    int finalDbSize = pPager->dbOrigSize;
    if (pSavepoint){
        jOffset = pSavepoint->iOffset;
        finalDbSize = pSavepoint->nOrig;
    }
    
    // pages are written in journal ONLY when they are in the original database, so the bitvec
    // only needs to be of size orig.

    Bitvec *writeBackJournal = sqlite3BitvecCreate(pPager->dbOrigSize);
    pagerPlaybackJournalOffset(pPager->mainJournal, pPager->mainJournalTail, jOffset, pPager, writeBackJournal, -1);

    // Need to journal back from the sub-journal (if savepoint is defined).
    // There can be an edge case that the page to write lies outside the size at this savepoint (we ignore such changes)

    if (pSavepoint){
        const int maxSize = pSavepoint->nOrig;
        int subJOffset = pSavepoint->iSubRec;
        Bitvec *writeBackSubJournal = sqlite3BitvecCreate(maxSize);
        pagerPlaybackJournalOffset(pPager->subJournal, pPager->subJournalTail, subJOffset, pPager, writeBackSubJournal, maxSize);
    }

    pPager->dbSize = finalDbSize;

    return SQLITE_OK;

}


int sqlite3PagerSavepoint(Pager *pPager, int op, int iSavepoint){

    if (iSavepoint > pPager->nSavepoint){
        // Nothing to do in this case
        return SQLITE_OK;
    }

    // If it is a release, then the iSavepoint will also be released.
    int nNew = iSavepoint + ((op == SAVEPOINT_RELEASE) ? 0 : 1);

    // We don't need to realloc the previous list, actually.
    pPager->nSavepoint = nNew;

    // Usually, SQLite when there is a savepoint release, also tries to
    // truncate the sub-journal. But, actually, other than the bigger journal, there 
    // should be no side-effect (so, we don't do that)

    if (op == SAVEPOINT_ROLLBACK){
        // This is the terminal savepoint. We'd need to reset the database state 
        // as it was, when this savepoint was created.
        PagerSavepoint *pSavepoint = (nNew == 0) ? NULL : &pPager->aSavepoint[nNew-1];
        pagerPlaybackSavepoint(pSavepoint, pPager);
    }

    return SQLITE_OK;
}

int sqlite3PagerSharedLock(Pager *pPager){
    
    // Now, we'd also open the WAL if jorunal mode is WAL.
    int rc = SQLITE_OK;
    int is_open = 0;
    Pgno num_pages = 0;
    if (pPager->journal_mode == PAGER_JOURNALMODE_WAL){
        rc = sqlite3PagerOpenWal(pPager, &is_open);
        num_pages = sqlite3WalDbsize(pPager->wal);
        rc = sqlite3WalBeginReadTransaction(pPager->wal, NULL);
        sqlite3WalEndReadTransaction(pPager->wal);
    }
    if (!num_pages){
        // number of pages can be zero if the wal has not been written to
        // in that case, num_pages will be simply number of pages in the file
        num_pages = pPager->pFile->size;
    }
    return rc;
}


void sqlite3PagerSetBusyHandler(Pager*, int(*)(void *), void *){}

int sqlite3PagerSetPagesize(Pager* pPager, u32* pPtr, int size){
    *pPtr = pPager->pageSize;
    return SQLITE_OK;
}

Pgno sqlite3PagerMaxPageCount(Pager*, Pgno){
    UNEXPECTED();
    return -1;
}
void sqlite3PagerSetCachesize(Pager*, int){
    UNEXPECTED();
}

int sqlite3PagerSetSpillsize(Pager*, int){
    UNEXPECTED();
    return SQLITE_NOMEM_BKPT;
}

void sqlite3PagerSetMmapLimit(Pager *, sqlite3_int64){
    UNEXPECTED();
}

void sqlite3PagerShrink(Pager*){
    UNEXPECTED();
}

void sqlite3PagerSetFlags(Pager*,unsigned){
    UNEXPECTED();
}

int sqlite3PagerLockingMode(Pager *, int){
    UNEXPECTED();
    return SQLITE_OK;
}

int sqlite3PagerSetJournalMode(Pager * pager, int mode){
    pager->journal_mode = mode;
    return SQLITE_OK;
}


int sqlite3PagerGetJournalMode(Pager* pager){
    return pager->journal_mode;
}

int sqlite3PagerOkToChangeJournalMode(Pager*){
    return 1;
}

i64 sqlite3PagerJournalSizeLimit(Pager *, i64){
    return -1;
}

sqlite3_backup **sqlite3PagerBackupPtr(Pager*){
    return NULL;
}

int sqlite3PagerFlush(Pager*){
    return SQLITE_OK;
}

void sqlite3PagerRekey(DbPage *pPg, Pgno iNew, u16 flags){
    pPg->flags = flags;
    pPg->pgno = iNew;
}

void sqlite3PagerTruncateImage(Pager *pPager, Pgno nPage){
    pPager->dbSize = nPage;
}


u8 sqlite3PagerIsreadonly(Pager*){
    UNEXPECTED();
    return 0;
}

u32 sqlite3PagerDataVersion(Pager*){
    UNEXPECTED();
    return 1;
}

int sqlite3PagerMemUsed(Pager*){
    UNEXPECTED();
    return 1;
}

const char *sqlite3PagerFilename(const Pager* pPager, int){
    UNEXPECTED();
    return pPager->pFile->fileName;
}

sqlite3_vfs *sqlite3PagerVfs(Pager*){
    UNEXPECTED();
    return NULL;
}

sqlite3_file *sqlite3PagerFile(Pager* pPager){
    UNEXPECTED();
    return (sqlite3_file*)pPager->pFile;
}

sqlite3_file *sqlite3PagerJrnlFile(Pager* pPager){
    UNEXPECTED();
    return (sqlite3_file*)pPager->mainJournal;
}

const char *sqlite3PagerJournalname(Pager* pPager){
    UNEXPECTED();
    return sqlite3PagerFilename(pPager, -1);
}

void *sqlite3PagerTempSpace(Pager* pPager){
    UNEXPECTED();
    return pPager->tmpSpace;
}

int sqlite3PagerIsMemdb(Pager*){
    UNEXPECTED();
    return 0;
}

void sqlite3PagerCacheStat(Pager *, int, int, u64*){
    UNEXPECTED();
}

void sqlite3PagerClearCache(Pager*){
    UNEXPECTED();
}

int sqlite3SectorSize(sqlite3_file *){
    return 512;
}

int sqlite3PagerClose(Pager *pPager, sqlite3*){
    return SQLITE_OK;
}

int sqlite3PagerReadFileheader(Pager *pPager, int N, unsigned char *pDest){
    CustomSQLiteFile *pFile = pPager->pFile;
    memset(pDest, 0, N);
    if (pFile->size > 0){
        memcpy(pDest, pFile->ppData[0], N);
    }
    return SQLITE_OK;
}

int sqlite3PagerMovepage(Pager*,DbPage*,Pgno,int){
    UNEXPECTED();
    return SQLITE_ERROR;
}

int sqlite3PagerCheckpoint(Pager *pPager, sqlite3*, int, int*, int*){
    UNEXPECTED();
    return SQLITE_ERROR;
}

int sqlite3PagerWalSupported(Pager *pPager){
    return 1;
}

int sqlite3PagerWalCallback(Pager *pPager){
    UNEXPECTED();
    return SQLITE_OK;
}

int sqlite3PagerOpenWal(Pager *pPager, int *pisOpen){
    int rc = SQLITE_OK;
    int is_open = 0;
    if (pPager->wal){
        is_open = 1;
        goto out;
    }
    Wal *wal;
    rc = sqlite3ModWalOpen(&wal, pPager->pFile->fileName, pPager->pageSize, pPager->dbSize);
    if (rc != SQLITE_OK) goto out;
    is_open = 1;
    pPager->wal = wal;
out:
    if (pisOpen) *pisOpen = is_open;
    return rc;
}

int sqlite3PagerCloseWal(Pager *pPager, sqlite3*){
    UNEXPECTED();
    return SQLITE_ERROR;
}

int sqlite3PagerDirectReadOk(Pager *pPager, Pgno pgno){
    UNEXPECTED();
    return 0;
}

int sqlite3PagerUsePage(Pager *pPager, Pgno pgno){
    UNEXPECTED();
    return SQLITE_OK;
}

void sqlite3PagerEndConcurrent(Pager*){}

int sqlite3PagerBeginConcurrent(Pager*){
    UNEXPECTED();
    return SQLITE_ERROR;
}

void sqlite3PagerDropExclusiveLock(Pager *){
    UNEXPECTED();
}

int sqlite3PagerUpgradeSnapshot(Pager *pPager, DbPage*){
    UNEXPECTED();
    return SQLITE_ERROR;
}

void sqlite3PagerSetDbsize(Pager *pPager, Pgno nSz){
  pPager->dbSize = nSz;
}

int sqlite3PagerIsWal(Pager* pager){
    return pager->journal_mode == PAGER_JOURNALMODE_WAL;
}

int sqlite3PagerRefcount(Pager* pPager){
    return pPager->nRefSum;
}

Pgno sqlite3PagerPagenumber(DbPage *pPg){
  return pPg->pgno;
}

int sqlite3PagerIswriteable(DbPage*){
    UNEXPECTED();
    return 1;
}

int sqlite3PagerWalInfo(Pager*, u32 *pnPrior, u32 *pnFrame){
    UNEXPECTED();
    return SQLITE_ERROR;
}

sqlite3_file *sqlite3_database_file_object(const char *zName){
    UNEXPECTED();
    return NULL;
}

// int main(){
//     if (initializeCustomDir() == SQLITE_OK){
//         printf("Created the dir successfully!\n");
//     }
//     if (addFileToCustomDir("test_1") == SQLITE_OK){
//         printf("Added file1 correctly!\n");
//     }
//     if (addFileToCustomDir("test_2") == SQLITE_OK){
//         printf("Added file2 correctly!\n");
//     }

//     struct CustomSQLiteFile *file1 = customFindFile("test_1");
//     printf("Result of file search: %p\n", file1);
//     printf("Result of file search: %p\n", customFindFile("test_2"));

//     printf("Result of getting first page: %s", (char*)getPage(file1, 30));
// }

#ifdef SQLITE_ERROR
#undef SQLITE_ERROR
#endif 

#define SQLITE_ERROR 1