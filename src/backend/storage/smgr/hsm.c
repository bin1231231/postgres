/*-------------------------------------------------------------------------
 *
 * hsm.c
 *	  HSM (Halo-style Storage Manager)：把关系数据直接存放在块设备上的存储引擎。
 *
 *	  md.c 把每个关系 fork 存成文件系统里的一个（或多个 1GB 段）文件，空间管理
 *	  交给文件系统。HSM 绕过文件系统，自己在一块裸设备（如 /dev/loop0）上做
 *	  空间管理，核心就是课上讲的“空间元数据的管理与检索”：
 *
 *	  设备布局（块大小 = BLCKSZ = 8KB）：
 *
 *	    块 0                 超级块：魔数、版本、设备大小、文件表/数据区的位置
 *	    块 1 .. N            文件表：每个关系 fork 一条 512 字节的记录
 *	                         （相当于文件系统的 inode / 文件分配表），
 *	                         记录 fork 的身份、逻辑长度和它拥有的区列表
 *	    块 data_start ..     数据区：按“区”分配，区由若干 64KB 的分配单元组成
 *
 *	  - 以“区（extent）”为空间分配的基本单位，以“块”为空间使用的基本单位；
 *	  - 区的大小随文件增长而变大（64KB -> 512KB -> 4MB -> 32MB -> 64MB），
 *	    即“文件越大、区越大”，大文件的元数据依然很小；
 *	  - 文件表记录恰好 512 字节 = 1 个扇区，单条记录的写入是原子的；
 *	  - 空闲空间位图不落盘，启动时由文件表重建，避免两份元数据不一致。
 *
 *	  内存中的元数据检索：
 *	  - 共享内存哈希表 (RelFileNode, fork) -> HsmFile，相当于元数据的 1 级缓存；
 *	  - HsmFile 中按逻辑块号有序保存区列表，块号 -> 设备块号 只需一次查找。
 *
 *	  崩溃一致性：
 *	  - 数据块写入后向 checkpointer 注册一个“整个设备”的同步请求，
 *	    checkpoint 时 fsync 设备一次（与 md.c 按段 fsync 等价）；
 *	  - 元数据记录先写到页缓存，与数据一起在 checkpoint 时持久化；checkpoint
 *	    之后的变化由 WAL 重放恢复（与 md.c 依赖文件大小的方式相同）；
 *	  - 释放的区先进入 pending 状态，直到下一次设备 fsync 成功后才能被复用，
 *	    这样崩溃后不会出现两个文件同时声称拥有同一个区的情况；
 *	  - 删除 main fork 时与 md.c 一样留下“墓碑”，到下一个 checkpoint 之后才
 *	    真正删除，防止 relfilenode 过早被复用（见 mdunlink() 的注释）。
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/hsm.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "access/transam.h"
#include "access/xlogutils.h"
#include "catalog/pg_tablespace_d.h"
#include "common/relpath.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "port/pg_crc32c.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/hsm.h"
#include "storage/lwlock.h"
#include "storage/reinit.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/tuplestore.h"


/* ----------------------------------------------------------------
 *		磁盘格式
 * ----------------------------------------------------------------
 */
#define HSM_SB_MAGIC			0x314D5348	/* "HSM1" */
#define HSM_REC_MAGIC			0x43455248	/* "HREC" */
#define HSM_FORMAT_VERSION		1

#define HSM_UNIT_BLOCKS			8	/* 分配单元：8 块 = 64KB */
#define HSM_RECORD_SIZE			512 /* 一条文件表记录 = 一个扇区 */
#define HSM_RECORDS_PER_BLOCK	(BLCKSZ / HSM_RECORD_SIZE)
#define HSM_MAX_EXTENTS			60	/* 每个 fork 最多 60 个区 */

#define HSM_FLAG_TOMBSTONE		0x0001	/* main fork 已删除，等待 checkpoint */

/* 超级块，位于设备的第 0 块 */
typedef struct HsmSuperBlock
{
	uint32		magic;
	uint32		version;
	uint32		blcksz;
	uint32		unit_blocks;
	uint32		record_size;
	uint32		max_files;		/* 文件表槽位数 */
	uint32		filetab_start;	/* 文件表起始块 */
	uint32		filetab_blocks; /* 文件表占用的块数 */
	uint32		data_start;		/* 数据区起始块（按分配单元对齐） */
	uint32		nunits;			/* 数据区中的分配单元数 */
	uint64		device_blocks;	/* 格式化时的设备块数 */
	int64		format_time;
	pg_crc32c	crc;			/* 以上字段的 CRC */
} HsmSuperBlock;

/* 磁盘上的区描述：设备起始块 + 块数 */
typedef struct HsmDiskExtent
{
	uint32		start;
	uint32		nblocks;
} HsmDiskExtent;

/* 文件表中的一条记录，恰好 512 字节 */
typedef struct HsmDiskRecord
{
	uint32		magic;			/* HSM_REC_MAGIC 表示在用，0 表示空槽 */
	uint16		flags;
	int16		forknum;
	RelFileNode rnode;
	uint32		nblocks;		/* 逻辑长度（块） */
	uint16		nextents;
	uint16		reserved;
	pg_crc32c	crc;			/* 整条记录（crc 字段置 0）的 CRC */
	HsmDiskExtent extents[HSM_MAX_EXTENTS];
} HsmDiskRecord;

StaticAssertDecl(sizeof(HsmDiskRecord) == HSM_RECORD_SIZE,
				 "HsmDiskRecord must be exactly one sector");
StaticAssertDecl(sizeof(HsmSuperBlock) <= BLCKSZ,
				 "HsmSuperBlock must fit in one block");


/* ----------------------------------------------------------------
 *		共享内存中的结构
 * ----------------------------------------------------------------
 */
typedef struct HsmFileKey
{
	RelFileNode rnode;
	int32		forknum;
} HsmFileKey;

/* 内存中的区：多记一个逻辑起始块号，便于块号映射 */
typedef struct HsmExtent
{
	uint32		start;			/* 设备块号 */
	uint32		nblocks;
	uint32		lstart;			/* 该区第一个块在 fork 中的逻辑块号 */
} HsmExtent;

/* 一个关系 fork 的元数据（哈希表的条目） */
typedef struct HsmFile
{
	HsmFileKey	key;			/* 哈希键，必须在最前 */
	int32		slot;			/* 文件表中的槽位号 */
	uint32		nblocks;		/* 逻辑长度 */
	uint32		capacity;		/* 已分配的块数 = 所有区块数之和 */
	uint16		nextents;
	uint16		flags;
	HsmExtent	extents[HSM_MAX_EXTENTS];
} HsmFile;

typedef struct HsmControl
{
	HsmSuperBlock sb;
	uint32		max_files;
	uint32		nunits;
	uint32		free_units;		/* 可立即分配的单元数 */
	uint32		pending_units;	/* 已释放、等待下一次 fsync 的单元数 */
	uint32		releasing_units;	/* fsync 进行中的单元数 */
	uint32		used_slots;
	uint32		unit_hint;		/* 下次分配从这里开始找（next-fit） */
	uint32		slot_hint;
	pg_atomic_uint64 nreads;
	pg_atomic_uint64 nwrites;
	pg_atomic_uint64 nextends;
	pg_atomic_uint64 nallocs;
	pg_atomic_uint64 nfrees;
	pg_atomic_uint64 nsyncs;
	/* 之后依次是：unit_used、unit_pending、unit_releasing、slot_used 四个位图 */
} HsmControl;


/* GUC 变量 */
char	   *hsm_device = NULL;
int			hsm_max_files = 4096;

/* 指向共享内存 */
static HsmControl *HsmCtl = NULL;
static HTAB *HsmHash = NULL;
static uint64 *hsm_unit_used = NULL;	/* 已分配（含 pending）的单元 */
static uint64 *hsm_unit_pending = NULL; /* 已释放但未持久化 */
static uint64 *hsm_unit_releasing = NULL;	/* 正在随 fsync 持久化 */
static uint64 *hsm_slot_used = NULL;

/* 本进程打开的设备 fd（每个后端只需要一个） */
static int	hsm_fd = -1;

/* hsm_probe() 的结果：磁盘上的（或即将写入的）超级块 */
static bool hsm_layout_valid = false;
static HsmSuperBlock hsm_layout;

static const char zero_record[HSM_RECORD_SIZE];


/* ----------------------------------------------------------------
 *		小工具
 * ----------------------------------------------------------------
 */
#define BM_WORDS(nbits)		(((nbits) + 63) / 64)

static inline bool
bm_test(const uint64 *bm, uint32 i)
{
	return (bm[i >> 6] >> (i & 63)) & 1;
}

static inline void
bm_set(uint64 *bm, uint32 i)
{
	bm[i >> 6] |= UINT64CONST(1) << (i & 63);
}

static inline void
bm_clear(uint64 *bm, uint32 i)
{
	bm[i >> 6] &= ~(UINT64CONST(1) << (i & 63));
}

static inline bool
hsm_guc_enabled(void)
{
	return hsm_device != NULL && hsm_device[0] != '\0';
}

/*
 * 区大小的增长策略：文件越大、区越大。
 * 前 8 个区 64KB，之后 512KB、4MB、32MB，最后 64MB。
 * 60 个区最多可容纳约 1.6GB。
 */
