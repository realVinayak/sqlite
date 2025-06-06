// Implements a custom WAL.

#include "wal.h"
#include "sqliteInt.h"

// TODO: Make this macro part of general header file?
#ifdef SQLITE_DEBUG
#define FFLUSH_UNEXPECTED() \
	do { \
    printf("Did not expect at line %d, file %s\n", __LINE__, __FILE__); \
	fflush(stdout); \
	} while(0);
#else
#define FFLUSH_UNEXPECTED()
#endif

/*
 * TODO: 
 * 1. Add support for checkpoints
 * 2. Add locking. Will be done later in kernel version
 * 3. Think about retries - make them as rare as possible.
 *	- in the kernel version, we can make things quite atomic.
 * 4. Sort when checkpointing?
 * 	- It'll be a nice benchmark to add.
 */

/*
 * Current implementation notes:
 * 1. No checkpoints. The WAL grows as much as possible
 */
struct storage_node {
	void *data;
	void *key;
};

struct storage_dir {
	struct storage_node **dir;
	unsigned int item_count;
	int (*comp)(void *, const void *);
};

struct wal_log {
	// This stores the log.
	// Since WAL file is always accessed end-to-start, we don't need a tail.
	// More and more, this is a bad idea. It'd be better to make this an array.
	PgHdr *log;
	u32 page_count;
	u32 page_size;
	Pgno db_size;
};

struct Wal {
	// Similar to read mark concept in original WAL.
	int read_mark;
	struct wal_log *p_wal_log;
	u32 iCallback;
};


void initialize_wal_log_entry(void *ptr, void *context){
	struct wal_log *p_wal_log = (struct wal_log *)ptr;
	p_wal_log->log = NULL;
	p_wal_log->page_count = 0;
	p_wal_log->page_size = ((struct wal_log *)context)->page_size;
	p_wal_log->db_size = ((struct wal_log *)context)->db_size;
}

int initialize_storage_dir(struct storage_dir **pp_dir,  int (*comp)(void *, const void*)){
	if (*pp_dir != NULL) return SQLITE_OK;
	struct storage_dir *p_dir = malloc(sizeof(struct storage_dir));
	if (!p_dir) goto error_out;
	p_dir->item_count = 0;
	p_dir->dir = malloc(sizeof(struct storage_node *));
	if (!p_dir->dir) goto error_out;
	p_dir->dir[0] = NULL;
	p_dir->comp = comp;
	*pp_dir = p_dir;
	return SQLITE_OK;

error_out:
	free(p_dir);
	return SQLITE_NOMEM_BKPT;
}

void * find_storage_node(struct storage_dir *p_dir, const void *key){
	unsigned int ind = p_dir->item_count;
	while (1){
		struct storage_node *sn = p_dir->dir[ind];
		if (sn == NULL) return NULL;
		if (p_dir->comp(sn->key, key) == 0) return sn->data;
		ind--;
	}
}

int add_storage_node(struct storage_dir *p_dir, const void *key, void *data, size_t size_key){
	u8 *ptr = malloc(size_key + sizeof(struct storage_node));
	if (!ptr) goto error_out;
	p_dir->dir = realloc(p_dir->dir, (p_dir->item_count+1)*sizeof(struct storage_node*));
	if (!p_dir->dir) goto error_out;
	struct storage_node *sn = (struct storage_node *)ptr;
	sn->data = data;
	sn->key = &sn[1];
	// This way, the caller can free the key.
	memcpy(sn->key, key, size_key);
	p_dir->dir[++p_dir->item_count] = sn;
	return SQLITE_OK;

error_out:
	free(ptr);
	return SQLITE_NOMEM_BKPT;
}


int find_or_create(
	struct storage_dir *p_dir, 
	const void *key, 
	size_t size_obj, 
	size_t size_key,
	void**obj_out,
	void initialize(void *, void *),
	void *context
){

	void * sn = find_storage_node(p_dir, key);
	int rc;

	if (sn){
		*obj_out = sn;
		goto success_out;
	}

	*obj_out = malloc(size_obj);
	if (*obj_out == NULL){
		rc = SQLITE_NOMEM_BKPT;
		goto error_out;
	}

	if ((rc = add_storage_node(p_dir, key, *obj_out, size_key))) goto error_out;
	initialize(*obj_out, context);

success_out:
	return 0;

error_out:
	if (*obj_out) free(*obj_out);
	return rc;
}

int str_cmp_cast(void * str1, const void *str2){
	return strcmp((const char *)str1, (const char *)str2);
}

static struct storage_dir *g_wal_logs = NULL;

int initialize_wal_log(){
	return initialize_storage_dir(&g_wal_logs, str_cmp_cast);
}

struct wal_log *find_wal_log(const char *key, size_t compare_count){
	return (struct wal_log*)find_storage_node(g_wal_logs, (void*)key);
}

