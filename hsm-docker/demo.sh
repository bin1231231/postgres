#!/bin/bash
# 准备块设备 -> initdb -> 打开 HSM -> 启动 -> 跑演示 SQL -> 进入 shell
set -e

WORK=${HSM_WORK:-/var/lib/hsm}
IMG=$WORK/vdisk.img
PGDATA=$WORK/data
OSUSER=${HSM_OSUSER:-postgres}
SIZE_MB=${HSM_SIZE_MB:-1024}
# su 会重置 PATH，先找到 PostgreSQL 的 bin 目录
BIN=$(dirname "$(command -v pg_ctl)")

mkdir -p "$WORK"
chown "$OSUSER" "$WORK"

# 1. 准备一个全零的“磁盘镜像”（稀疏文件，不真正占用空间）
if [ ! -e "$IMG" ]; then
    dd if=/dev/zero of="$IMG" bs=1M count=0 seek="$SIZE_MB" status=none
fi

# 2. 把它挂成 loop 块设备（需要 root 和 --privileged）
DEV=$(losetup -j "$IMG" | cut -d: -f1 | head -1)
if [ -z "$DEV" ]; then
    DEV=$(losetup -f --show "$IMG" 2>/dev/null || true)
fi
if [ -z "$DEV" ]; then
    echo "!! losetup 不可用（容器没有加 --privileged？），改用普通文件作为 HSM 设备"
    DEV=$IMG
fi
chown "$OSUSER" "$DEV" "$IMG"
echo ">> HSM 设备: $DEV"

# 3. initdb（只在第一次）
if [ ! -s "$PGDATA/PG_VERSION" ]; then
    mkdir -p "$PGDATA"
    chown "$OSUSER" "$PGDATA"
    su "$OSUSER" -c "$BIN/initdb -D '$PGDATA' -A trust -U postgres" >/dev/null
fi

# 4. 打开 HSM
sed -i '/^hsm_device/d' "$PGDATA/postgresql.conf"
echo "hsm_device = '$DEV'" >> "$PGDATA/postgresql.conf"

# 5. 启动并演示
su "$OSUSER" -c "$BIN/pg_ctl -D '$PGDATA' -l '$PGDATA/log.txt' -w start" >/dev/null
grep -E "HSM" "$PGDATA/log.txt" | tail -2
"$BIN/psql" -X -U postgres -h /tmp -f "${HSM_DEMO_SQL:-/usr/local/share/hsm-demo.sql}"

VB=$("$BIN/psql" -X -At -U postgres -h /tmp -c \
     "SELECT device_start_block FROM pg_hsm_extents() e JOIN pg_class c USING (relfilenode) WHERE c.relname = 'vage'")
echo ">> 直接从块设备 $DEV 的第 $VB 块读出 vage 表的数据页（页尾就是元组）："
dd if="$DEV" bs=8192 skip="$VB" count=1 status=none | od -A d -c | tail -6

if [ -t 0 ] && [ -z "$HSM_NO_SHELL" ]; then
    echo ">> 进入 shell，可以用: psql -U postgres -h /tmp"
    exec bash
fi