static uint32
hsm_extent_target_blocks(int idx)
{
	if (idx < 8)
		return 8;
	if (idx < 16)
		return 64;
	if (idx < 32)
		return 512;
	if (idx < 40)
		return 4096;
	return 8192;
}

static void
hsm_sb_set_crc(HsmSuperBlock *sb)
{
	INIT_CRC32C(sb->crc);
	COMP_CRC32C(sb->crc, sb, offsetof(HsmSuperBlock, crc));
	FIN_CRC32C(sb->crc);
}

static bool
hsm_sb_crc_ok(const HsmSuperBlock *sb)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, sb, offsetof(HsmSuperBlock, crc));
	FIN_CRC32C(crc);
	return EQ_CRC32C(crc, sb->crc);
}

static pg_crc32c
hsm_record_crc(const HsmDiskRecord *rec)
{
	HsmDiskRecord tmp;
	pg_crc32c	crc;

	memcpy(&tmp, rec, sizeof(tmp));
	tmp.crc = 0;
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, &tmp, sizeof(tmp));
	FIN_CRC32C(crc);
	return crc;
}

/* 读满 len 字节；返回实际读到的字节数，出错返回 -1 */
static ssize_t
hsm_pread_full(int fd, void *buf, size_t len, off_t off)
{
	size_t		done = 0;

	while (done < len)
	{
		ssize_t		r = pg_pread(fd, (char *) buf + done, len - done, off + done);

		if (r < 0)
		{
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			break;
		done += r;
	}
	return done;
}

/* 写满 len 字节；返回实际写入的字节数，出错返回 -1 */
static ssize_t
hsm_pwrite_full(int fd, const void *buf, size_t len, off_t off)
{
	size_t		done = 0;

	while (done < len)
	{
		ssize_t		r = pg_pwrite(fd, (const char *) buf + done, len - done, off + done);

		if (r < 0)
		{
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
		{
			errno = ENOSPC;
			break;
		}
		done += r;
	}
	return done;
}

/* 打开（并缓存）本进程的设备 fd；失败返回 -1，errno 有效 */
static int
hsm_get_fd(void)
{
	if (hsm_fd < 0)
	{
		int			fd = BasicOpenFile(hsm_device, O_RDWR | PG_BINARY);

		if (fd < 0)
			return -1;
		/* 让 fd.c 知道我们长期占用了一个 fd */
		ReserveExternalFD();
		hsm_fd = fd;
	}
	return hsm_fd;
}

static int
hsm_fd_or_error(void)
{
	int			fd = hsm_get_fd();

	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open HSM device \"%s\": %m", hsm_device)));
	return fd;
}

static inline HsmFile *
hsm_lookup(RelFileNode rnode, ForkNumber forknum)
{
	HsmFileKey	key;

	memset(&key, 0, sizeof(key));
	key.rnode = rnode;
	key.forknum = forknum;
	return (HsmFile *) hash_search(HsmHash, &key, HASH_FIND, NULL);
}

/*
 * 逻辑块号 -> 设备块号。*run 返回从该块开始、在同一个区内连续的块数。
 * 调用者需持有 HsmLock，并保证 blkno < f->capacity。
 */
static BlockNumber
hsm_map_block(const HsmFile *f, BlockNumber blkno, uint32 *run)
{
	int			lo = 0,
				hi = f->nextents - 1;

	/* 区按 lstart 有序，二分查找 */
	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;
		const HsmExtent *e = &f->extents[mid];

		if (blkno < e->lstart)
			hi = mid - 1;
		else if (blkno >= e->lstart + e->nblocks)
			lo = mid + 1;
		else
		{
			if (run)
				*run = e->lstart + e->nblocks - blkno;
			return e->start + (blkno - e->lstart);
		}
	}
	elog(ERROR, "HSM: block %u of relation %u/%u/%u fork %d is not allocated",
		 blkno, f->key.rnode.spcNode, f->key.rnode.dbNode,
		 f->key.rnode.relNode, f->key.forknum);
	return InvalidBlockNumber;	/* keep compiler quiet */
}

static void
hsm_record_from_file(const HsmFile *f, HsmDiskRecord *rec)
{
	memset(rec, 0, sizeof(*rec));
	rec->magic = HSM_REC_MAGIC;
	rec->flags = f->flags;
	rec->forknum = (int16) f->key.forknum;
	rec->rnode = f->key.rnode;
	rec->nblocks = f->nblocks;
	rec->nextents = f->nextents;
	for (int i = 0; i < f->nextents; i++)
	{
		rec->extents[i].start = f->extents[i].start;
		rec->extents[i].nblocks = f->extents[i].nblocks;
	}
	rec->crc = hsm_record_crc(rec);
}

/* 把一条记录写到文件表的 slot 处。只写入页缓存，持久化随设备 fsync。 */
static bool
hsm_write_slot(int slot, const void *rec, int elevel)
{
	off_t		off = (off_t) HsmCtl->sb.filetab_start * BLCKSZ +
		(off_t) slot * HSM_RECORD_SIZE;
	int			fd = hsm_get_fd();

	if (fd < 0 || hsm_pwrite_full(fd, rec, HSM_RECORD_SIZE, off) != HSM_RECORD_SIZE)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not write HSM metadata record %d on device \"%s\": %m",
						slot, hsm_device)));
		return false;
	}
	return true;
}

static bool
hsm_write_record(const HsmFile *f, int elevel)
{
	HsmDiskRecord rec;

	hsm_record_from_file(f, &rec);
	return hsm_write_slot(f->slot, &rec, elevel);
}


/* ----------------------------------------------------------------
 *		空间分配（调用者持有 HsmLock 排他锁）
 * ----------------------------------------------------------------
 */
static int
hsm_alloc_slot(void)
{
	uint32		n = HsmCtl->max_files;

	for (uint32 k = 0; k < n; k++)
	{
		uint32		s = (HsmCtl->slot_hint + k) % n;

		if (!bm_test(hsm_slot_used, s))
		{
			bm_set(hsm_slot_used, s);
			HsmCtl->used_slots++;
			HsmCtl->slot_hint = (s + 1) % n;
			return (int) s;
		}
	}
	return -1;
}

static void
hsm_free_slot(int slot)
{
	Assert(bm_test(hsm_slot_used, slot));
	bm_clear(hsm_slot_used, slot);
	HsmCtl->used_slots--;
}

/*
 * 在单元位图中找 want 个连续空闲单元（next-fit：从上次分配的位置往后找，
 * 绕回到开头）。返回起始单元号，找不到返回 -1。
 */
static int64
hsm_find_free_run(uint32 want)
{
	uint32		n = HsmCtl->nunits;
	uint32		i = HsmCtl->unit_hint % n;
	uint64		scanned = 0;
	uint32		runlen = 0;
	uint32		runstart = 0;

	while (scanned < (uint64) n + want)
	{
		if (i >= n)
		{
			i = 0;
			runlen = 0;			/* 物理上不连续，重新计数 */
		}
		/* 整个 64 位字都已占用：一次跳过 */
		if (runlen == 0 && (i & 63) == 0 && i + 64 <= n &&
			hsm_unit_used[i >> 6] == ~UINT64CONST(0))
		{
			i += 64;
			scanned += 64;
			continue;
		}
		if (bm_test(hsm_unit_used, i))
			runlen = 0;
		else
		{
			if (runlen == 0)
				runstart = i;
			if (++runlen == want)
				return runstart;
		}
		i++;
		scanned++;
	}
	return -1;
}

/*
 * 为 f 追加一个区。区大小按 hsm_extent_target_blocks()，若找不到足够大的
 * 连续空间则逐次减半，最小 1 个分配单元。空间不足返回 false。
 */
static bool
hsm_add_extent(HsmFile *f)
{
	int			idx = f->nextents;
	uint32		want;

	if (idx >= HSM_MAX_EXTENTS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("HSM relation %s cannot have more than %d extents (%u blocks)",
						relpathperm(f->key.rnode, f->key.forknum),
						HSM_MAX_EXTENTS, f->capacity)));

	want = hsm_extent_target_blocks(idx) / HSM_UNIT_BLOCKS;
	/* 小设备上避免一个区就占掉大半空间 */
	want = Min(want, Max(1, HsmCtl->nunits / 8));

	for (uint32 units = want; units > 0; units >>= 1)
	{
		int64		su;
		HsmExtent  *e;

		if (units > HsmCtl->free_units)
			continue;
		su = hsm_find_free_run(units);
		if (su < 0)
			continue;

		for (uint32 u = 0; u < units; u++)
			bm_set(hsm_unit_used, (uint32) su + u);
		HsmCtl->free_units -= units;
		HsmCtl->unit_hint = (uint32) su + units;

		e = &f->extents[idx];
		e->start = HsmCtl->sb.data_start + (uint32) su * HSM_UNIT_BLOCKS;
		e->nblocks = units * HSM_UNIT_BLOCKS;
		e->lstart = f->capacity;
		f->capacity += e->nblocks;
		f->nextents++;
		pg_atomic_fetch_add_u64(&HsmCtl->nallocs, 1);

		elog(DEBUG2, "HSM: extent %d of %u/%u/%u fork %d: %u blocks at device block %u",
			 idx, f->key.rnode.spcNode, f->key.rnode.dbNode,
			 f->key.rnode.relNode, f->key.forknum, e->nblocks, e->start);
		return true;
	}
	return false;
}

