# Copyright (c) 2026, PostgreSQL Global Development Group

# Test database-level filtering of extended vacuum statistics.  The
# track_vacuum_statistics GUC can be attached to individual databases with
# ALTER DATABASE ... SET, restricting the collection to vacuums running in
# specific databases; relations vacuumed in an opted-out database must show
# up neither in the per-relation views nor in the per-database aggregate.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq[
autovacuum = off
track_vacuum_statistics = on
]);
$node->start;

$node->safe_psql('postgres', 'CREATE DATABASE db_tracked');
$node->safe_psql('postgres', 'CREATE DATABASE db_filtered');
$node->safe_psql('postgres',
	'ALTER DATABASE db_filtered SET track_vacuum_statistics = off');

# Run the same workload in both databases.
foreach my $db ('db_tracked', 'db_filtered')
{
	$node->safe_psql(
		$db, qq[
CREATE TABLE vacstat_t (id int PRIMARY KEY, v text)
  WITH (autovacuum_enabled = off);
INSERT INTO vacstat_t SELECT g, repeat('x', 20) FROM generate_series(1, 1000) g;
DELETE FROM vacstat_t WHERE id % 2 = 0;
]);
	$node->safe_psql($db, 'VACUUM vacstat_t;');
	$node->safe_psql($db, 'SELECT pg_stat_force_next_flush();');
}

is( $node->safe_psql(
		'db_tracked',
		"SELECT count(*) FROM pg_stat_vacuum_tables WHERE relname = 'vacstat_t'"
	),
	'1',
	'vacuum statistics collected in the tracked database');

is( $node->safe_psql(
		'db_filtered',
		"SELECT count(*) FROM pg_stat_vacuum_tables WHERE relname = 'vacstat_t'"
	),
	'0',
	'no relation statistics in the opted-out database');

# The per-database aggregate follows the same rule.
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_stat_vacuum_database WHERE dbname = 'db_tracked'"
	),
	'1',
	'database aggregate present for the tracked database');

is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_stat_vacuum_database WHERE dbname = 'db_filtered'"
	),
	'0',
	'no database aggregate for the opted-out database');

done_testing();
