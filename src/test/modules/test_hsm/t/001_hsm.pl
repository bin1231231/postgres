
# Tests for the HSM block-device storage manager (hsm.c).
#
# Each node gets its own sparse file as the "block device".

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tmp = PostgreSQL::Test::Utils::tempdir;

# Create a sparse, all-zero "device" of the given size in MB.
sub make_device
{
	my ($name, $mb) = @_;
	my $path = "$tmp/$name.img";
	open(my $fh, '>', $path) or die "could not create $path: $!";
	truncate($fh, $mb * 1024 * 1024) or die "could not size $path: $!";
	close($fh);
	return $path;
}

sub hsm_node
{
	my ($name, $mb, %opts) = @_;
	my $dev = make_device($name, $mb);
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init;
	$node->append_conf('postgresql.conf', "hsm_device = '$dev'");
	$node->append_conf('postgresql.conf', $opts{extra}) if $opts{extra};
	$node->start;
	return ($node, $dev);
}

###########################################################################
# 1. Basic operation
###########################################################################
# The test framework sets fsync = off; the main node runs with fsync = on so
# that the deferred reuse of freed extents is exercised (with fsync = off
# freed space is reused immediately, which the other nodes cover).
my ($node, $dev) = hsm_node('main', 512, extra => 'fsync = on');

is($node->safe_psql('postgres', 'SELECT enabled FROM pg_hsm_status()'),
	't', 'HSM is enabled');
like(slurp_file($node->logfile),
	qr/formatted HSM device/, 'new all-zero device was formatted');

$node->safe_psql('postgres', q{
	CREATE EXTENSION amcheck;
	CREATE TABLE t (id int PRIMARY KEY, v text);
	INSERT INTO t SELECT g, md5(g::text) FROM generate_series(1, 100000) g;
	CREATE INDEX t_hash ON t USING hash (v);
	CREATE INDEX t_v ON t (v);
});

my $relpath = $node->safe_psql('postgres', "SELECT pg_relation_filepath('t')");
ok(!-e $node->data_dir . "/$relpath", 'no data file for an HSM relation');

is( $node->safe_psql(
		'postgres', q{
	SELECT pg_relation_size('t') = f.nblocks * 8192
	FROM pg_hsm_files() f WHERE f.relfilenode = pg_relation_filenode('t')
	  AND f.fork = 'main'}),
	't',
	'pg_relation_size() reports the HSM size');

ok( $node->safe_psql(
		'postgres', q{
	SELECT count(*) FROM pg_hsm_extents()
	WHERE relfilenode = pg_relation_filenode('t') AND fork = 'main'}) > 1,
	'relation uses several extents');

my $sum_before = $node->safe_psql('postgres',
	"SELECT count(*), md5(string_agg(v, ',' ORDER BY id)) FROM t");
is( $node->safe_psql(
		'postgres', "SELECT count(*) FROM t WHERE v = md5('4242')"),
	'1', 'hash index (sparse extension) works');

###########################################################################
# 2. Clean restart and crash recovery
###########################################################################
$node->restart;
is( $node->safe_psql('postgres',
		"SELECT count(*), md5(string_agg(v, ',' ORDER BY id)) FROM t"),
	$sum_before, 'data survives a clean restart');

# committed work after the last checkpoint + an uncommitted transaction
$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres', q{
	INSERT INTO t SELECT g, md5(g::text) FROM generate_series(100001, 150000) g;
	UPDATE t SET v = 'updated' WHERE id % 100 = 0;
	CREATE TABLE t_after_ckpt AS SELECT g FROM generate_series(1, 5000) g;
	CREATE UNLOGGED TABLE u (id int);
	INSERT INTO u SELECT generate_series(1, 1000);
});
# keep a transaction open with uncommitted inserts while we crash
my $bg = $node->background_psql('postgres');
$bg->query_safe('BEGIN');
$bg->query_safe(
	"INSERT INTO t SELECT g, 'uncommitted' FROM generate_series(200001, 210000) g");
$node->stop('immediate');
eval { $bg->quit; };
$node->start;

