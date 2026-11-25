# 大作业二：PostgreSQL 15 块设备存储引擎（HSM）

本分支在 PostgreSQL 15（REL_15_STABLE，15.20）上新增了一个存储引擎 **HSM**，
注册为 `smgrsw[]` 中的第二个存储管理器。它不经过文件系统，把表、索引等关系数据
**直接存放在本地块设备上**（例如 `losetup` 得到的 `/dev/loop0`），并自己完成
空间分配、回收和元数据检索。

- `f_smgr` 的全部 15 个接口都已实现。作业要求的 `smgr_init / open / create / read / write / close` 之外，
  `exists / extend / nblocks / truncate / unlink / prefetch / writeback / immedsync / shutdown` 也一并实现。
- 设计说明：[`src/backend/storage/smgr/README.hsm`](src/backend/storage/smgr/README.hsm)
- 主要代码：[`src/backend/storage/smgr/hsm.c`](src/backend/storage/smgr/hsm.c)、
  [`src/include/storage/hsm.h`](src/include/storage/hsm.h)

## 设计一览

```
块 0           超级块：魔数 "HSM1"、版本、设备大小、文件表/数据区位置、CRC
块 1..N        文件表：每个关系 fork 一条 512 字节（= 1 个扇区）记录，
               含 RelFileNode、fork、长度、区列表（≈ PPT 中的 FILE_ID / Extent 表）
块 data_start  数据区：以 64KB 分配单元管理；以“区”为分配单位、以“块”为使用单位
```

- **区随文件增长变大**：64KB → 512KB → 4MB → 32MB → 64MB（文件越大、区越大）。
- **元数据检索**：postmaster 启动时把文件表加载到共享内存哈希表
  `(RelFileNode, fork) → 区列表`；块号到设备块号的映射是一次二分查找。
- **崩溃一致性**：与 md.c 一样依靠 WAL 和 checkpoint 保证一致性：
  - 新增 `SYNC_HANDLER_HSM`，每次 checkpoint 只 fsync 设备一次；
  - 释放的空间等到下一次 fsync 成功后才复用；
  - 删除的 main fork 先留下墓碑，到下一次 checkpoint 后才真正删除，避免 relfilenode 过早复用。
- **路由**：只接管默认表空间中新建的用户关系（relfilenode ≥ 16384）。系统表、临时表、
  其它表空间中的表、启用 HSM 之前就已存在的表，仍由 md.c 负责。

## 快速开始（Linux）

```bash
# 1. 编译安装
./configure --prefix=$HOME/pg15 --enable-debug --enable-cassert --enable-tap-tests CFLAGS="-O0 -g3"
make -j$(nproc) && make install

# 2. 准备块设备（课件中的方法，这里用 1GB）
dd if=/dev/zero of=/data/vdisk.img bs=1M count=0 seek=1024
sudo losetup -fP /data/vdisk.img          # 得到 /dev/loop0
sudo chown $USER /dev/loop0

# 3. 初始化并打开 HSM（第一次启动时会自动格式化全零的设备）
$HOME/pg15/bin/initdb -D $HOME/pgdata
echo "hsm_device = '/dev/loop0'" >> $HOME/pgdata/postgresql.conf
$HOME/pg15/bin/pg_ctl -D $HOME/pgdata -l $HOME/pgdata/log.txt start
```

```sql
CREATE TABLE vage (id1 int, c1 varchar(30), id2 int, c2 varchar(30));
INSERT INTO vage SELECT generate_series(1,5), 'AAAAAAAA'||generate_series(1,5),
                        generate_series(1,5)+100, 'aaaaaaaa'||generate_series(1,5);

SELECT * FROM pg_hsm_status();   -- 设备、空间、I/O 统计
SELECT * FROM pg_hsm_files();    -- 文件表：每个 fork 一行
SELECT * FROM pg_hsm_extents();  -- 区表：每个区一行
```

`base/<db>/<relfilenode>` 下没有这张表的文件。执行 `CHECKPOINT` 之后，
可以用 `dd if=/dev/loop0 bs=8192 skip=<device_start_block> count=1 | od -c`
在设备上直接看到元组。

### 参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `hsm_device` | `''`（关闭） | 块设备（或普通文件）路径，修改后需重启 |
| `hsm_max_files` | 4096 | 格式化新设备时文件表的槽位数（fork 数上限） |