int sqlite3ModWalOpen(
	// TODO: Get rid of the typedefs. Not acceptable in Linux kernel.
	Wal **ppWal,
	const char *zWalName,
	u32 page_size,
	Pgno current_db_size
){
	int rc = SQLITE_OK;
	Wal *wal = NULL;
	if ((rc =initialize_wal_log()) != SQLITE_OK) return rc;
	struct wal_log *curr_wal_log = NULL;
	wal = malloc(sizeof(Wal));
	

	if (wal == NULL){
		rc = SQLITE_NOMEM;
		goto error_out;
	}

	struct wal_log temp_wal_log;
	temp_wal_log.page_size = page_size;
	temp_wal_log.db_size = current_db_size;
	
	rc = find_or_create(
		g_wal_logs,
		zWalName,
		sizeof(struct wal_log),
		strlen(zWalName) + 1,
		(void **)&curr_wal_log,
		initialize_wal_log_entry,
		(void *)&temp_wal_log
	);

	if (rc != SQLITE_OK) goto error_out;

	wal->read_mark = -1;
	wal->p_wal_log = curr_wal_log;
	*ppWal = wal;
	return SQLITE_OK;


error_out:
	return rc;

}

int sqlite3WalClose(Wal *pWal, sqlite3*, int sync_flags, int, u8 *){
	return SQLITE_OK;
}

void sqlite3WalLimit(Wal*, i64){
	FFLUSH_UNEXPECTED()
}

int sqlite3WalBeginReadTransaction(Wal *pWal, int *){
	if (pWal->read_mark >= 0){
		FFLUSH_UNEXPECTED()
		return SQLITE_ERROR;
	}
	pWal->read_mark = pWal->p_wal_log->page_count;
	return SQLITE_OK;
}

void sqlite3WalEndReadTransaction(Wal *pWal){
	if (pWal >= 0) pWal->read_mark = -1;
}


void iterate(
	PgHdr *head, 
	void *context,
	int comp(PgHdr *, int, const void*), 
	PgHdr **out_node, 
	int *out_index
){
	int index = 0;
	int found_index = 0;

	PgHdr *node = head;
	while (node != NULL){
		index++;
		if (comp(node, index, context)){
			found_index = index;
			break;
		}
		node = node->pDirtyNext;
	}
	if (out_index) *out_index = found_index;
	if (out_node) *out_node = node;
}

static int find_pg_comp(PgHdr *node, int, const void *p_pgno){
	Pgno pgno = *(Pgno *)p_pgno;
	return pgno == node->pgno;
}

static int find_frame_comp(PgHdr *, int index, const void *context){
	u32 index_to_check = *(u32 *)context;
	return index == index_to_check;
}

int sqlite3WalFindFrame(Wal * wal, Pgno pgno, u32 * p_out){
	struct wal_log *p_wal_log = wal->p_wal_log;
	iterate(p_wal_log->log, &pgno, find_pg_comp, NULL, p_out);
	return SQLITE_OK;
}

int sqlite3WalReadFrame(
	Wal *wal, 
	u32 i_read, 
	int n_out, 
	u8 *p_out
){
	struct wal_log *p_wal_log = wal->p_wal_log;
	PgHdr *page_head_at;
	iterate(p_wal_log->log, &i_read, find_frame_comp, &page_head_at, NULL);
	if (page_head_at == NULL){
		FFLUSH_UNEXPECTED()
		return SQLITE_OK;
	}
	memcpy(p_out, page_head_at->pData, n_out > p_wal_log->page_size ? p_wal_log->page_size : n_out);
	// Technically, there should be some kind of short read error. But, I don't think
	// it is relevant here.
	return SQLITE_OK;
}

Pgno sqlite3WalDbsize(Wal *wal){
	if (wal){
		return wal->p_wal_log->db_size;
	}
	return 0;
}

int sqlite3WalBeginWriteTransaction(Wal *pWal){
	return SQLITE_OK;
}

int sqlite3WalEndWriteTransaction(Wal *pWal){
	return SQLITE_OK;
}

int sqlite3WalUndo(Wal *pWal, int (*xUndo)(void *, Pgno), void *pUndoCtx, int){
	// I don't think we'd need this, currently.
	// That's because we don't do any spills.
	return SQLITE_OK;
}

void sqlite3WalSavepoint(Wal *pWal, u32 *aWalData){
	// Not needed, currently, savepoints aren't leaked
	// to WAL.
	return;
}

int sqlite3WalSavepointUndo(Wal *pWal, u32 *aWalData){
	// Not needed, currently, savepoints aren't leaked
	// to WAL.
	return SQLITE_OK;
}

int clone_pghdr(PgHdr *old, PgHdr **new, size_t page_size){
	// Both of them are allocated together.
	PgHdr *created = malloc(sizeof(PgHdr) + page_size);
	if (!created) return SQLITE_NOMEM_BKPT;
	// Don't waste time cleaning out page space.
	memset(created, 0, sizeof(PgHdr));
	created->pData = (u8*)&created[1];
	*new = created;

	if (old){
		created->pgno = old->pgno;
		memcpy(created->pData, old->pData, page_size);
	}
	
	return SQLITE_OK;
}

