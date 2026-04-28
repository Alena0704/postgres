# Copyright (c) 2025 PostgreSQL Global Development Group
#
# Test GUC parameters for ext_vacuum_statistics extension:
#   vacuum_statistics.enabled
#   vacuum_statistics.object_types (all, databases, relations)
#   vacuum_statistics.track_relations (all, system, user)
#   vacuum_statistics.track_databases_from_list, add/remove_track_database
#   add/remove_track_database, add/remove_track_relation, track_*_from_list

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
    my $modify = $opts && $opts->{modify};
    my $extra = $opts && $opts->{extra_vacuum} ? $opts->{extra_vacuum} : [];
    $extra = [$extra] unless ref $extra eq 'ARRAY';
    my $sql = join("\n", (map { "SET $_;" } @$gucs),
        "SELECT ext_vacuum_statistics.vacuum_statistics_reset();",
        $modify ? (
            "TRUNCATE $table;",
            "INSERT INTO $table SELECT x FROM generate_series(1, 100) AS g(x);",
            "DELETE FROM $table;",
        ) : (),
        "VACUUM $table;",
        (map { "VACUUM $_;" } @$extra),
        # Make pending stats visible to subsequent sessions without sleeping.
        "SELECT pg_stat_force_next_flush();");
    $node->safe_psql($db, $sql);
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

    # Disable, reset and vacuum in same session.  Assert not only that the
    # row count is zero, but that the specific counters remain zero: a stray
    # row with zero counters would otherwise pass a bare COUNT(*)=0 check.
    reset_and_vacuum($dbname, 'guc_test', { gucs => ['vacuum_statistics.enabled = off'] });

    $count = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    is($count, 0, 'no rows when disabled');

    my $sums = $node->safe_psql($dbname, q{
        SELECT COALESCE(SUM(total_blks_read), 0)
             + COALESCE(SUM(total_blks_dirtied), 0)
             + COALESCE(SUM(pages_scanned), 0)
          FROM ext_vacuum_statistics.pg_stats_vacuum_tables
         WHERE relname = 'guc_test'
    });
    is($sums, '0', 'no counters accumulated when disabled');
};