/*
 * 释放一个区占用的单元。为保证崩溃一致性，单元先进入 pending 状态，
 * 等下一次设备 fsync 成功（保证“释放”这件事已落盘）后才能复用。
 * fsync=off 时没有持久化可言，直接释放。
 */
static void
hsm_release_extent(const HsmExtent *e)
{
	uint32		u0 = (e->start - HsmCtl->sb.data_start) / HSM_UNIT_BLOCKS;
	uint32		n = e->nblocks / HSM_UNIT_BLOCKS;

	for (uint32 u = u0; u < u0 + n; u++)
	{
		Assert(bm_test(hsm_unit_used, u));
		if (enableFsync)
			bm_set(hsm_unit_pending, u);
		else
			bm_clear(hsm_unit_used, u);
	}
	if (enableFsync)
		HsmCtl->pending_units += n;
	else
		HsmCtl->free_units += n;
	pg_atomic_fetch_add_u64(&HsmCtl->nfrees, 1);
}

/*
 * fsync 整个设备，然后把 fsync 之前已释放的单元归还给空闲位图。
 * 成功返回 0；失败返回 -1，errno 有效。
 */
static int
hsm_sync_device(void)
{
	int			fd;
	int			rc;
	uint32		nw;

	fd = hsm_get_fd();
	if (fd < 0)
		return -1;

	nw = BM_WORDS(HsmCtl->nunits);

	/* 同一时刻只允许一个进程执行这个流程 */
	LWLockAcquire(HsmSyncLock, LW_EXCLUSIVE);

	LWLockAcquire(HsmLock, LW_EXCLUSIVE);
	for (uint32 w = 0; w < nw; w++)
	{
		hsm_unit_releasing[w] |= hsm_unit_pending[w];
		hsm_unit_pending[w] = 0;
	}
	HsmCtl->releasing_units += HsmCtl->pending_units;
	HsmCtl->pending_units = 0;
	LWLockRelease(HsmLock);

	pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_SYNC);
	rc = pg_fsync(fd);
	pgstat_report_wait_end();

	if (rc == 0)
	{
		pg_atomic_fetch_add_u64(&HsmCtl->nsyncs, 1);
		LWLockAcquire(HsmLock, LW_EXCLUSIVE);
		for (uint32 w = 0; w < nw; w++)
		{
			hsm_unit_used[w] &= ~hsm_unit_releasing[w];
			hsm_unit_releasing[w] = 0;
		}
		HsmCtl->free_units += HsmCtl->releasing_units;
		HsmCtl->releasing_units = 0;
		LWLockRelease(HsmLock);
	}
	else
	{
		/* releasing 中的单元留到下一次成功的 fsync 再归还 */
		int			save_errno = errno;

		LWLockRelease(HsmSyncLock);
		errno = save_errno;
		return -1;
	}

	LWLockRelease(HsmSyncLock);
	return 0;
}

static void
hsm_device_filetag(FileTag *tag)
{
	memset(tag, 0, sizeof(*tag));
	tag->handler = SYNC_HANDLER_HSM;
	tag->forknum = InvalidForkNumber;
}

/*
 * 告诉 checkpointer：设备上有尚未持久化的写入，下个 checkpoint 要 fsync。
 * 所有 fork 共享同一个 tag，checkpointer 会自动去重，所以每次 checkpoint
 * 只 fsync 设备一次。
 */
static void
hsm_register_dirty(void)
{
	FileTag		tag;

	hsm_device_filetag(&tag);
	if (!RegisterSyncRequest(&tag, SYNC_REQUEST, false /* retryOnError */ ))
	{
		ereport(DEBUG1,
				(errmsg_internal("could not forward fsync request because request queue is full")));
		if (hsm_sync_device() != 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync HSM device \"%s\": %m", hsm_device)));
	}
}


/* ----------------------------------------------------------------
 *		启动：探测设备、格式化、从文件表重建内存元数据
 * ----------------------------------------------------------------
 */
static void
hsm_compute_layout(uint64 device_blocks, uint32 max_files, HsmSuperBlock *sb)
{
	uint64		cap_files;

	memset(sb, 0, sizeof(*sb));
	if (device_blocks > (uint64) MaxBlockNumber)
		device_blocks = MaxBlockNumber;

	/* 文件表最多占设备的 1/8，保证很小的设备（如 1MB）也能用 */
	cap_files = (device_blocks / 8) * HSM_RECORDS_PER_BLOCK;
	if ((uint64) max_files > cap_files)
		max_files = (uint32) cap_files;

	sb->magic = HSM_SB_MAGIC;
	sb->version = HSM_FORMAT_VERSION;
	sb->blcksz = BLCKSZ;
	sb->unit_blocks = HSM_UNIT_BLOCKS;
	sb->record_size = HSM_RECORD_SIZE;
	sb->max_files = max_files;
	sb->filetab_start = 1;
	sb->filetab_blocks = (max_files + HSM_RECORDS_PER_BLOCK - 1) / HSM_RECORDS_PER_BLOCK;
	sb->data_start = ((1 + sb->filetab_blocks + HSM_UNIT_BLOCKS - 1) / HSM_UNIT_BLOCKS) * HSM_UNIT_BLOCKS;
	sb->nunits = (device_blocks > sb->data_start) ?
		(uint32) ((device_blocks - sb->data_start) / HSM_UNIT_BLOCKS) : 0;
	sb->device_blocks = device_blocks;
}

static bool
hsm_block_is_zero(const char *buf)
{
	for (int i = 0; i < BLCKSZ; i++)
		if (buf[i] != 0)
			return false;
	return true;
}

/*
 * 读取设备的超级块，确定布局（共享内存的大小依赖于它）。
 * 在 postmaster 中计算共享内存大小时调用；结果缓存在 hsm_layout 中。
 */
static void
hsm_probe(void)
{
	PGAlignedBlock buf;
	int			fd;
	off_t		size;
	ssize_t		nread;

	if (hsm_layout_valid)
		return;

	fd = open(hsm_device, O_RDWR | PG_BINARY, 0);
	if (fd < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open HSM device \"%s\": %m", hsm_device),
				 errhint("Set hsm_device to a block device (or file) writable by the server, or to '' to disable HSM.")));

	size = lseek(fd, 0, SEEK_END);
	if (size < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not determine size of HSM device \"%s\": %m", hsm_device)));

	nread = hsm_pread_full(fd, buf.data, BLCKSZ, 0);
	if (nread < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not read HSM device \"%s\": %m", hsm_device)));

	if (nread < BLCKSZ || hsm_block_is_zero(buf.data))
	{
		/* 全零：新设备，按 GUC 计算布局，稍后格式化 */
		hsm_compute_layout((uint64) size / BLCKSZ, (uint32) hsm_max_files, &hsm_layout);
	}
	else
	{
		HsmSuperBlock *sb = (HsmSuperBlock *) buf.data;

		if (sb->magic != HSM_SB_MAGIC)
			ereport(FATAL,
					(errmsg("device \"%s\" is not an HSM device (bad magic number %08X)",
							hsm_device, sb->magic),
					 errhint("HSM only formats devices whose first block is all zeroes; "
							 "wipe it with \"dd if=/dev/zero of=%s bs=8192 count=1\" if you really want to use it.",
							 hsm_device)));
		if (!hsm_sb_crc_ok(sb))
			ereport(FATAL,
					(errmsg("HSM superblock on \"%s\" is corrupted (CRC mismatch)", hsm_device)));
		if (sb->version != HSM_FORMAT_VERSION || sb->blcksz != BLCKSZ ||
			sb->unit_blocks != HSM_UNIT_BLOCKS || sb->record_size != HSM_RECORD_SIZE)
			ereport(FATAL,
					(errmsg("HSM device \"%s\" was formatted with an incompatible layout "
							"(version %u, block size %u)", hsm_device, sb->version, sb->blcksz)));
		if ((uint64) size / BLCKSZ < sb->data_start + (uint64) sb->nunits * HSM_UNIT_BLOCKS)
			ereport(FATAL,
					(errmsg("HSM device \"%s\" is smaller than when it was formatted", hsm_device)));
		memcpy(&hsm_layout, sb, sizeof(HsmSuperBlock));
		if (hsm_layout.max_files != (uint32) hsm_max_files)
			ereport(LOG,
					(errmsg("hsm_max_files = %d ignored: device \"%s\" was formatted with %u file slots",
							hsm_max_files, hsm_device, hsm_layout.max_files)));
	}

	close(fd);

	if (hsm_layout.max_files == 0 || hsm_layout.nunits == 0)
		ereport(FATAL,
				(errmsg("HSM device \"%s\" is too small (%lld bytes)",
						hsm_device, (long long) size)));

	hsm_layout_valid = true;
}

static void
hsm_format_device(int fd, HsmSuperBlock *sb)
{
	PGAlignedBlock buf;

	sb->format_time = (int64) time(NULL);
	hsm_sb_set_crc(sb);

	/* 先清零文件表并落盘，再写超级块：格式化一半的设备不会被当成有效设备 */
	memset(buf.data, 0, BLCKSZ);
	for (uint32 b = 0; b < sb->filetab_blocks; b++)
	{
		if (hsm_pwrite_full(fd, buf.data, BLCKSZ,
							(off_t) (sb->filetab_start + b) * BLCKSZ) != BLCKSZ)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not format HSM device \"%s\": %m", hsm_device)));
	}
	if (pg_fsync(fd) != 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not fsync HSM device \"%s\": %m", hsm_device)));

	memcpy(buf.data, sb, sizeof(HsmSuperBlock));
	if (hsm_pwrite_full(fd, buf.data, BLCKSZ, 0) != BLCKSZ ||
		pg_fsync(fd) != 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not write HSM superblock on \"%s\": %m", hsm_device)));

	ereport(LOG,
			(errmsg("formatted HSM device \"%s\": %llu blocks, %u file slots, %u extent units of %d kB",
					hsm_device, (unsigned long long) sb->device_blocks,
					sb->max_files, sb->nunits, HSM_UNIT_BLOCKS * BLCKSZ / 1024)));
}

