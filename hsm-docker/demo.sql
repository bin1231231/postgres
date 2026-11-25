\echo '==== HSM 设备概况 ===='
\x on
SELECT * FROM pg_hsm_status();
\x off

\echo '==== PPT 中的 vage 示例：表存放在块设备上 ===='
DROP TABLE IF EXISTS vage;
CREATE TABLE vage (id1 int, c1 varchar(30), id2 int, c2 varchar(30));
INSERT INTO vage SELECT generate_series(1,5), 'AAAAAAAA'||generate_series(1,5),
                        generate_series(1,5)+100, 'aaaaaaaa'||generate_series(1,5);
SELECT * FROM vage;
SELECT relname, relfilenode, pg_relation_filepath(oid) AS md_path_would_be
FROM pg_class WHERE relname = 'vage';

\echo '==== 大一点的表：区随文件增长而变大（64KB -> 512KB -> 4MB） ===='
DROP TABLE IF EXISTS big;
CREATE TABLE big AS SELECT g AS id, md5(g::text) AS v FROM generate_series(1, 200000) g;
CREATE INDEX big_id ON big (id);
SELECT c.relname, f.fork, f.nblocks, f.nextents, f.allocated_blocks
FROM pg_hsm_files() f JOIN pg_class c USING (relfilenode)
ORDER BY 1, 2;
SELECT extent_no, logical_start_block, device_start_block, nblocks
FROM pg_hsm_extents() e JOIN pg_class c USING (relfilenode)
WHERE c.relname = 'big' AND fork = 'main';
SELECT pg_size_pretty(pg_relation_size('big')) AS heap,
       pg_size_pretty(pg_total_relation_size('big')) AS total;

\echo '==== CHECKPOINT：把 Shared Buffer 中的脏页写到块设备上 ===='
CHECKPOINT;