#------------------------------------------------------------------------------
# Test 2: vacuum_statistics.object_types (databases only, relations only)
#------------------------------------------------------------------------------
subtest 'vacuum_statistics.object_types' => sub {
    # track only db stats, no relation stats
    reset_and_vacuum($dbname, 'guc_test', {
        gucs => ["vacuum_statistics.object_types = 'databases'"],
        modify => 1,
    });
    my $db_has_dbs = $node->safe_psql($dbname,
        "SELECT COALESCE(SUM(db_blks_hit), 0) FROM ext_vacuum_statistics.pg_stats_vacuum_database WHERE dboid = $dboid");
    my $rel_dbs = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    is($rel_dbs, 0, 'track=databases: no relation stats');
    ok($db_has_dbs > 0, 'track=databases: database stats collected');

    # track only relation stats, no db stats
    reset_and_vacuum($dbname, 'guc_test', {
        gucs => ["vacuum_statistics.object_types = 'relations'"],
        modify => 1,
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
    reset_and_vacuum($dbname, 'guc_test', {
        gucs => [
            "vacuum_statistics.object_types = 'relations'",
            "vacuum_statistics.track_relations = 'user'",
        ],
        extra_vacuum => ['pg_class'],
    });

    my $user_rel = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    my $sys_rel = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'pg_class'");
    ok($user_rel > 0, 'track_relations=user: user table stats collected');
    is($sys_rel, 0, 'track_relations=user: system table stats not collected');

    # track_relations - only system tables
    reset_and_vacuum($dbname, 'guc_test', {
        gucs => [
            "vacuum_statistics.object_types = 'relations'",
            "vacuum_statistics.track_relations = 'system'",
        ],
        extra_vacuum => ['pg_class'],
    });

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
    $node->safe_psql($dbname, "SELECT ext_vacuum_statistics.add_track_database($dboid)");
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.track_databases_from_list = on"], modify => 1 });

    my $rel_count = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    ok($rel_count > 0, 'db in list: stats collected');

    $node->safe_psql($dbname, "SELECT ext_vacuum_statistics.remove_track_database($dboid)");
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.track_databases_from_list = on"], modify => 1 });

    $rel_count = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    is($rel_count, 0, 'db removed from list: no stats');
};

#------------------------------------------------------------------------------
# Test 5: track_relations (via add/remove_track_relation)
#------------------------------------------------------------------------------
subtest 'track_relations (add/remove)' => sub {
    $node->safe_psql($dbname, "SELECT ext_vacuum_statistics.remove_track_relation($dboid, $reloid)");
    $node->safe_psql($dbname, "SELECT ext_vacuum_statistics.add_track_relation($dboid, $reloid)");
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.track_relations_from_list = on"], modify => 1 });

    my $rel_count = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    ok($rel_count > 0, 'table in list: stats collected');

    $node->safe_psql($dbname, "SELECT ext_vacuum_statistics.remove_track_relation($dboid, $reloid)");
    reset_and_vacuum($dbname, 'guc_test', { gucs => ["vacuum_statistics.track_relations_from_list = on"], modify => 1 });

    $rel_count = $node->safe_psql($dbname,
        "SELECT COUNT(*) FROM ext_vacuum_statistics.pg_stats_vacuum_tables WHERE relname = 'guc_test'");
    is($rel_count, 0, 'table removed from list: no stats');
};

#------------------------------------------------------------------------------
# Test 6: vacuum_statistics.collect - per-category gating
#
# With collect='wal' only wal_* counters must advance; buffer, timing, and
# general categories must stay at zero.  With collect='buffers' the inverse
# holds.  Unknown tokens must be rejected by the check-hook.
#------------------------------------------------------------------------------
subtest 'vacuum_statistics.collect' => sub {
    # wal-only: WAL counters should accumulate, buffers/timing/general should not.
    reset_and_vacuum($dbname, 'guc_test', {
        gucs => ["vacuum_statistics.collect = 'wal'"],
        modify => 1,
    });

    my $wal = $node->safe_psql($dbname, q{
        SELECT COALESCE(SUM(wal_records), 0) > 0
          FROM ext_vacuum_statistics.pg_stats_vacuum_tables
         WHERE relname = 'guc_test'
    });
    is($wal, 't', "collect='wal': wal_records accumulated");

    my $other = $node->safe_psql($dbname, q{
        SELECT COALESCE(SUM(total_blks_read), 0)
             + COALESCE(SUM(total_blks_hit), 0)
             + COALESCE(SUM(total_time), 0)
             + COALESCE(SUM(tuples_deleted), 0)
             + COALESCE(SUM(pages_scanned), 0)
          FROM ext_vacuum_statistics.pg_stats_vacuum_tables
         WHERE relname = 'guc_test'
    });
    is($other, '0',
        "collect='wal': buffer/timing/general counters not accumulated");

    # buffers-only: buffer counters should advance, WAL should not.
    reset_and_vacuum($dbname, 'guc_test', {
        gucs => ["vacuum_statistics.collect = 'buffers'"],
        modify => 1,
    });

    my $buf = $node->safe_psql($dbname, q{
        SELECT COALESCE(SUM(total_blks_read), 0)
             + COALESCE(SUM(total_blks_hit), 0) > 0
          FROM ext_vacuum_statistics.pg_stats_vacuum_tables
         WHERE relname = 'guc_test'
    });
    is($buf, 't', "collect='buffers': buffer counters accumulated");

    my $wal_off = $node->safe_psql($dbname, q{
        SELECT COALESCE(SUM(wal_records), 0)
          FROM ext_vacuum_statistics.pg_stats_vacuum_tables
         WHERE relname = 'guc_test'
    });
    is($wal_off, '0',
        "collect='buffers': WAL counters not accumulated");

    # Unknown category must be rejected by the check-hook.
    my ($ret, $stdout, $stderr) = $node->psql($dbname,
        "SET vacuum_statistics.collect = 'nope'");
    isnt($ret, 0, "collect='nope': rejected by check-hook");
    like($stderr, qr/Unrecognized category "nope"/,
        "collect='nope': errdetail names the offending token");
};

$node->stop;

done_testing();