/*
 * 把磁盘上的文件表读入共享内存，并重建空闲空间位图。
 * 在 postmaster 中创建共享内存时调用（包括崩溃后的重新初始化）。
 */
static void
hsm_load_metadata(void)
{
	PGAlignedBlock buf;
	HsmSuperBlock *sb = &HsmCtl->sb;
	uint32		used_units = 0;
	int			fd;

	fd = open(hsm_device, O_RDWR | PG_BINARY, 0);
	if (fd < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open HSM device \"%s\": %m", hsm_device)));

	/* 重新读超级块：只有第 0 块全零时才格式化，绝不覆盖已有数据 */
	if (hsm_pread_full(fd, buf.data, BLCKSZ, 0) != BLCKSZ)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not read HSM superblock on \"%s\": %m", hsm_device)));
	if (hsm_block_is_zero(buf.data))
		hsm_format_device(fd, &hsm_layout);
	else if (((HsmSuperBlock *) buf.data)->magic != HSM_SB_MAGIC ||
			 !hsm_sb_crc_ok((HsmSuperBlock *) buf.data))
		ereport(FATAL,
				(errmsg("HSM superblock on \"%s\" is invalid", hsm_device)));
	else
		memcpy(&hsm_layout, buf.data, sizeof(HsmSuperBlock));

	memcpy(sb, &hsm_layout, sizeof(HsmSuperBlock));
	HsmCtl->max_files = sb->max_files;
	HsmCtl->nunits = sb->nunits;

	for (uint32 b = 0; b < sb->filetab_blocks; b++)
	{
		if (hsm_pread_full(fd, buf.data, BLCKSZ,
						   (off_t) (sb->filetab_start + b) * BLCKSZ) != BLCKSZ)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not read HSM file table on \"%s\": %m", hsm_device)));

		for (int i = 0; i < HSM_RECORDS_PER_BLOCK; i++)
		{
			uint32		slot = b * HSM_RECORDS_PER_BLOCK + i;
			HsmDiskRecord *rec = (HsmDiskRecord *) (buf.data + i * HSM_RECORD_SIZE);
			HsmFileKey	key;
			HsmFile    *f;
			bool		found;

			if (slot >= sb->max_files)
				break;
			if (rec->magic == 0)
				continue;		/* 空槽 */

			if (rec->magic != HSM_REC_MAGIC || rec->crc != hsm_record_crc(rec) ||
				rec->nextents > HSM_MAX_EXTENTS ||
				rec->forknum < 0 || rec->forknum > MAX_FORKNUM)
				ereport(FATAL,
						(errmsg("HSM metadata record %u on \"%s\" is corrupted", slot, hsm_device)));

			memset(&key, 0, sizeof(key));
			key.rnode = rec->rnode;
			key.forknum = rec->forknum;
			f = (HsmFile *) hash_search(HsmHash, &key, HASH_ENTER_NULL, &found);
			if (f == NULL)
				ereport(FATAL, (errmsg("out of shared memory for HSM file table")));
			if (found)
				ereport(FATAL,
						(errmsg("HSM metadata record %u on \"%s\" duplicates record %d",
								slot, hsm_device, f->slot)));

			f->slot = slot;
			f->nblocks = rec->nblocks;
			f->flags = rec->flags;
			f->nextents = rec->nextents;
			f->capacity = 0;
			for (int k = 0; k < rec->nextents; k++)
			{
				uint32		start = rec->extents[k].start;
				uint32		nblk = rec->extents[k].nblocks;
				uint32		u0;

				if (start < sb->data_start || nblk == 0 ||
					(start - sb->data_start) % HSM_UNIT_BLOCKS != 0 ||
					nblk % HSM_UNIT_BLOCKS != 0 ||
					(uint64) (start - sb->data_start) + nblk >
					(uint64) sb->nunits * HSM_UNIT_BLOCKS)
					ereport(FATAL,
							(errmsg("HSM metadata record %u on \"%s\" has an invalid extent", slot, hsm_device)));

				f->extents[k].start = start;
				f->extents[k].nblocks = nblk;
				f->extents[k].lstart = f->capacity;
				f->capacity += nblk;

				u0 = (start - sb->data_start) / HSM_UNIT_BLOCKS;
				for (uint32 u = u0; u < u0 + nblk / HSM_UNIT_BLOCKS; u++)
				{
					if (bm_test(hsm_unit_used, u))
						ereport(FATAL,
								(errmsg("HSM metadata on \"%s\" is inconsistent: extent unit %u is claimed twice",
										hsm_device, u)));
					bm_set(hsm_unit_used, u);
					used_units++;
				}
			}
			if (f->nblocks > f->capacity)
				ereport(FATAL,
						(errmsg("HSM metadata record %u on \"%s\" is longer than its extents", slot, hsm_device)));

			bm_set(hsm_slot_used, slot);
			HsmCtl->used_slots++;
		}
	}

	HsmCtl->free_units = sb->nunits - used_units;

	/*
	 * 把页缓存中的状态落盘：若这是一次后端崩溃后的重新初始化，崩溃前写入
	 * 页缓存的释放记录从此刻起也是持久的，被释放的空间可以安全复用。
	 */
	if (pg_fsync(fd) != 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not fsync HSM device \"%s\": %m", hsm_device)));
	close(fd);

	ereport(LOG,
			(errmsg("HSM storage manager using \"%s\": %u relation forks, %u of %u extent units free",
					hsm_device, HsmCtl->used_slots, HsmCtl->free_units, HsmCtl->nunits)));
}

static Size
hsm_control_size(void)
{
	Size		size = MAXALIGN(sizeof(HsmControl));
	uint32		uw = BM_WORDS(hsm_layout.nunits);
	uint32		sw = BM_WORDS(hsm_layout.max_files);

	size = add_size(size, mul_size(uw, sizeof(uint64) * 3));
	size = add_size(size, mul_size(sw, sizeof(uint64)));
	return size;
}

Size
HsmShmemSize(void)
{
	Size		size;

	if (!hsm_guc_enabled())
		return 0;
	hsm_probe();
	size = hsm_control_size();
	size = add_size(size, hash_estimate_size(hsm_layout.max_files, sizeof(HsmFile)));
	return size;
}

void
HsmShmemInit(void)
{
	HASHCTL		info;
	bool		found;
	char	   *p;
	Size		ctlsize;

	if (!hsm_guc_enabled())
	{
		HsmCtl = NULL;
		return;
	}
	hsm_probe();

	ctlsize = hsm_control_size();
	HsmCtl = (HsmControl *) ShmemInitStruct("HSM Control", ctlsize, &found);

	p = (char *) HsmCtl + MAXALIGN(sizeof(HsmControl));
	hsm_unit_used = (uint64 *) p;
	p += BM_WORDS(hsm_layout.nunits) * sizeof(uint64);
	hsm_unit_pending = (uint64 *) p;
	p += BM_WORDS(hsm_layout.nunits) * sizeof(uint64);
	hsm_unit_releasing = (uint64 *) p;
	p += BM_WORDS(hsm_layout.nunits) * sizeof(uint64);
	hsm_slot_used = (uint64 *) p;

	info.keysize = sizeof(HsmFileKey);
	info.entrysize = sizeof(HsmFile);
	HsmHash = ShmemInitHash("HSM File Table",
							hsm_layout.max_files, hsm_layout.max_files,
							&info, HASH_ELEM | HASH_BLOBS);

	if (!found)
	{
		memset(HsmCtl, 0, ctlsize);
		pg_atomic_init_u64(&HsmCtl->nreads, 0);
		pg_atomic_init_u64(&HsmCtl->nwrites, 0);
		pg_atomic_init_u64(&HsmCtl->nextends, 0);
		pg_atomic_init_u64(&HsmCtl->nallocs, 0);
		pg_atomic_init_u64(&HsmCtl->nfrees, 0);
		pg_atomic_init_u64(&HsmCtl->nsyncs, 0);
		hsm_load_metadata();
	}
}


/* ----------------------------------------------------------------
 *		路由
 * ----------------------------------------------------------------
 */

/*
 * 决定一个关系由哪个存储管理器负责（smgropen() 中调用一次，结果缓存在
 * SMgrRelation 中）。
 *
 * HSM 只接管默认表空间中、relfilenode >= FirstNormalObjectId 的永久/unlogged
 * 关系。系统表（initdb 创建）、临时表、其它表空间中的关系仍然由 md.c 负责。
 * 打开 HSM 之前就已经存在的用户表（有 md 文件）也继续由 md.c 负责。
 */