is( $node->safe_psql(
		'postgres',
		"SELECT count(*), count(*) FILTER (WHERE v = 'updated') FROM t"),
	'150000|1500',
	'committed changes recovered, uncommitted ones gone');
is($node->safe_psql('postgres', 'SELECT count(*) FROM t_after_ckpt'),
	'5000', 'table created after the checkpoint recovered');
is($node->safe_psql('postgres', 'SELECT count(*) FROM u'),
	'0', 'unlogged table was reset by crash recovery');
$node->safe_psql('postgres', 'INSERT INTO u VALUES (1)');
is($node->safe_psql('postgres', 'SELECT count(*) FROM u'),
	'1', 'unlogged table usable after reset');

$node->safe_psql('postgres', q{
	SELECT bt_index_check(c.oid, true) FROM pg_class c
	WHERE c.relname IN ('t_pkey', 't_v')});
pass('amcheck finds no corruption after crash recovery');

###########################################################################
# 3. DROP / TRUNCATE: tombstones and space reclamation
###########################################################################
my $fn = $node->safe_psql('postgres', "SELECT pg_relation_filenode('t')");
my $used_before = $node->safe_psql('postgres', 'SELECT used_units FROM pg_hsm_status()');
$node->safe_psql('postgres', 'DROP TABLE t');

is( $node->safe_psql(
		'postgres',
		"SELECT tombstone, nblocks FROM pg_hsm_files() WHERE relfilenode = $fn AND fork = 'main'"),
	't|0',
	'dropped main fork is kept as an empty tombstone');
ok( $node->safe_psql('postgres', 'SELECT pending_units FROM pg_hsm_status()') > 0,
	'freed space waits for the next checkpoint');

$node->safe_psql('postgres', 'CHECKPOINT');
is( $node->safe_psql(
		'postgres', "SELECT count(*) FROM pg_hsm_files() WHERE relfilenode = $fn"),
	'0', 'tombstone removed after checkpoint');
ok( $node->safe_psql('postgres', 'SELECT used_units FROM pg_hsm_status()') <
	  $used_before,
	'space reclaimed after checkpoint');
is($node->safe_psql('postgres', 'SELECT pending_units FROM pg_hsm_status()'),
	'0', 'no pending space left');

# VACUUM truncation releases trailing extents
$node->safe_psql('postgres', q{
	CREATE TABLE s AS SELECT g AS id, repeat('x', 500) AS pad FROM generate_series(1, 20000) g;
	DELETE FROM s WHERE id > 1000;
	VACUUM s;
});
is( $node->safe_psql(
		'postgres', q{
	SELECT nblocks < 100 AND allocated_blocks < 200 FROM pg_hsm_files()
	WHERE relfilenode = pg_relation_filenode('s') AND fork = 'main'}),
	't',
	'VACUUM truncation shrinks the relation and its extents');

# rewrite (new relfilenode)
$node->safe_psql('postgres', 'VACUUM FULL s');
is($node->safe_psql('postgres', 'SELECT count(*), sum(id) FROM s'),
	'1000|500500', 'VACUUM FULL keeps the data');

###########################################################################
# 4. Databases
###########################################################################
$node->safe_psql('postgres', 'CREATE DATABASE tmpl');
$node->safe_psql('tmpl',
	'CREATE TABLE tt AS SELECT generate_series(1, 1000) AS g');
$node->safe_psql('postgres', 'CREATE DATABASE copy1 TEMPLATE tmpl');
is($node->safe_psql('copy1', 'SELECT sum(g) FROM tt'),
	'500500', 'CREATE DATABASE (WAL_LOG) copies HSM relations');

my ($ret, $stdout, $stderr) = $node->psql('postgres',
	'CREATE DATABASE copy2 TEMPLATE tmpl STRATEGY = FILE_COPY');
like($stderr, qr/has relations stored on the HSM device/,
	'FILE_COPY from a template with HSM relations is rejected');

my $dboid = $node->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'copy1'");
ok( $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_hsm_files() WHERE dbnode = $dboid") > 0,
	'copied database has forks on the device');
$node->safe_psql('postgres', 'DROP DATABASE copy1');
is( $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_hsm_files() WHERE dbnode = $dboid"),
	'0', 'DROP DATABASE removes its HSM forks');

