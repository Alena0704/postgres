# Copyright (c) 2025 PostgreSQL Global Development Group
#
# Test GUC parameters for ext_vacuum_statistics extension:
#   vacuum_statistics.enabled
#   vacuum_statistics.track (all, databases, relations)
#   vacuum_statistics.track_relations (all, system, user)
#   vacuum_statistics.track_databases
#   vacuum_statistics.track_relations_list
#   vacuum_statistics.collect (space-separated: buffers, wal, tuples, timing, all)

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

#------------------------------------------------------------------------------
# Test cluster setup
#------------------------------------------------------------------------------

my $node = PostgreSQL::Test::Cluster->new('ext_stat_vacuum_gucs');
$node->init;

$node->append_conf('postgresql.conf', q{
    shared_preload_libraries = 'ext_vacuum_statistics'
    log_min_messages = notice
});

$node->start;

#------------------------------------------------------------------------------
# Database creation and initialization
#------------------------------------------------------------------------------

$node->safe_psql('postgres', q{
    CREATE DATABASE statistic_vacuum_gucs;
});

my $dbname = 'statistic_vacuum_gucs';

$node->safe_psql($dbname, q{
    CREATE EXTENSION ext_vacuum_statistics;
    CREATE TABLE guc_test (x int PRIMARY KEY)
        WITH (autovacuum_enabled = off);
    INSERT INTO guc_test SELECT x FROM generate_series(1, 100) AS g(x);
    ANALYZE guc_test;
});

# Get OIDs for filtering tests
my $dboid = $node->safe_psql($dbname, q{SELECT oid FROM pg_database WHERE datname = current_database()});
my $reloid = $node->safe_psql($dbname, q{SELECT oid FROM pg_class WHERE relname = 'guc_test'});

#------------------------------------------------------------------------------
# Helper: reset stats and run vacuum (all in one session so GUCs persist)
# Optional $opts is hash with keys: gucs (array of "name=value"), table
#------------------------------------------------------------------------------

sub reset_and_vacuum {
    my ($db, $table, $opts) = @_;
    $table ||= 'guc_test';
    my $gucs = $opts && $opts->{gucs} ? $opts->{gucs} : [];
    my $sql = join("\n", (map { "SET $_;" } @$gucs),
        "SELECT ext_vacuum_statistics.vacuum_statistics_reset();",
        "VACUUM $table;",
        "SELECT pg_stat_force_next_flush();");
    $node->safe_psql($db, $sql);
    sleep(0.1);
}

#------------------------------------------------------------------------------
# Test 1: vacuum_statistics.enabled
#------------------------------------------------------------------------------
subtest 'vacuum_statistics.enabled' => sub {
    reset_and_vacuum($dbname);

    # Default: enabled - should have stats
    my $count = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    ok($count > 0, 'stats collected when enabled (default)');

    # Disable, reset and vacuum in same session
    reset_and_vacuum($dbname, 'guc_test', { gucs => ['vacuum_statistics.enabled = off'] });

    $count = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    is($count, 0, 'no stats when disabled');
};