int
hsm_choose_smgr(RelFileNodeBackend rnode)
{
	char	   *path;
	bool		md_exists;

	if (HsmCtl == NULL)
		return 0;
	if (RelFileNodeBackendIsTemp(rnode))
		return 0;
	if (rnode.node.spcNode != DEFAULTTABLESPACE_OID)
		return 0;
	if (rnode.node.relNode < FirstNormalObjectId)
		return 0;

	if (hsm_relfilenode_in_use(rnode.node))
		return HSM_SMGR_ID;

	path = relpath(rnode, MAIN_FORKNUM);
	md_exists = (access(path, F_OK) == 0);
	pfree(path);

	return md_exists ? 0 : HSM_SMGR_ID;
}

/* 该 relfilenode 的任一 fork（包括墓碑）是否在 HSM 中 */
bool
hsm_relfilenode_in_use(RelFileNode rnode)
{
	bool		result = false;

	if (HsmCtl == NULL)
		return false;

	LWLockAcquire(HsmLock, LW_SHARED);
	for (ForkNumber fork = 0; fork <= MAX_FORKNUM; fork++)
	{
		if (hsm_lookup(rnode, fork) != NULL)
		{
			result = true;
			break;
		}
	}
	LWLockRelease(HsmLock);
	return result;
}


/* ----------------------------------------------------------------
 *		内部操作（以 RelFileNode 为参数，供接口函数和启动流程共用）
 * ----------------------------------------------------------------
 */

/* 在文件表中新建一个空的 fork。调用者持有 HsmLock 排他锁。 */
static HsmFile *
hsm_create_entry(RelFileNode rnode, ForkNumber forknum)
{
	HsmFile		tmp;
	HsmFile    *f;
	bool		found;
	int			slot;

	slot = hsm_alloc_slot();
	if (slot < 0)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("HSM file table on \"%s\" is full (%u relation forks)",
						hsm_device, HsmCtl->max_files),
				 errhint("Format a new HSM device with a larger hsm_max_files.")));

	memset(&tmp, 0, sizeof(tmp));
	tmp.key.rnode = rnode;
	tmp.key.forknum = forknum;
	tmp.slot = slot;

	/* 先写盘，成功后再改内存 */
	if (!hsm_write_record(&tmp, WARNING))
	{
		hsm_free_slot(slot);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create HSM relation %s", relpathperm(rnode, forknum))));
	}

	f = (HsmFile *) hash_search(HsmHash, &tmp.key, HASH_ENTER_NULL, &found);
	if (f == NULL)
	{
		hsm_free_slot(slot);
		(void) hsm_write_slot(slot, zero_record, WARNING);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory for HSM file table")));
	}
	Assert(!found);
	memcpy(f, &tmp, sizeof(HsmFile));
	return f;
}

/*
 * 立即删除一个 fork：释放所有区、清空文件表记录、归还槽位。
 * 调用者持有 HsmLock 排他锁。返回 false 表示写盘失败（内存状态不变）。
 */
static bool
hsm_remove_entry(HsmFile *f, int elevel)
{
	if (!hsm_write_slot(f->slot, zero_record, elevel))
		return false;
	for (int i = 0; i < f->nextents; i++)
		hsm_release_extent(&f->extents[i]);
	hsm_free_slot(f->slot);
	hash_search(HsmHash, &f->key, HASH_REMOVE, NULL);
	return true;
}

/*
 * 把 f 截断到 nblocks 块：完全位于 nblocks 之后的区被释放。
 * 若 tombstone 为 true，同时打上墓碑标记。先写盘，后改内存。
 * 不分配内存，可在临界区中调用。
 */
static bool
hsm_shrink_entry(HsmFile *f, BlockNumber nblocks, bool tombstone, int elevel)
{
	HsmFile		newf;

	memcpy(&newf, f, sizeof(HsmFile));
	newf.nblocks = nblocks;
	if (tombstone)
		newf.flags |= HSM_FLAG_TOMBSTONE;
	while (newf.nextents > 0 &&
		   newf.extents[newf.nextents - 1].lstart >= nblocks)
	{
		newf.capacity -= newf.extents[newf.nextents - 1].nblocks;
		newf.nextents--;
	}

	if (!hsm_write_record(&newf, elevel))
		return false;

	for (int i = newf.nextents; i < f->nextents; i++)
		hsm_release_extent(&f->extents[i]);
	memcpy(f, &newf, sizeof(HsmFile));
	return true;
}

static void
hsm_create_internal(RelFileNode rnode, ForkNumber forknum, bool isRedo)
{
	HsmFile    *f;

	LWLockAcquire(HsmLock, LW_EXCLUSIVE);
	f = hsm_lookup(rnode, forknum);
	if (f != NULL)
	{
		if (isRedo)
		{
			/*
			 * 重放一个后来又被删除的关系的创建：去掉墓碑，让后续 WAL 能写入，
			 * 之后重放到删除记录时会再删掉它。
			 */
			if (f->flags & HSM_FLAG_TOMBSTONE)
			{
				HsmFile		newf;

				memcpy(&newf, f, sizeof(HsmFile));
				newf.flags &= ~HSM_FLAG_TOMBSTONE;
				if (hsm_write_record(&newf, ERROR))
					f->flags = newf.flags;
			}
			LWLockRelease(HsmLock);
			return;
		}
		LWLockRelease(HsmLock);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FILE),
				 errmsg("could not create HSM relation %s: already exists",
						relpathperm(rnode, forknum))));
	}

	(void) hsm_create_entry(rnode, forknum);
	LWLockRelease(HsmLock);

	elog(DEBUG1, "HSM: created %s", relpathperm(rnode, forknum));
}

/*
 * 为写第 blocknum 块做准备：保证 fork 已分配到足够的区。返回扩展前的长度。
 * 空间不足时先尝试 fsync 设备以回收 pending 的区，仍不足则报错。
 */
static BlockNumber
hsm_prepare_extend(RelFileNode rnode, ForkNumber forknum, BlockNumber blocknum)
{
	bool		synced = false;

	for (;;)
	{
		HsmFile    *f;
		BlockNumber old_nblocks;
		bool		added = false;
		uint32		reclaimable;

		LWLockAcquire(HsmLock, LW_EXCLUSIVE);
		f = hsm_lookup(rnode, forknum);
		if (f == NULL)
		{
			if (!InRecovery)
			{
				LWLockRelease(HsmLock);
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FILE),
						 errmsg("could not extend HSM relation %s: it does not exist",
								relpathperm(rnode, forknum))));
			}
			/* 与 md.c 的 EXTENSION_CREATE_RECOVERY 一致：恢复期间自动创建 */
			f = hsm_create_entry(rnode, forknum);
		}
		else if (f->flags & HSM_FLAG_TOMBSTONE)
		{
			HsmFile		newf;

			if (!InRecovery)
			{
				LWLockRelease(HsmLock);
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FILE),
						 errmsg("could not extend HSM relation %s: it has been dropped",
								relpathperm(rnode, forknum))));
			}
			memcpy(&newf, f, sizeof(HsmFile));
			newf.flags &= ~HSM_FLAG_TOMBSTONE;
			if (hsm_write_record(&newf, ERROR))
				f->flags = newf.flags;
		}

		old_nblocks = f->nblocks;
		while (f->capacity <= blocknum)
		{
			if (!hsm_add_extent(f))
				break;
			added = true;
		}
		if (added)
			(void) hsm_write_record(f, ERROR);

		if (f->capacity > blocknum)
		{
			LWLockRelease(HsmLock);
			return old_nblocks;
		}

		reclaimable = HsmCtl->pending_units + HsmCtl->releasing_units;
		LWLockRelease(HsmLock);

		if (!synced && reclaimable > 0)
		{
			/* 有刚释放、尚未持久化的空间：fsync 之后就能复用 */
			if (hsm_sync_device() != 0)
				ereport(data_sync_elevel(ERROR),
						(errcode_for_file_access(),
						 errmsg("could not fsync HSM device \"%s\": %m", hsm_device)));
			synced = true;
			continue;
		}

		ereport(ERROR,
				(errcode(ERRCODE_DISK_FULL),
				 errmsg("could not extend HSM relation %s to %u blocks: no space left on device \"%s\"",
						relpathperm(rnode, forknum), blocknum + 1, hsm_device),
				 errhint("Use a larger device for hsm_device.")));
	}
}

/* 扩展过程中重新查找条目；它不应在此期间消失（上层持有扩展锁） */
static HsmFile *
hsm_lookup_existing(RelFileNode rnode, ForkNumber forknum)
{
	HsmFile    *f = hsm_lookup(rnode, forknum);

	if (f == NULL)
		elog(ERROR, "HSM relation %s disappeared while being extended",
			 relpathperm(rnode, forknum));
	return f;
}

static void
hsm_write_block_at(int fd, BlockNumber phys, const char *buffer, uint32 wait_event,
				   RelFileNode rnode, ForkNumber forknum, BlockNumber blocknum)
{
	ssize_t		nbytes;

	pgstat_report_wait_start(wait_event);
	nbytes = hsm_pwrite_full(fd, buffer, BLCKSZ, (off_t) phys * BLCKSZ);
	pgstat_report_wait_end();

	if (nbytes != BLCKSZ)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write block %u of HSM relation %s (device block %u): %m",
							blocknum, relpathperm(rnode, forknum), phys)));
		ereport(ERROR,
				(errcode(ERRCODE_DISK_FULL),
				 errmsg("could not write block %u of HSM relation %s: wrote only %zd of %d bytes",
						blocknum, relpathperm(rnode, forknum), nbytes, BLCKSZ)));
	}
}