mkdir "$tmp/ts";
$node->safe_psql('postgres', "CREATE TABLESPACE ts LOCATION '$tmp/ts'");
($ret, $stdout, $stderr) =
  $node->psql('postgres', 'ALTER DATABASE tmpl SET TABLESPACE ts');
like($stderr, qr/has relations stored on the HSM device/,
	'ALTER DATABASE SET TABLESPACE is rejected');
$node->safe_psql('postgres',
	'CREATE TABLE in_ts TABLESPACE ts AS SELECT 1 AS x');
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_hsm_files() WHERE relfilenode = pg_relation_filenode('in_ts')"),
	'0', 'relations in other tablespaces stay with md.c');

$node->stop;

###########################################################################
# 5. Disk full, then recovery of space
###########################################################################
my ($small, $sdev) = hsm_node('small', 4);
($ret, $stdout, $stderr) = $small->psql('postgres', q{
	CREATE TABLE fill (pad text);
	INSERT INTO fill SELECT repeat('x', 1000) FROM generate_series(1, 100000)});
like($stderr, qr/no space left on device/, 'device full raises an ERROR');
is($small->safe_psql('postgres', 'SELECT 1'), '1', 'server survives a full device');
$small->safe_psql('postgres', 'DROP TABLE IF EXISTS fill');
$small->safe_psql('postgres',
	"CREATE TABLE again AS SELECT repeat('y', 1000) AS pad FROM generate_series(1, 500)");
is($small->safe_psql('postgres', 'SELECT count(*) FROM again'),
	'500', 'space of a dropped table is reused');
$small->stop;

###########################################################################
# 6. Tiny 1MB device (the example from the course slides)
###########################################################################
my ($tiny) = hsm_node('tiny', 1);
$tiny->safe_psql('postgres',
	'CREATE TABLE vage (id1 int, c1 varchar(30)); INSERT INTO vage VALUES (1, \'AAAAAAAA1\')');
is($tiny->safe_psql('postgres', 'SELECT c1 FROM vage'),
	'AAAAAAAA1', 'a 1MB device works');
$tiny->stop;

###########################################################################
# 7. Enabling HSM on a cluster that already has md tables
###########################################################################
my $mixed = PostgreSQL::Test::Cluster->new('mixed');
$mixed->init;
$mixed->start;
$mixed->safe_psql('postgres',
	'CREATE TABLE old_md AS SELECT generate_series(1, 100) AS g');
$mixed->stop;
my $mdev = make_device('mixed', 64);
$mixed->append_conf('postgresql.conf', "hsm_device = '$mdev'");
$mixed->start;
$mixed->safe_psql('postgres',
	'CREATE TABLE new_hsm AS SELECT generate_series(1, 100) AS g');
is( $mixed->safe_psql(
		'postgres', q{
	SELECT (SELECT sum(g) FROM old_md),
	       (SELECT count(*) FROM pg_hsm_files() WHERE relfilenode = pg_relation_filenode('old_md')),
	       (SELECT count(*) FROM pg_hsm_files() WHERE relfilenode = pg_relation_filenode('new_hsm'))}
	),
	'5050|0|1',
	'existing md table stays in md, new table goes to HSM');
$mixed->stop;

###########################################################################
# 8. Refuse a device that is not ours
###########################################################################
my $bad = PostgreSQL::Test::Cluster->new('bad');
$bad->init;
my $bdev = make_device('bad', 16);
open(my $fh, '+<', $bdev) or die;
print $fh 'not an HSM device';
close($fh);
$bad->append_conf('postgresql.conf', "hsm_device = '$bdev'");
ok(!$bad->start(fail_ok => 1), 'server refuses to start on a foreign device');
like(slurp_file($bad->logfile), qr/is not an HSM device/,
	'and says why');

###########################################################################
# 9. HSM off by default
###########################################################################
my $plain = PostgreSQL::Test::Cluster->new('plain');
$plain->init;
$plain->start;
is($plain->safe_psql('postgres', 'SELECT enabled FROM pg_hsm_status()'),
	'f', 'HSM is disabled by default');
$plain->stop;

done_testing();