#------------------------------------------------------------------------------
# Test 2: vacuum_statistics.track (databases only, relations only)
#------------------------------------------------------------------------------
subtest 'vacuum_statistics.track' => sub {
    # Run vacuum and checks in same session so stats are visible.
    # Create dead tuples so vacuum does work and produces stats.
    $node->safe_psql($dbname, q{ DELETE FROM guc_test WHERE x % 2 = 0; });
    my $result = $node->safe_psql($dbname, qq{
        SET vacuum_statistics.track = 'databases';
        SELECT ext_vacuum_statistics.vacuum_statistics_reset();
        VACUUM guc_test;
        SELECT pg_stat_force_next_flush();
        SELECT
            (SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test') AS rel_cnt,
            (SELECT COALESCE(db_blks_read, 0) > 0 FROM ext_vacuum_statistics.pg_stats_vacuum_database WHERE dboid = $dboid) AS db_has;
    });
    $result =~ s/\s*\|\s*/ /g;
    my ($rel_count, $db_has_stats) = split /\s+/, $result;
    is($rel_count, 0, 'track=databases: no relation stats');
    is($db_has_stats, 't', 'track=databases: database stats collected');

    $result = $node->safe_psql($dbname, qq{
        SET vacuum_statistics.track = 'relations';
        SELECT ext_vacuum_statistics.vacuum_statistics_reset();
        VACUUM guc_test;
        SELECT pg_stat_force_next_flush();
        SELECT
            (SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test') AS rel_cnt,
            (SELECT COALESCE(db_blks_read, 0) > 0 FROM ext_vacuum_statistics.pg_stats_vacuum_database WHERE dboid = $dboid) AS db_has;
    });
    $result =~ s/\s*\|\s*/ /g;
    ($rel_count, my $db_has_stats_rel) = split /\s+/, $result;
    ok($rel_count > 0, 'track=relations: relation stats collected');
    is($db_has_stats_rel, 'f', 'track=relations: no database stats');
};

#------------------------------------------------------------------------------
# Test 3: vacuum_statistics.track_relations (system, user)
#------------------------------------------------------------------------------
subtest 'vacuum_statistics.track_relations' => sub {
    # track_relations = 'user' - only user tables
    $node->safe_psql($dbname, qq{
        SET vacuum_statistics.track = 'relations';
        SET vacuum_statistics.track_relations = 'user';
        SELECT ext_vacuum_statistics.vacuum_statistics_reset();
        VACUUM guc_test;
        VACUUM pg_class;
        SELECT pg_stat_force_next_flush();
    });
    sleep(0.1);

    my $user_rel = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    my $sys_rel = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'pg_class'");
    ok($user_rel > 0, 'track_relations=user: user table stats collected');
    is($sys_rel, 0, 'track_relations=user: system table stats not collected');

    # track_relations = 'system' - only system tables
    $node->safe_psql($dbname, qq{
        SET vacuum_statistics.track = 'relations';
        SET vacuum_statistics.track_relations = 'system';
        SELECT ext_vacuum_statistics.vacuum_statistics_reset();
        VACUUM guc_test;
        VACUUM pg_class;
        SELECT pg_stat_force_next_flush();
    });
    sleep(0.1);

    $user_rel = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    $sys_rel = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'pg_class'");
    is($user_rel, 0, 'track_relations=system: user table stats not collected');
    ok($sys_rel > 0, 'track_relations=system: system table stats collected');
};

#------------------------------------------------------------------------------
# Test 4: vacuum_statistics.track_databases (OID filter)
#------------------------------------------------------------------------------
subtest 'vacuum_statistics.track_databases' => sub {
    # Filter to a different DB OID - should collect nothing for current db
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.track_databases = '1'"] });

    my $rel_count = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    is($rel_count, 0, 'track_databases=1: no stats for other database');

    # Filter to current DB OID
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.track_databases = '$dboid'"] });

    $rel_count = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    ok($rel_count > 0, 'track_databases=current: stats collected');
};

#------------------------------------------------------------------------------
# Test 5: vacuum_statistics.track_relations_list (OID filter)
#------------------------------------------------------------------------------
subtest 'vacuum_statistics.track_relations_list' => sub {
    # Filter to a non-existent relation OID
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.track_relations_list = '99999'"] });

    my $rel_count = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    is($rel_count, 0, 'track_relations_list=99999: no stats for excluded relation');

    # Filter to current table OID
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.track_relations_list = '$reloid'"] });

    $rel_count = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    ok($rel_count > 0, 'track_relations_list=reloid: stats collected for table');
};

#------------------------------------------------------------------------------
# Test 6: vacuum_statistics.collect_mask
#------------------------------------------------------------------------------
subtest 'vacuum_statistics.collect_mask' => sub {
    # collect = buffers: only buffers (total_blks_read, blk_read_time, etc.)
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.collect = 'buffers'"] });

    my $row = $node->safe_psql($dbname,
        "SELECT wal_records, tuples_deleted, delay_time, total_time
         FROM ext_vacuum_statistics.pg_stats_vacuum_tables
         WHERE relname = 'guc_test'");
    my ($wal, $tuples, $delay, $total) = split /\|/, $row;
    $wal =~ s/\s//g;
    $tuples =~ s/\s//g;
    $delay =~ s/\s//g;
    $total =~ s/\s//g;
    is($wal, 0, 'collect=buffers: wal_records zeroed');
    is($tuples, 0, 'collect=buffers: tuples_deleted zeroed');
    is($delay, 0, 'collect=buffers: delay_time zeroed');
    is($total, 0, 'collect=buffers: total_time zeroed');

    # collect = wal: only WAL
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.collect = 'wal'"] });

    $row = $node->safe_psql($dbname,
        "SELECT total_blks_read, blk_read_time, tuples_deleted, delay_time
         FROM ext_vacuum_statistics.pg_stats_vacuum_tables
         WHERE relname = 'guc_test'");
    my ($blks, $blk_read, $tup, $dly) = split /\|/, $row;
    $blks =~ s/\s//g;
    $blk_read =~ s/\s//g;
    $tup =~ s/\s//g;
    $dly =~ s/\s//g;
    is($blks, 0, 'collect=wal: total_blks_read zeroed');
    is($blk_read, 0, 'collect=wal: blk_read_time zeroed');
    is($tup, 0, 'collect=wal: tuples_deleted zeroed');
    is($dly, 0, 'collect=wal: delay_time zeroed');

    # collect = wal tuples timing: all except buffers
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.collect = 'wal tuples timing'"] });

    $row = $node->safe_psql($dbname,
        "SELECT total_blks_read, total_blks_hit, blk_read_time, blk_write_time
         FROM ext_vacuum_statistics.pg_stats_vacuum_tables
         WHERE relname = 'guc_test'");
    my ($r1, $r2, $r3, $r4) = split /\|/, $row;
    $r1 =~ s/\s//g;
    $r2 =~ s/\s//g;
    $r3 =~ s/\s//g;
    $r4 =~ s/\s//g;
    is($r1, 0, 'collect=wal tuples timing: total_blks_read zeroed');
    is($r2, 0, 'collect=wal tuples timing: total_blks_hit zeroed');
    is($r3, 0, 'collect=wal tuples timing: blk_read_time zeroed');
    is($r4, 0, 'collect=wal tuples timing: blk_write_time zeroed');
};

$node->stop;

done_testing();