/*
 * 写一个块。块在当前长度之内时只需“查映射 + pwrite”；超出长度时走扩展
 * 流程：分配区、把中间的空洞填零（与 md.c 中“跳着写文件会留下零”语义一致，
 * hash 索引依赖这一点），写目标块，最后更新长度。
 */
static void
hsm_write_internal(RelFileNode rnode, ForkNumber forknum, BlockNumber blocknum,
				   const char *buffer, bool skipFsync, bool is_extend)
{
	int			fd = hsm_fd_or_error();
	uint32		wait_event = is_extend ? WAIT_EVENT_DATA_FILE_EXTEND : WAIT_EVENT_DATA_FILE_WRITE;
	HsmFile    *f;
	BlockNumber phys;
	BlockNumber old_nblocks;

	/* 快路径 */
	LWLockAcquire(HsmLock, LW_SHARED);
	f = hsm_lookup(rnode, forknum);
	if (f != NULL && blocknum < f->nblocks)
	{
		phys = hsm_map_block(f, blocknum, NULL);
		LWLockRelease(HsmLock);

		hsm_write_block_at(fd, phys, buffer, wait_event, rnode, forknum, blocknum);
		pg_atomic_fetch_add_u64(is_extend ? &HsmCtl->nextends : &HsmCtl->nwrites, 1);
		if (!skipFsync)
			hsm_register_dirty();
		return;
	}
	LWLockRelease(HsmLock);

	if (!is_extend)
		elog(DEBUG1, "HSM: write of block %u beyond the end of %s, extending",
			 blocknum, relpathperm(rnode, forknum));

	/* 慢路径：扩展（同一 fork 的扩展由上层的 relation extension lock 串行化） */
	old_nblocks = hsm_prepare_extend(rnode, forknum, blocknum);

	if (blocknum > old_nblocks)
	{
		PGAlignedBlock zerobuf;
		BlockNumber b = old_nblocks;

		memset(zerobuf.data, 0, BLCKSZ);
		while (b < blocknum)
		{
			uint32		run;

			LWLockAcquire(HsmLock, LW_SHARED);
			f = hsm_lookup_existing(rnode, forknum);
			phys = hsm_map_block(f, b, &run);
			LWLockRelease(HsmLock);

			run = Min(run, blocknum - b);
			for (uint32 k = 0; k < run; k++)
				hsm_write_block_at(fd, phys + k, zerobuf.data, wait_event,
								   rnode, forknum, b + k);
			b += run;
		}
	}

	LWLockAcquire(HsmLock, LW_SHARED);
	f = hsm_lookup_existing(rnode, forknum);
	phys = hsm_map_block(f, blocknum, NULL);
	LWLockRelease(HsmLock);

	hsm_write_block_at(fd, phys, buffer, wait_event, rnode, forknum, blocknum);
	pg_atomic_fetch_add_u64(&HsmCtl->nextends, 1);

	/* 数据写完后再把新长度公布出去，别的进程不会读到未初始化的块 */
	LWLockAcquire(HsmLock, LW_EXCLUSIVE);
	f = hsm_lookup(rnode, forknum);
	if (f != NULL && blocknum >= f->nblocks)
	{
		HsmFile		newf;

		memcpy(&newf, f, sizeof(HsmFile));
		newf.nblocks = blocknum + 1;
		if (hsm_write_record(&newf, ERROR))
			f->nblocks = newf.nblocks;
	}
	LWLockRelease(HsmLock);

	if (!skipFsync)
		hsm_register_dirty();
}

/* 读一个块。返回 false 表示块不存在（fork 不存在或超出长度）。 */
static bool
hsm_read_internal(RelFileNode rnode, ForkNumber forknum, BlockNumber blocknum,
				  char *buffer)
{
	int			fd = hsm_fd_or_error();
	HsmFile    *f;
	BlockNumber phys;
	ssize_t		nbytes;

	LWLockAcquire(HsmLock, LW_SHARED);
	f = hsm_lookup(rnode, forknum);
	if (f == NULL || blocknum >= f->nblocks)
	{
		LWLockRelease(HsmLock);
		return false;
	}
	phys = hsm_map_block(f, blocknum, NULL);
	LWLockRelease(HsmLock);

	pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_READ);
	nbytes = hsm_pread_full(fd, buffer, BLCKSZ, (off_t) phys * BLCKSZ);
	pgstat_report_wait_end();

	if (nbytes != BLCKSZ)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read block %u of HSM relation %s (device block %u): %m",
							blocknum, relpathperm(rnode, forknum), phys)));
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("could not read block %u of HSM relation %s: read only %zd of %d bytes",
						blocknum, relpathperm(rnode, forknum), nbytes, BLCKSZ)));
	}
	pg_atomic_fetch_add_u64(&HsmCtl->nreads, 1);
	return true;
}

/* 删除一个 fork（unlink 的单 fork 版本）。不抛 ERROR。 */
static void
hsm_unlink_fork(RelFileNode rnode, ForkNumber forknum, bool isRedo)
{
	HsmFile    *f;
	bool		tombstone;
	bool		ok;

	tombstone = (!isRedo && !IsBinaryUpgrade && forknum == MAIN_FORKNUM);

	LWLockAcquire(HsmLock, LW_EXCLUSIVE);
	f = hsm_lookup(rnode, forknum);
	if (f == NULL || (tombstone && (f->flags & HSM_FLAG_TOMBSTONE)))
	{
		LWLockRelease(HsmLock);
		return;					/* 与 md.c 一样，不存在就算了 */
	}

	if (tombstone)
	{
		/*
		 * main fork：立即释放空间，但保留一条长度为 0 的墓碑记录，直到下一个
		 * checkpoint 之后才删除，防止 relfilenode 被过早复用（见 mdunlink）。
		 */
		ok = hsm_shrink_entry(f, 0, true, WARNING);
	}
	else
		ok = hsm_remove_entry(f, WARNING);
	LWLockRelease(HsmLock);

	if (!ok)
		return;

	if (tombstone)
	{
		FileTag		tag;

		memset(&tag, 0, sizeof(tag));
		tag.handler = SYNC_HANDLER_HSM;
		tag.forknum = MAIN_FORKNUM;
		tag.rnode = rnode;
		RegisterSyncRequest(&tag, SYNC_UNLINK_REQUEST, true /* retryOnError */ );
	}

	/* 让下一个 checkpoint 把“释放”落盘，pending 的区才能被复用 */
	hsm_register_dirty();

	elog(DEBUG1, "HSM: %s %s", tombstone ? "tombstoned" : "removed",
		 relpathperm(rnode, forknum));
}


/* ----------------------------------------------------------------
 *		f_smgr 接口
 * ----------------------------------------------------------------
 */

/* 每个后端启动时调用。设备在第一次 I/O 时才打开。 */
void
hsm_init(void)
{
}

/* 后端退出时关闭设备 fd */
void
hsm_shutdown(void)
{
	if (hsm_fd >= 0)
	{
		close(hsm_fd);
		ReleaseExternalFD();
		hsm_fd = -1;
	}
}

/*
 * md.c 需要为每个 fork 维护打开的段文件（MdfdVec + vfd）。HSM 的所有关系
 * 共用一个设备 fd，元数据在共享内存中，因此 open/close 不需要做任何事。
 */
void
hsm_open(SMgrRelation reln)
{
}

void
hsm_close(SMgrRelation reln, ForkNumber forknum)
{
}

void
hsm_create(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	hsm_create_internal(reln->smgr_rnode.node, forknum, isRedo);

	/* 与 mdcreate 一样，让 checkpoint 把新文件的元数据持久化 */
	if (!SmgrIsTemp(reln))
		hsm_register_dirty();
}

/* 与 md.c 一样，墓碑（等待删除的 main fork）也算存在 */
bool
hsm_exists(SMgrRelation reln, ForkNumber forknum)
{
	bool		exists;

	LWLockAcquire(HsmLock, LW_SHARED);
	exists = (hsm_lookup(reln->smgr_rnode.node, forknum) != NULL);
	LWLockRelease(HsmLock);
	return exists;
}

void
hsm_unlink(RelFileNodeBackend rnode, ForkNumber forknum, bool isRedo)
{
	if (forknum == InvalidForkNumber)
	{
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			hsm_unlink_fork(rnode.node, forknum, isRedo);
	}
	else
		hsm_unlink_fork(rnode.node, forknum, isRedo);
}

void
hsm_extend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   char *buffer, bool skipFsync)
{
	if (blocknum == InvalidBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend HSM relation %s beyond %u blocks",
						relpath(reln->smgr_rnode, forknum), InvalidBlockNumber)));

	hsm_write_internal(reln->smgr_rnode.node, forknum, blocknum, buffer,
					   skipFsync || SmgrIsTemp(reln), true);
}

bool
hsm_prefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
#if defined(USE_PREFETCH) && defined(POSIX_FADV_WILLNEED)
	HsmFile    *f;
	BlockNumber phys = InvalidBlockNumber;
	int			fd;

	LWLockAcquire(HsmLock, LW_SHARED);
	f = hsm_lookup(reln->smgr_rnode.node, forknum);
	if (f == NULL)
	{
		LWLockRelease(HsmLock);
		if (InRecovery)
			return false;
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FILE),
				 errmsg("could not open HSM relation %s: it does not exist",
						relpath(reln->smgr_rnode, forknum))));
	}
	if (blocknum < f->capacity)
		phys = hsm_map_block(f, blocknum, NULL);
	LWLockRelease(HsmLock);

	if (phys != InvalidBlockNumber && (fd = hsm_get_fd()) >= 0)
		(void) posix_fadvise(fd, (off_t) phys * BLCKSZ, BLCKSZ, POSIX_FADV_WILLNEED);