int sqlite3WalFrames(
	Wal *wal,
	int page_size,
	PgHdr *p_list,
	Pgno n_truncate,
	int is_commit,
	int sync_flags
){
	// This is going to be only called in the context of a commit.
	PgHdr *p;
	PgHdr *wal_head = wal->p_wal_log->log;
	int num_entries = 0;
	// The idea is to create a "reversed" list, and copy all the pages first.
	// This way, if a memory allocation error occurs, we can rollback more cleanly.
	PgHdr *new_head = NULL;
	int rc = SQLITE_OK;
	PgHdr *tail = NULL;
	int frame_count = 0;
	for (p = p_list; p; (p = p->pDirtyNext)){
		PgHdr *new_header;
		if ((clone_pghdr(p, &new_header, page_size)) != SQLITE_OK){
			// We'd actually need to free up previously allocated headers too.
			goto error_out;
		}
		// Set the tail. This is needed to append the WAL.
		if (!tail) tail = new_header;
		new_header->pDirtyNext = new_head;
		new_head = new_header;
		frame_count++;
	}
	tail->pDirtyNext = wal_head;
	wal->p_wal_log->log = new_head;
	wal->p_wal_log->db_size = n_truncate;
	wal->p_wal_log->page_count += frame_count;
	wal->iCallback = wal->p_wal_log->page_count;
	return SQLITE_OK;

error_out:
	PgHdr *freed_headers = new_head;
	while (freed_headers){
		// Get the next header, before freeing. Otherwise, we'd be referencing
		// unmapped memory.
		PgHdr *next = freed_headers->pDirtyNext;
		free(freed_headers);
		freed_headers = next;
	}
	return rc;
}

int sqlite3WalCheckpoint(
  Wal *pWal,                      /* Write-ahead log connection */
  sqlite3 *db,                    /* Check this handle's interrupt flag */
  int eMode,                      /* One of PASSIVE, FULL and RESTART */
  int (*xBusy)(void*),            /* Function to call when busy */
  void *pBusyArg,                 /* Context argument for xBusyHandler */
  int sync_flags,                 /* Flags to sync db file with (or 0) */
  int nBuf,                       /* Size of buffer nBuf */
  u8 *zBuf,                       /* Temporary buffer to use */
  int *pnLog,                     /* OUT: Number of frames in WAL */
  int *pnCkpt                     /* OUT: Number of backfilled frames in WAL */
) {
	FFLUSH_UNEXPECTED()
	return SQLITE_OK;
}

int sqlite3WalCallback(Wal *pWal){
	u32 ret = 0;
	if ( pWal ){
		ret = pWal->iCallback;
		pWal->iCallback = 0;
	}
	return (int)ret;
}

int sqlite3WalExclusiveMode(Wal *pWal, int op){
	FFLUSH_UNEXPECTED()
	return SQLITE_OK;
}

int sqlite3WalHeapMemory(Wal *pWal){
	FFLUSH_UNEXPECTED()
	return 1;
}

int sqlite3WalLockForCommit(Wal *pWal, PgHdr *pPg, Bitvec *pRead, Pgno*){
	FFLUSH_UNEXPECTED()
	return SQLITE_OK;
}

int sqlite3WalUpgradeSnapshot(Wal *pWal){
	FFLUSH_UNEXPECTED()
	return SQLITE_OK;
}

sqlite3_file *sqlite3WalFile(Wal *pWal){
	FFLUSH_UNEXPECTED()
	return NULL;
}

int sqlite3WalInfo(Wal *pWal, u32 *pnPrior, u32 *pnFrame){
	FFLUSH_UNEXPECTED()
	return 0;
}

// #define NICE_PRINT(X) printf("Return code: %d. Line: %d. File: %s\n", X, __LINE__, __FILE__)

// void nice_print(int rc){
// 	printf("Return code: %d\n", rc);
// }

// int main(){
// 	Wal *wal1 = NULL, *wal2 = NULL;
// 	int rc = sqlite3ModWalOpen(
// 		&wal1,
// 		"test",
// 		1024,
// 		5
// 	);
// 	printf("WAL 1: %p\n", wal1);
// 	rc = sqlite3ModWalOpen(
// 		&wal2,
// 		"test",
// 		1024,
// 		5
// 	);
// 	printf("WAL 2: %p\n", wal2);
// 	NICE_PRINT(rc);

// 	PgHdr *page_to_add_1;
// 	rc = clone_pghdr(NULL, &page_to_add_1, wal1->p_wal_log->page_size);
// 	page_to_add_1->pgno = 7;

// 	PgHdr *page_to_add_2;
// 	rc = clone_pghdr(NULL, &page_to_add_2, wal1->p_wal_log->page_size);
// 	page_to_add_2->pgno = 5;

// 	page_to_add_1->pDirtyNext = page_to_add_2;

// 	NICE_PRINT(rc);
// 	rc = sqlite3WalFrames(wal1, 1024, page_to_add_1, 2, 1, 1);
	
// 	NICE_PRINT(rc);
// 	int frame_out;
// 	rc = sqlite3WalFindFrame(wal1, 7, &frame_out);

// 	NICE_PRINT(rc);
// 	printf("Found page, at index: %d\n", frame_out);
	
// 	return 0;
// }