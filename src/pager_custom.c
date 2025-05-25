#include "sqliteInt.h"
#include "pager.h"

#define CUSTOM_PAGE_SIZE   4096
#define UNEXPECTED() \
    printf("Did not expect at line %d, file %s\n", __LINE__, __FILE__)

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

int pagerSharedLock(Pager *){
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

PgHdr *getPage(struct CustomSQLiteFile *cSFile, u64 pgNo, Pager *pPager, int flags){
    PgHdr *cachedHeader;
    // If the page is found in the cached list, return it first.
    if ((cachedHeader = findInCached(pPager, pgNo))) return cachedHeader;
    
    if (cSFile->size < pgNo){
        char **newData;
        if (cSFile->ppData == NULL){
            newData = malloc(pgNo * sizeof(u8*));
        }else{
            newData = realloc(cSFile->ppData, pgNo * sizeof(u8*));
        }
        cSFile->ppData = (u8 **)newData;
        // Allocate the page.
        cSFile->ppData[pgNo-1] = malloc(pPager->pageSize);
    }

    size_t allocSize = pPager->pageSize + sizeof(PgHdr) + ROUND8(pPager->nExtra);
    u8 *allocPtr = malloc(allocSize);
    PgHdr *allocHdr = (PgHdr*)&allocPtr[pPager->pageSize];

    // Copy the page contents -- this is important.
    u8 noContent = (flags & PAGER_GET_NOCONTENT) != 0;
    // if the noContent is set, then we don't care about the actual contents of the page, so,
    // we can skip copy from the main file.
    if (!noContent){
        memcpy(allocPtr, cSFile->ppData[pgNo-1], pPager->pageSize);
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
    Pager *pAlloc = malloc(sizeof(Pager)); // Just allocate the pager here.
    if (pAlloc == NULL) return SQLITE_NOMEM_BKPT;
    memset(pAlloc, 0, sizeof(Pager));
    pAlloc->pFile = customFile;
    pAlloc->xReinit = xReinit;
    pAlloc->nExtra = nExtra;
    pAlloc->pageSize = CUSTOM_PAGE_SIZE;
    pAlloc->tmpSpace = malloc(sizeof(pAlloc->pageSize));
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


int checkIfExists(Pager *pPager, PgHdr *pPg){\

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
    }
    // We now follow the usual journal procedure (the value is copied to the main journal)
    // What's nice here is that we don't need to try sub-journaling the page again 
    // (any page written to main journal is, essentially, available for all the savepoints)
    
    if (!pPager->pInJournal){
        pPager->pInJournal = sqlite3BitvecCreate(pPager->dbOrigSize);
    }

    if ((sqlite3BitvecTestNotNull(pPager->pInJournal, pPg->pgno) == 0)){
        if (pPg->pgno <= pPager->dbOrigSize){
            printf("Journaling page: %d\n", pPg->pgno);
                        // Now, we'd need to add the page to the main journal.
            addPageToJournal(&pPager->mainJournal, &pPager->mainJournalTail, pPg, &pPager->mainJournalOffset);
            addPageToSavepointBitvecs(pPager, pPg);
            sqlite3BitvecSet(pPager->pInJournal, pPg->pgno);
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
    PgHdr *dirtyHead = pPager->dirtyList;
    CustomSQLiteFile *cSF = pPager->pFile;
    while (dirtyHead){
        printf("Writing dirty head: %p, page no: %d\n", dirtyHead, dirtyHead->pgno);
        memcpy(cSF->ppData[dirtyHead->pgno-1], dirtyHead->pData, pPager->pageSize);
        dirtyHead = dirtyHead->pDirtyNext;
    }
    cSF->size = pPager->dbSize;
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
    PgHdr *journalNode = pPager->mainJournalTail;
    do {
        if (!journalNode) break;
        // Copy the original file contents to the database.
        memcpy(pPager->pFile->ppData[journalNode->pgno-1], journalNode->pData, pPager->pageSize);
        journalNode = journalNode->pDirtyNext;
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
        pPager->aSavepoint = malloc(sizeof(PagerSavepoint));
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
        // Open the savepoints
    }else{
        return SQLITE_OK;
    }
}

int sqlite3PagerSavepoint(Pager *pPager, int op, int iSavepoint){
    printf("Calling savepoint (unexpected!)");
    return SQLITE_OK;
}

int sqlite3PagerSharedLock(Pager *pPager){
    pPager->dbSize = pPager->pFile->size;
    return SQLITE_OK;
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

int sqlite3PagerSetJournalMode(Pager *, int){
    UNEXPECTED();
    return SQLITE_OK;
}
int sqlite3PagerGetJournalMode(Pager*){
    UNEXPECTED();
    return PAGER_JOURNALMODE_DELETE;
}
int sqlite3PagerOkToChangeJournalMode(Pager*){
    return 0;
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
    UNEXPECTED();
    return SQLITE_OK;
}

int sqlite3PagerWalCallback(Pager *pPager){
    UNEXPECTED();
    return SQLITE_OK;
}

int sqlite3PagerOpenWal(Pager *pPager, int *pisOpen){
    UNEXPECTED();
    return SQLITE_ERROR;
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

int sqlite3PagerIsWal(Pager*){
    return 0;
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