#endif
	return true;
}

void
hsm_read(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 char *buffer)
{
	if (hsm_read_internal(reln->smgr_rnode.node, forknum, blocknum, buffer))
		return;

	/*
	 * 块不存在。与 mdread 一样：zero_damaged_pages 打开或正在恢复时返回全零页，
	 * 否则报错。
	 */
	if (zero_damaged_pages || InRecovery)
	{
		MemSet(buffer, 0, BLCKSZ);
		return;
	}
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("could not read block %u of HSM relation %s: block does not exist",
					blocknum, relpath(reln->smgr_rnode, forknum))));
}

void
hsm_write(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		  char *buffer, bool skipFsync)
{
	hsm_write_internal(reln->smgr_rnode.node, forknum, blocknum, buffer,
					   skipFsync || SmgrIsTemp(reln), false);
}

/* 提示内核尽快把这些块写回设备（对应 md 的 FileWriteback） */
void
hsm_writeback(SMgrRelation reln, ForkNumber forknum,
			  BlockNumber blocknum, BlockNumber nblocks)
{
	int			fd = hsm_get_fd();

	if (fd < 0)
		return;

	while (nblocks > 0)
	{
		HsmFile    *f;
		BlockNumber phys;
		uint32		run;

		LWLockAcquire(HsmLock, LW_SHARED);
		f = hsm_lookup(reln->smgr_rnode.node, forknum);
		if (f == NULL || blocknum >= f->capacity)
		{
			LWLockRelease(HsmLock);
			return;
		}
		phys = hsm_map_block(f, blocknum, &run);
		LWLockRelease(HsmLock);

		run = Min(run, nblocks);
		pg_flush_data(fd, (off_t) phys * BLCKSZ, (off_t) run * BLCKSZ);
		blocknum += run;
		nblocks -= run;
	}
}

BlockNumber
hsm_nblocks(SMgrRelation reln, ForkNumber forknum)
{
	HsmFile    *f;
	BlockNumber nblocks;

	LWLockAcquire(HsmLock, LW_SHARED);
	f = hsm_lookup(reln->smgr_rnode.node, forknum);
	if (f == NULL)
	{
		LWLockRelease(HsmLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FILE),
				 errmsg("could not open HSM relation %s: it does not exist",
						relpath(reln->smgr_rnode, forknum))));
	}
	nblocks = f->nblocks;
	LWLockRelease(HsmLock);
	return nblocks;
}

/*
 * 截断。与 mdtruncate 一样不分配内存（可能在临界区中被调用）。
 * 被截掉的完整区立即从文件中摘除，空间在下一次设备 fsync 后才能复用。
 */
void
hsm_truncate(SMgrRelation reln, ForkNumber forknum,
			 BlockNumber old_blocks, BlockNumber nblocks)
{
	HsmFile    *f;

	LWLockAcquire(HsmLock, LW_EXCLUSIVE);
	f = hsm_lookup(reln->smgr_rnode.node, forknum);
	if (f == NULL)
	{
		LWLockRelease(HsmLock);
		if (InRecovery)
			return;
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FILE),
				 errmsg("could not truncate HSM relation %s: it does not exist",
						relpath(reln->smgr_rnode, forknum))));
	}
	if (nblocks > f->nblocks)
	{
		BlockNumber cur = f->nblocks;

		LWLockRelease(HsmLock);
		/* 与 mdtruncate 一样，恢复期间不抱怨 */
		if (InRecovery)
			return;
		ereport(ERROR,
				(errmsg("could not truncate HSM relation %s to %u blocks: it's only %u blocks now",
						relpath(reln->smgr_rnode, forknum), nblocks, cur)));
	}
	if (nblocks == f->nblocks)
	{
		LWLockRelease(HsmLock);
		return;
	}
	(void) hsm_shrink_entry(f, nblocks, false, ERROR);
	LWLockRelease(HsmLock);

	if (!SmgrIsTemp(reln))
		hsm_register_dirty();
}

/* 立即持久化：fsync 整个设备 */
void
hsm_immedsync(SMgrRelation reln, ForkNumber forknum)
{
	if (hsm_sync_device() != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync HSM device \"%s\": %m", hsm_device)));
}


/* ----------------------------------------------------------------
 *		同步处理器（由 checkpointer 调用）
 * ----------------------------------------------------------------
 */
int
hsm_syncfiletag(const FileTag *ftag, char *path)
{
	strlcpy(path, hsm_device ? hsm_device : "", MAXPGPATH);
	if (HsmCtl == NULL)
		return 0;
	return hsm_sync_device();
}

/* checkpoint 之后，真正删除墓碑 */
int
hsm_unlinkfiletag(const FileTag *ftag, char *path)
{
	char	   *p;
	HsmFile    *f;
	int			rc = 0;

	p = relpathperm(ftag->rnode, ftag->forknum);
	strlcpy(path, p, MAXPGPATH);
	pfree(p);

	if (HsmCtl == NULL)
		return 0;

	LWLockAcquire(HsmLock, LW_EXCLUSIVE);
	f = hsm_lookup(ftag->rnode, ftag->forknum);
	/* 墓碑可能已经被重放“复活”为新关系，那就不能删 */
	if (f != NULL && (f->flags & HSM_FLAG_TOMBSTONE))
	{
		if (!hsm_remove_entry(f, LOG))
			rc = -1;
	}
	LWLockRelease(HsmLock);

	if (rc == 0)
		hsm_register_dirty();
	return rc;
}

bool
hsm_filetagmatches(const FileTag *ftag, const FileTag *candidate)
{
	/* 只用于 DROP DATABASE：取消该库所有待删除的墓碑 */
	return ftag->rnode.dbNode == candidate->rnode.dbNode;
}


/* ----------------------------------------------------------------
 *		数据库级别的配套操作
 * ----------------------------------------------------------------
 */

/* DROP DATABASE：删除该库在 HSM 中的所有 fork */
void
hsm_drop_database(Oid dbid)
{
	HASH_SEQ_STATUS status;
	HsmFile    *f;
	FileTag		tag;
	int			nremoved = 0;

	if (HsmCtl == NULL)
		return;

	LWLockAcquire(HsmLock, LW_EXCLUSIVE);
	hash_seq_init(&status, HsmHash);
	while ((f = (HsmFile *) hash_seq_search(&status)) != NULL)
	{
		if (f->key.rnode.dbNode != dbid)
			continue;
		/* 动态哈希允许在顺序扫描中删除当前条目 */
		if (hsm_remove_entry(f, WARNING))
			nremoved++;
	}
	LWLockRelease(HsmLock);

	if (nremoved > 0)
	{
		memset(&tag, 0, sizeof(tag));
		tag.handler = SYNC_HANDLER_HSM;
		tag.forknum = InvalidForkNumber;
		tag.rnode.dbNode = dbid;
		RegisterSyncRequest(&tag, SYNC_FILTER_REQUEST, true /* retryOnError */ );
		hsm_register_dirty();
		elog(DEBUG1, "HSM: removed %d relation forks of database %u", nremoved, dbid);
	}
}

bool
hsm_database_has_relations(Oid dbid)
{
	HASH_SEQ_STATUS status;
	HsmFile    *f;
	bool		found = false;

	if (HsmCtl == NULL)
		return false;

	LWLockAcquire(HsmLock, LW_SHARED);
	hash_seq_init(&status, HsmHash);
	while ((f = (HsmFile *) hash_seq_search(&status)) != NULL)
	{
		if (f->key.rnode.dbNode == dbid && !(f->flags & HSM_FLAG_TOMBSTONE))
		{
			found = true;
			hash_seq_term(&status);
			break;
		}
	}
	LWLockRelease(HsmLock);
	return found;
}

/* 某个库（dbid 为 InvalidOid 时为全部）在 HSM 中的数据量（字节） */
int64
hsm_database_size(Oid dbid)
{
	HASH_SEQ_STATUS status;
	HsmFile    *f;
	int64		total = 0;

	if (HsmCtl == NULL)
		return 0;

	LWLockAcquire(HsmLock, LW_SHARED);
	hash_seq_init(&status, HsmHash);
	while ((f = (HsmFile *) hash_seq_search(&status)) != NULL)
	{
		if (!OidIsValid(dbid) || f->key.rnode.dbNode == dbid)
			total += (int64) f->nblocks * BLCKSZ;
	}
	LWLockRelease(HsmLock);
	return total;
}

/* pg_relation_size() 用：*found 为 false 表示这个 fork 不在 HSM 中 */
int64
hsm_relation_size(RelFileNode rnode, ForkNumber forknum, bool *found)
{
	HsmFile    *f;
	int64		size = 0;

	*found = false;
	if (HsmCtl == NULL)
		return 0;

	LWLockAcquire(HsmLock, LW_SHARED);
	f = hsm_lookup(rnode, forknum);
	if (f != NULL)
	{
		*found = true;
		size = (int64) f->nblocks * BLCKSZ;
	}
	LWLockRelease(HsmLock);
	return size;
}