## 在 macOS 上运行

macOS 没有 loop 设备，用 Docker Desktop（或 OrbStack）运行 Linux 容器：

```bash
docker build -t pg-hsm -f hsm-docker/Dockerfile .
docker run -it --rm --privileged pg-hsm      # 自动建设备、initdb、启动、跑演示
```

`--privileged` 让容器能用 `losetup` 创建 `/dev/loopN`。如果环境不允许创建，
脚本会改用一个普通文件作为设备，HSM 的行为完全一样。

## 测试

| 测试 | 结果 |
|---|---|
| PG 全量回归测试 `make check`，所有用户表都存放在 HSM 设备上 | **221 / 221 通过** |
| PG 全量回归测试 `make check`，HSM 关闭（验证没有改变原有行为） | **221 / 221 通过** |
| HSM 专项 TAP 测试 `make -C src/test/modules/test_hsm check` | **33 / 33 通过** |
| GUC 配置样例检查 `src/test/modules/test_misc` | 通过 |
| contrib：amcheck、pageinspect、pg_visibility、bloom、pg_surgery | 全部通过 |
| 崩溃测试：pgbench 加上建表/删表/截断的压力下 `kill -9` 式立即停机 3 次 | 余额不变式全部成立，`pg_amcheck` 无损坏 |

运行 HSM 模式的回归测试：

```bash
truncate -s 3G /tmp/regress.img
printf "hsm_device = '/tmp/regress.img'\nhsm_max_files = 30000\n" > /tmp/hsm.conf
make check TEMP_CONFIG=/tmp/hsm.conf
```

TAP 测试覆盖：格式化、重启持久化、崩溃恢复（已提交可见、未提交消失）、unlogged 表重置、
hash 索引的稀疏扩展、墓碑与 checkpoint 后的空间回收、VACUUM 截断、VACUUM FULL、
CREATE/DROP DATABASE、拒绝 FILE_COPY 和 ALTER DATABASE SET TABLESPACE、
设备写满报错后可恢复、1MB 小设备、与已有 md 表混用、拒绝非 HSM 设备、默认关闭。

pgbench（scale 20，4 个客户端，25 秒，debug + cassert 编译，同一台机器）：

| | md.c（文件） | HSM（/dev/loop0） |
|---|---|---|
| TPC-B | 1290 tps | 1318 tps |
| 只读 `-S` | 14459 tps | 13823 tps |

## 修改的文件

| 文件 | 内容 |
|---|---|
| `src/backend/storage/smgr/hsm.c`（新增） | 存储引擎实现 |
| `src/include/storage/hsm.h`（新增） | 接口声明 |
| `src/backend/storage/smgr/README.hsm`（新增） | 设计说明 |
| `src/backend/storage/smgr/smgr.c` | `smgrsw[]` 注册 HSM；`smgropen()` 路由 |
| `src/backend/storage/sync/sync.c`、`src/include/storage/sync.h` | 同步处理器 `SYNC_HANDLER_HSM` |
| `src/backend/storage/ipc/ipci.c` | 共享内存 |
| `src/backend/storage/lmgr/lwlocknames.txt` | `HsmLock`、`HsmSyncLock` |
| `src/backend/utils/misc/guc.c`、`postgresql.conf.sample` | `hsm_device`、`hsm_max_files` |
| `src/backend/catalog/catalog.c` | 分配 relfilenode 时避开 HSM 中仍被占用的编号 |
| `src/backend/commands/dbcommands.c` | DROP/CREATE DATABASE 配套处理 |
| `src/backend/storage/file/reinit.c` | 崩溃后重置 HSM 中的 unlogged 表 |
| `src/backend/utils/adt/dbsize.c` | `pg_relation_size()` 等计入 HSM 数据 |
| `src/include/catalog/pg_proc.dat`、`catversion.h` | `pg_hsm_status / files / extents` |
| `src/test/modules/test_hsm/`（新增） | TAP 测试 |
| `hsm-docker/`（新增） | Docker 运行环境与演示脚本 |

## 已知限制

- 单个 fork 最多 60 个区（约 1.6GB）；文件表大小在格式化时确定。
- pg_basebackup、物理复制备库、pg_upgrade、pg_rewind 等按文件复制的工具看不到设备上的数据。
- 与 md.c 一样，崩溃时未提交事务新建的关系会遗留在设备上。
