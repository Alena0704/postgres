# Copyright (c) 2025 PostgreSQL Global Development Group
#
# Test GUC parameters for ext_vacuum_statistics extension:
#   vacuum_statistics.enabled
#   vacuum_statistics.track (all, databases, relations)
#   vacuum_statistics.track_relations (all, system, user)
#   add/remove_track_database, add/remove_track_relation, track_*_from_list
#   vacuum_statistics.collect (space-separated: buffers, wal, general, timing, all)

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
# Reset stats and run vacuum (all in one session so GUCs persist)
#------------------------------------------------------------------------------

sub reset_and_vacuum {
    my ($db, $table, $opts) = @_;
    $table ||= 'guc_test';
    my $gucs = $opts && $opts->{gucs} ? $opts->{gucs} : [];
    my $sql = join("\n", (map { "SET $_;" } @$gucs),
        "SELECT ext_vacuum_statistics.vacuum_statistics_reset();",
        "VACUUM $table;");
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
    ok($count > 0, 'stats collected when enabled');

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
    # track only db stats, no relation stats
    my $r1 = $node->safe_psql($dbname, qq{
        SET vacuum_statistics.track = 'databases';
        SELECT ext_vacuum_statistics.vacuum_statistics_reset();
        TRUNCATE guc_test;
        INSERT INTO guc_test SELECT x FROM generate_series(1, 100) AS g(x);
        DELETE FROM guc_test;
        VACUUM guc_test;
    });
    my $db_has_dbs = $node->safe_psql($dbname,
                                   "SELECT COALESCE(SUM(db_blks_hit), 0) FROM ext_vacuum_statistics.pg_stats_vacuum_database WHERE dboid = $dboid");
    my $rel_dbs = $node->safe_psql($dbname,
                                "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    is($rel_dbs, 0, 'track=databases: no relation stats');
    ok($db_has_dbs > 0, 'track=databases: database stats collected');

    # track only relation stats, no db stats
    my $r2 = $node->safe_psql($dbname, qq{
        SET vacuum_statistics.track = 'relations';
        SELECT ext_vacuum_statistics.vacuum_statistics_reset();
        TRUNCATE guc_test;
        INSERT INTO guc_test SELECT x FROM generate_series(1, 100) AS g(x);
        DELETE FROM guc_test;
        VACUUM guc_test;
    });
    my $db_has_rels = $node->safe_psql($dbname,
                                   "SELECT COALESCE(SUM(db_blks_hit), 0) > 0 FROM ext_vacuum_statistics.pg_stats_vacuum_database WHERE dboid = $dboid");
    my $rel_rels = $node->safe_psql($dbname,
                                "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    ok($rel_rels > 0, 'track=relations: relation stats collected');
    is($db_has_rels, 'f', 'track=relations: no database stats');
};

#------------------------------------------------------------------------------
# Test 3: vacuum_statistics.track_relations (system, user)
#------------------------------------------------------------------------------
subtest 'vacuum_statistics.track_relations' => sub {
    # track_relations - only user tables
    $node->safe_psql($dbname, qq{
        SET vacuum_statistics.track = 'relations';
        SET vacuum_statistics.track_relations = 'user';
        SELECT ext_vacuum_statistics.vacuum_statistics_reset();
        VACUUM guc_test;
        VACUUM pg_class;
    });
    sleep(0.1);

    my $user_rel = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    my $sys_rel = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'pg_class'");
    ok($user_rel > 0, 'track_relations=user: user table stats collected');
    is($sys_rel, 0, 'track_relations=user: system table stats not collected');

    # track_relations - only system tables
    $node->safe_psql($dbname, qq{
        SET vacuum_statistics.track = 'relations';
        SET vacuum_statistics.track_relations = 'system';
        SELECT ext_vacuum_statistics.vacuum_statistics_reset();
        VACUUM guc_test;
        VACUUM pg_class;
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
# Test 4: track_databases (via add/remove_track_database)
#------------------------------------------------------------------------------
subtest 'track_databases (add/remove)' => sub {
    $node->safe_psql($dbname, "SELECT ext_vacuum_statistics.remove_track_database($dboid)");
    $node->safe_psql($dbname, "SELECT ext_vacuum_statistics.add_track_database(1)");
    my $track = $node->safe_psql($dbname, "SELECT track_kind, count(*) FROM ext_vacuum_statistics.track_list() GROUP BY track_kind");
    like($track, qr/database\s*\|\s*1/, 'track_list: 1 database after add_track_database(1)');
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.track_databases_from_list = on"] });

    my $rel_count = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    is($rel_count, 0, 'only db 1 in list: no stats for current db');

    $node->safe_psql($dbname, "SELECT ext_vacuum_statistics.remove_track_database(1)");
    $node->safe_psql($dbname, "SELECT ext_vacuum_statistics.add_track_database($dboid)");
    $track = $node->safe_psql($dbname, "SELECT track_kind, count(*) FROM ext_vacuum_statistics.track_list() GROUP BY track_kind");
    like($track, qr/database\s*\|\s*1/, 'track_list: 1 database after add_track_database(dboid)');
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.track_databases_from_list = on"] });

    $rel_count = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    ok($rel_count > 0, 'current db in list: stats collected');
};

#------------------------------------------------------------------------------
# Test 5: track_relations (via add/remove_track_relation)
#------------------------------------------------------------------------------
subtest 'track_relations (add/remove)' => sub {
    $node->safe_psql($dbname, "SELECT ext_vacuum_statistics.remove_track_relation($dboid, $reloid)");
    $node->safe_psql($dbname, "SELECT ext_vacuum_statistics.add_track_relation(0, 99999)");  # rel 99999, any db
    my $track = $node->safe_psql($dbname, "SELECT track_kind, count(*) FROM ext_vacuum_statistics.track_list() GROUP BY track_kind");
    like($track, qr/relation\s*\|\s*1/, 'track_list: 1 relation after add_track_relation(0, 99999)');
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.track_relations_from_list = on"] });

    my $rel_count = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    is($rel_count, 0, 'only rel 99999 in list: no stats for guc_test');

    $node->safe_psql($dbname, "SELECT ext_vacuum_statistics.remove_track_relation(0, 99999)");
    $node->safe_psql($dbname, "SELECT ext_vacuum_statistics.add_track_relation($dboid, $reloid)");
    $track = $node->safe_psql($dbname, "SELECT track_kind, count(*) FROM ext_vacuum_statistics.track_list() GROUP BY track_kind");
    like($track, qr/relation\s*\|\s*1/, 'track_list: 1 relation after add_track_relation(dboid, reloid)');
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.track_relations_from_list = on"] });

    $rel_count = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    ok($rel_count > 0, 'current table in list: stats collected');
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

    # collect = wal general timing: all except buffers
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.collect = 'wal general timing'"] });

    $row = $node->safe_psql($dbname,
        "SELECT total_blks_read, total_blks_hit, blk_read_time, blk_write_time
         FROM ext_vacuum_statistics.pg_stats_vacuum_tables
         WHERE relname = 'guc_test'");
    my ($r1, $r2, $r3, $r4) = split /\|/, $row;
    $r1 =~ s/\s//g;
    $r2 =~ s/\s//g;
    $r3 =~ s/\s//g;
    $r4 =~ s/\s//g;
    is($r1, 0, 'collect=wal general timing: total_blks_read zeroed');
    is($r2, 0, 'collect=wal general timing: total_blks_hit zeroed');
    is($r3, 0, 'collect=wal general timing: blk_read_time zeroed');
    is($r4, 0, 'collect=wal general timing: blk_write_time zeroed');
};

$node->stop;

done_testing();