/*
 * 崩溃恢复时重置 unlogged 表（md.c 的对应逻辑在 reinit.c 中，靠扫描目录完成）：
 *	CLEANUP：删除带 init fork 的关系的所有其它 fork；
 *	INIT：   把 init fork 复制为 main fork。
 */
void
hsm_reset_unlogged_relations(int op)
{
	HASH_SEQ_STATUS status;
	HsmFile    *f;
	RelFileNode *rnodes;
	int			n = 0;
	int			maxn;

	if (HsmCtl == NULL)
		return;

	LWLockAcquire(HsmLock, LW_SHARED);
	maxn = hash_get_num_entries(HsmHash);
	rnodes = (RelFileNode *) palloc(sizeof(RelFileNode) * Max(maxn, 1));
	hash_seq_init(&status, HsmHash);
	while ((f = (HsmFile *) hash_seq_search(&status)) != NULL)
	{
		if (f->key.forknum == INIT_FORKNUM && !(f->flags & HSM_FLAG_TOMBSTONE))
			rnodes[n++] = f->key.rnode;
	}
	LWLockRelease(HsmLock);

	for (int i = 0; i < n; i++)
	{
		if (op & UNLOGGED_RELATION_CLEANUP)
		{
			hsm_unlink_fork(rnodes[i], MAIN_FORKNUM, true);
			hsm_unlink_fork(rnodes[i], FSM_FORKNUM, true);
			hsm_unlink_fork(rnodes[i], VISIBILITYMAP_FORKNUM, true);
		}
		if (op & UNLOGGED_RELATION_INIT)
		{
			PGAlignedBlock buf;
			BlockNumber nblocks;

			/* 与 md.c 用 init fork 覆盖 main fork 一样：先删后建再复制 */
			hsm_unlink_fork(rnodes[i], MAIN_FORKNUM, true);
			hsm_create_internal(rnodes[i], MAIN_FORKNUM, false);

			LWLockAcquire(HsmLock, LW_SHARED);
			f = hsm_lookup(rnodes[i], INIT_FORKNUM);
			nblocks = f ? f->nblocks : 0;
			LWLockRelease(HsmLock);

			for (BlockNumber b = 0; b < nblocks; b++)
			{
				if (!hsm_read_internal(rnodes[i], INIT_FORKNUM, b, buf.data))
					MemSet(buf.data, 0, BLCKSZ);
				hsm_write_internal(rnodes[i], MAIN_FORKNUM, b, buf.data, true, true);
			}
		}
	}

	if ((op & UNLOGGED_RELATION_INIT) && n > 0 && enableFsync)
	{
		if (hsm_sync_device() != 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync HSM device \"%s\": %m", hsm_device)));
	}

	if (n > 0)
		elog(DEBUG1, "HSM: reset %d unlogged relations (op %d)", n, op);
	pfree(rnodes);
}


/* ----------------------------------------------------------------
 *		SQL 观测函数
 * ----------------------------------------------------------------
 */

/* 一致性快照：持锁把所有条目复制出来，按 (db, relfilenode, fork) 排序 */
static int
hsm_file_cmp(const void *a, const void *b)
{
	const HsmFile *fa = (const HsmFile *) a;
	const HsmFile *fb = (const HsmFile *) b;

	if (fa->key.rnode.dbNode != fb->key.rnode.dbNode)
		return fa->key.rnode.dbNode < fb->key.rnode.dbNode ? -1 : 1;
	if (fa->key.rnode.relNode != fb->key.rnode.relNode)
		return fa->key.rnode.relNode < fb->key.rnode.relNode ? -1 : 1;
	return fa->key.forknum - fb->key.forknum;
}

static HsmFile *
hsm_snapshot_files(int *nfiles)
{
	HASH_SEQ_STATUS status;
	HsmFile    *f;
	HsmFile    *arr;
	int			n = 0;

	LWLockAcquire(HsmLock, LW_SHARED);
	arr = (HsmFile *) palloc(sizeof(HsmFile) * Max(hash_get_num_entries(HsmHash), 1));
	hash_seq_init(&status, HsmHash);
	while ((f = (HsmFile *) hash_seq_search(&status)) != NULL)
		memcpy(&arr[n++], f, sizeof(HsmFile));
	LWLockRelease(HsmLock);

	qsort(arr, n, sizeof(HsmFile), hsm_file_cmp);
	*nfiles = n;
	return arr;
}

/* pg_hsm_status()：设备与空间使用概况 */
Datum
pg_hsm_status(PG_FUNCTION_ARGS)
{
#define HSM_STATUS_COLS 17
	TupleDesc	tupdesc;
	Datum		values[HSM_STATUS_COLS];
	bool		nulls[HSM_STATUS_COLS];

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	memset(nulls, 0, sizeof(nulls));
	memset(values, 0, sizeof(values));

	if (HsmCtl == NULL)
	{
		values[0] = BoolGetDatum(false);
		for (int i = 1; i < HSM_STATUS_COLS; i++)
			nulls[i] = true;
	}
	else
	{
		uint32		free_units,
					pending_units,
					used_slots;

		LWLockAcquire(HsmLock, LW_SHARED);
		free_units = HsmCtl->free_units;
		pending_units = HsmCtl->pending_units + HsmCtl->releasing_units;
		used_slots = HsmCtl->used_slots;
		LWLockRelease(HsmLock);

		values[0] = BoolGetDatum(true);
		values[1] = CStringGetTextDatum(hsm_device);
		values[2] = Int64GetDatum((int64) HsmCtl->sb.device_blocks);
		values[3] = Int64GetDatum((int64) HsmCtl->sb.data_start);
		values[4] = Int32GetDatum(HSM_UNIT_BLOCKS * BLCKSZ / 1024);
		values[5] = Int64GetDatum((int64) HsmCtl->nunits);
		values[6] = Int64GetDatum((int64) HsmCtl->nunits - free_units - pending_units);
		values[7] = Int64GetDatum((int64) pending_units);
		values[8] = Int64GetDatum((int64) free_units);
		values[9] = Int32GetDatum((int32) used_slots);
		values[10] = Int32GetDatum((int32) HsmCtl->max_files);
		values[11] = Int64GetDatum((int64) pg_atomic_read_u64(&HsmCtl->nreads));
		values[12] = Int64GetDatum((int64) pg_atomic_read_u64(&HsmCtl->nwrites));
		values[13] = Int64GetDatum((int64) pg_atomic_read_u64(&HsmCtl->nextends));
		values[14] = Int64GetDatum((int64) pg_atomic_read_u64(&HsmCtl->nallocs));
		values[15] = Int64GetDatum((int64) pg_atomic_read_u64(&HsmCtl->nfrees));
		values[16] = Int64GetDatum((int64) pg_atomic_read_u64(&HsmCtl->nsyncs));
	}

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* pg_hsm_files()：文件表，每个关系 fork 一行 */
Datum
pg_hsm_files(PG_FUNCTION_ARGS)
{
#define HSM_FILES_COLS 9
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HsmFile    *arr;
	int			n;

	InitMaterializedSRF(fcinfo, 0);
	if (HsmCtl == NULL)
		return (Datum) 0;

	arr = hsm_snapshot_files(&n);
	for (int i = 0; i < n; i++)
	{
		Datum		values[HSM_FILES_COLS];
		bool		nulls[HSM_FILES_COLS];

		memset(nulls, 0, sizeof(nulls));
		values[0] = ObjectIdGetDatum(arr[i].key.rnode.relNode);
		values[1] = ObjectIdGetDatum(arr[i].key.rnode.dbNode);
		values[2] = ObjectIdGetDatum(arr[i].key.rnode.spcNode);
		values[3] = CStringGetTextDatum(forkNames[arr[i].key.forknum]);
		values[4] = Int32GetDatum(arr[i].slot);
		values[5] = Int64GetDatum((int64) arr[i].nblocks);
		values[6] = Int32GetDatum((int32) arr[i].nextents);
		values[7] = Int64GetDatum((int64) arr[i].capacity);
		values[8] = BoolGetDatum((arr[i].flags & HSM_FLAG_TOMBSTONE) != 0);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	pfree(arr);
	return (Datum) 0;
}

/* pg_hsm_extents()：区表，每个区一行（对应 PPT 中的 FILE_ID / Extent_ID 表） */
Datum
pg_hsm_extents(PG_FUNCTION_ARGS)
{
#define HSM_EXTENTS_COLS 7
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HsmFile    *arr;
	int			n;

	InitMaterializedSRF(fcinfo, 0);
	if (HsmCtl == NULL)
		return (Datum) 0;

	arr = hsm_snapshot_files(&n);
	for (int i = 0; i < n; i++)
	{
		for (int k = 0; k < arr[i].nextents; k++)
		{
			Datum		values[HSM_EXTENTS_COLS];
			bool		nulls[HSM_EXTENTS_COLS];

			memset(nulls, 0, sizeof(nulls));
			values[0] = ObjectIdGetDatum(arr[i].key.rnode.relNode);
			values[1] = ObjectIdGetDatum(arr[i].key.rnode.dbNode);
			values[2] = CStringGetTextDatum(forkNames[arr[i].key.forknum]);
			values[3] = Int32GetDatum(k);
			values[4] = Int64GetDatum((int64) arr[i].extents[k].lstart);
			values[5] = Int64GetDatum((int64) arr[i].extents[k].start);
			values[6] = Int64GetDatum((int64) arr[i].extents[k].nblocks);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
	}
	pfree(arr);
	return (Datum) 0;
}
