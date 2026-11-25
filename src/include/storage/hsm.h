/*-------------------------------------------------------------------------
 *
 * hsm.h
 *	  HSM (Halo-style Storage Manager) —— 直接在块设备上存放关系数据的存储引擎。
 *
 *	  HSM 是 smgrsw[] 中的第 2 个存储管理器（下标 1）。打开 hsm_device 后，
 *	  默认表空间中的用户关系（relfilenode >= FirstNormalObjectId）都存放在
 *	  该块设备上；系统表、临时表以及其它表空间中的关系仍由 md.c 存放在普通文件中。
 *
 *	  设计说明见 src/backend/storage/smgr/README.hsm。
 *
 * src/include/storage/hsm.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HSM_H
#define HSM_H

#include "storage/block.h"
#include "storage/relfilenode.h"
#include "storage/smgr.h"
#include "storage/sync.h"

/* HSM 在 smgr.c 中 smgrsw[] 的下标 */
#define HSM_SMGR_ID		1

/* GUC 参数 */
extern PGDLLIMPORT char *hsm_device;
extern PGDLLIMPORT int hsm_max_files;

/* 共享内存 */
extern Size HsmShmemSize(void);
extern void HsmShmemInit(void);

/* 路由：决定一个关系由哪个存储管理器负责 */
extern int	hsm_choose_smgr(RelFileNodeBackend rnode);
extern bool hsm_relfilenode_in_use(RelFileNode rnode);

/* f_smgr 接口实现 */
extern void hsm_init(void);
extern void hsm_shutdown(void);
extern void hsm_open(SMgrRelation reln);
extern void hsm_close(SMgrRelation reln, ForkNumber forknum);
extern void hsm_create(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern bool hsm_exists(SMgrRelation reln, ForkNumber forknum);
extern void hsm_unlink(RelFileNodeBackend rnode, ForkNumber forknum, bool isRedo);
extern void hsm_extend(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber blocknum, char *buffer, bool skipFsync);
extern bool hsm_prefetch(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber blocknum);
extern void hsm_read(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
					 char *buffer);
extern void hsm_write(SMgrRelation reln, ForkNumber forknum,
					  BlockNumber blocknum, char *buffer, bool skipFsync);
extern void hsm_writeback(SMgrRelation reln, ForkNumber forknum,
						  BlockNumber blocknum, BlockNumber nblocks);
extern BlockNumber hsm_nblocks(SMgrRelation reln, ForkNumber forknum);
extern void hsm_truncate(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber old_blocks, BlockNumber nblocks);
extern void hsm_immedsync(SMgrRelation reln, ForkNumber forknum);

/* sync.c 的同步处理器（checkpoint 时调用） */
extern int	hsm_syncfiletag(const FileTag *ftag, char *path);
extern int	hsm_unlinkfiletag(const FileTag *ftag, char *path);
extern bool hsm_filetagmatches(const FileTag *ftag, const FileTag *candidate);

/* 数据库级别的配套操作 */
extern void hsm_drop_database(Oid dbid);
extern bool hsm_database_has_relations(Oid dbid);
extern int64 hsm_database_size(Oid dbid);
extern int64 hsm_relation_size(RelFileNode rnode, ForkNumber forknum,
							   bool *found);
extern void hsm_reset_unlogged_relations(int op);

#endif							/* HSM_H */
