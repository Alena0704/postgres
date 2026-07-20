# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that vacuum statistics entries do not outlive the objects they
# describe.
#
# An entry that survives its relation would occupy shared memory forever,
# be written to the statistics file on shutdown and read back on startup,
# and eventually be reused by whatever relation the recycled OID lands on.
# ext_vacuum_statistics.shared_memory_size() grows and shrinks with the
# number of entries, so it lets us observe all of that from SQL.  The last
# part checks that a relation created on a recycled OID does not inherit the
# statistics of the relation that had it before.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('vacstat_gc');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
shared_preload_libraries = 'ext_vacuum_statistics'
autovacuum = off
});
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION ext_vacuum_statistics');

sub stats_size
{
	return $node->safe_psql('postgres',
		'SELECT ext_vacuum_statistics.shared_memory_size()');
}

# Wait for the statistics to shrink back to $want, which happens once the
# dropping transaction has committed and the entries have been released.
sub wait_for_size
{
	my ($want, $what) = @_;

	$node->poll_query_until('postgres',
		"SELECT ext_vacuum_statistics.shared_memory_size() <= $want")
	  or die "timed out waiting for $what";
	return;
}

# A table that stays for the whole test.  Whatever statistics exist once it
# has been vacuumed are exactly what has to be left after everything else
# is gone.  Comparing against that, rather than counting entries, keeps the
# test independent of how large the entries of each kind are.
$node->safe_psql(
	'postgres', q{
CREATE TABLE gc_keep (id int);
INSERT INTO gc_keep SELECT generate_series(1, 100);
DELETE FROM gc_keep;
VACUUM gc_keep;
});
my $kept = stats_size();
cmp_ok($kept, '>', 0, 'vacuuming a table created statistics entries');

# Vacuum a handful of tables, each with an index, so that both entry kinds
# are populated.
my @tables = map { "gc_tab$_" } (1 .. 20);

$node->safe_psql('postgres',
	join('', map { qq[
CREATE TABLE $_ (id int);
CREATE INDEX ON $_ (id);
INSERT INTO $_ SELECT generate_series(1, 100);
DELETE FROM $_;] } @tables));
$node->safe_psql('postgres', 'VACUUM ' . join(', ', @tables));

my $populated = stats_size();
cmp_ok($populated, '>', $kept,
	'vacuuming tables and indexes created more statistics entries');

# A rolled back DROP must keep the statistics.
$node->safe_psql('postgres',
	'BEGIN; DROP TABLE gc_tab1; ROLLBACK;');
is(stats_size(), $populated, 'rolled back DROP TABLE kept the statistics');

# Dropping the tables takes their entries, and their indexes', with them.
$node->safe_psql('postgres', 'DROP TABLE ' . join(', ', @tables));
wait_for_size($kept, 'DROP TABLE to release the statistics');
is(stats_size(), $kept, 'DROP TABLE dropped the statistics entries');

# Nothing may come back from the statistics file either.
$node->restart;
is(stats_size(), $kept, 'no dropped entries survived a restart');

# The same has to hold for a whole database going away.
$node->safe_psql('postgres', 'CREATE DATABASE vacstat_gc_db');
$node->safe_psql(
	'vacstat_gc_db', q{
CREATE TABLE gc_dbtab (id int);
CREATE INDEX gc_dbidx ON gc_dbtab (id);
INSERT INTO gc_dbtab SELECT generate_series(1, 100);
DELETE FROM gc_dbtab;
VACUUM gc_dbtab;
});
cmp_ok(stats_size(), '>', $kept,
	'vacuum in another database created statistics entries');

$node->safe_psql('postgres', 'DROP DATABASE vacstat_gc_db');
wait_for_size($kept, 'DROP DATABASE to release the statistics');
is(stats_size(), $kept, 'DROP DATABASE dropped the statistics entries');

# A relation created on the OID of an earlier one must not inherit its
# statistics.  DROP never leaves an entry behind, and OIDs are only handed
# out again after a wraparound, so fake the situation: take a vacuumed table
# out of the catalogs behind the back of DROP, which leaves its entry
# orphaned, then create a new table with the very same OIDs in binary upgrade
# mode.
$node->safe_psql(
	'postgres', q{
CREATE TABLE gc_reuse (id int);
INSERT INTO gc_reuse SELECT generate_series(1, 100);
DELETE FROM gc_reuse;
VACUUM gc_reuse;
});
my ($relid, $reltype, $typarray) = split /\|/,
  $node->safe_psql(
	'postgres', q{
SELECT c.oid, c.reltype, t.typarray
  FROM pg_class c JOIN pg_type t ON t.oid = c.reltype
 WHERE c.relname = 'gc_reuse'});
is( $node->safe_psql(
		'postgres',
		"SELECT tuples_deleted FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relid = $relid"
	),
	'100',
	'the table whose OID gets recycled has statistics');

$node->safe_psql(
	'postgres', qq{
SET allow_system_table_mods = on;
DELETE FROM pg_depend
 WHERE (classid = 'pg_class'::regclass AND objid = $relid)
    OR (refclassid = 'pg_class'::regclass AND refobjid = $relid)
    OR (classid = 'pg_type'::regclass AND objid IN ($reltype, $typarray))
    OR (refclassid = 'pg_type'::regclass AND refobjid IN ($reltype, $typarray));
DELETE FROM pg_attribute WHERE attrelid = $relid;
DELETE FROM pg_type WHERE oid IN ($reltype, $typarray);
DELETE FROM pg_class WHERE oid = $relid;
});

# Binary upgrade mode is a postmaster switch, so start the node by hand.
$node->stop;
command_ok(
	[
		'pg_ctl',
		'--pgdata' => $node->data_dir,
		'--log' => $node->logfile,
		'--options' => '-b',
		'--wait', 'start'
	],
	'started in binary upgrade mode');

# The relfilenumber only has to be unused; the old file is still on disk.
my ($ret, $stdout, $stderr) = $node->psql(
	'postgres', qq{
SELECT binary_upgrade_set_next_heap_pg_class_oid('$relid'::oid);
SELECT binary_upgrade_set_next_heap_relfilenode('3000000000'::oid);
SELECT binary_upgrade_set_next_pg_type_oid('$reltype'::oid);
SELECT binary_upgrade_set_next_array_pg_type_oid('$typarray'::oid);
CREATE TABLE gc_reuse_new (id int);
});
is($ret, 0, 'created a table on the recycled OID');
like(
	$stderr,
	qr/resetting existing statistics for kind ext_vacuum_statistics_relation/,
	'creating the table reset the orphaned statistics entry');

command_ok(
	[
		'pg_ctl',
		'--pgdata' => $node->data_dir,
		'--mode' => 'fast',
		'--wait', 'stop'
	],
	'stopped binary upgrade mode');
$node->start;

is( $node->safe_psql(
		'postgres', "SELECT oid FROM pg_class WHERE relname = 'gc_reuse_new'"),
	$relid,
	'the new table got the recycled OID');
is( $node->safe_psql(
		'postgres', q{
SELECT coalesce((SELECT tuples_deleted
                   FROM ext_vacuum_statistics.pg_stats_vacuum_tables
                  WHERE relname = 'gc_reuse_new'), 0)}),
	'0',
	'the new table did not inherit the statistics of the old one');

$node->stop;

done_testing();
