# Copyright (c) 2025 PostgreSQL Global Development Group
#
# Test cumulative vacuum stats using ext_vacuum_statistics extension (TAP)
#
# In short, this test validates the correctness and stability of cumulative
# vacuum statistics accounting around freezing, visibility, and revision
# tracking across multiple VACUUMs and backend operations.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

#------------------------------------------------------------------------------
# Test cluster setup
#------------------------------------------------------------------------------

my $node = PostgreSQL::Test::Cluster->new('ext_stat_vacuum');
$node->init;

# Configure the server: preload extension and aggressive freezing behavior
$node->append_conf('postgresql.conf', q{
    shared_preload_libraries = 'ext_vacuum_statistics'
    log_min_messages = notice
    vacuum_freeze_min_age = 0
    vacuum_freeze_table_age = 0
    vacuum_multixact_freeze_min_age = 0
    vacuum_multixact_freeze_table_age = 0
    vacuum_max_eager_freeze_failure_rate = 1.0
    vacuum_failsafe_age = 0
    vacuum_multixact_failsafe_age = 0
    track_functions = 'all'
});

$node->start();

#------------------------------------------------------------------------------
# Database creation and initialization
#------------------------------------------------------------------------------

$node->safe_psql('postgres', q{
    CREATE DATABASE statistic_vacuum_database_regression;
});

# Main test database name
my $dbname = 'statistic_vacuum_database_regression';

# Create extension
$node->safe_psql($dbname, q{
    CREATE EXTENSION ext_vacuum_statistics;
});

#------------------------------------------------------------------------------
# Timing parameters for polling loops
#------------------------------------------------------------------------------

my $timeout    = 30;     # overall wait timeout in seconds
my $interval   = 0.015;  # poll interval in seconds (15 ms)
my $start_time = time();
my $updated    = 0;

#------------------------------------------------------------------------------
# wait_for_vacuum_stats
#
# Polls ext_vacuum_statistics.pg_stats_vacuum_tables until the named columns exceed the
# provided baseline values or until timeout.
#
#   tab_all_frozen_pages_count  => 0   # baseline numeric
#   tab_all_visible_pages_count => 0   # baseline numeric
#   run_vacuum                  => 0   # if true, run vacuum before polling
#
# Returns: 1 if the condition is met before timeout, 0 otherwise.
#------------------------------------------------------------------------------
sub wait_for_vacuum_stats {
    my (%args) = @_;

    my $tab_all_frozen_pages_count  = $args{tab_all_frozen_pages_count} || 0;
    my $tab_all_visible_pages_count = $args{tab_all_visible_pages_count} || 0;
    my $run_vacuum                  = $args{run_vacuum} ? 1 : 0;
    my $result_query;

    my $start = time();
    my $sql;

    # Run VACUUM once if requested, before polling
    if ($run_vacuum) {
        $node->safe_psql($dbname, 'VACUUM (FREEZE, VERBOSE) vestat');
    }

    while ((time() - $start) < $timeout) {

        if ($run_vacuum) {
            $sql = "
            SELECT (vm_new_visible_frozen_pages > $tab_all_frozen_pages_count)
               FROM ext_vacuum_statistics.pg_stats_vacuum_tables
              WHERE relname = 'vestat'";
        }
        else {
            $sql = "
            SELECT (pg_stat_get_frozen_page_marks_cleared(c.oid) > $tab_all_frozen_pages_count AND
                     pg_stat_get_visible_page_marks_cleared(c.oid) > $tab_all_visible_pages_count)
               FROM pg_class c
              WHERE relname = 'vestat'";
        }

        $result_query = $node->safe_psql($dbname, $sql);

        return 1 if (defined $result_query && $result_query eq 't');

        sleep($interval);
    }

    return 0;
}

#------------------------------------------------------------------------------
# Variables to hold vacuum statistics snapshots for comparisons
#------------------------------------------------------------------------------

my $vm_new_visible_frozen_pages = 0;

my $rev_all_frozen_pages = 0;
my $rev_all_visible_pages = 0;

my $vm_new_visible_frozen_pages_prev = 0;

my $rev_all_frozen_pages_prev = 0;
my $rev_all_visible_pages_prev = 0;

my $res;

#------------------------------------------------------------------------------
# fetch_vacuum_stats
#
# Loads current values of the relevant vacuum counters for the test table
# into the package-level variables above so tests can compare later.
#------------------------------------------------------------------------------

sub fetch_vacuum_stats {
    $vm_new_visible_frozen_pages = $node->safe_psql(
        $dbname,
        "SELECT vt.vm_new_visible_frozen_pages
           FROM ext_vacuum_statistics.pg_stats_vacuum_tables vt
          WHERE vt.relname = 'vestat';"
    );

    $rev_all_frozen_pages = $node->safe_psql(
        $dbname,
        "SELECT pg_stat_get_frozen_page_marks_cleared(c.oid)
           FROM pg_class c
          WHERE c.relname = 'vestat';"
    );

    $rev_all_visible_pages = $node->safe_psql(
        $dbname,
        "SELECT pg_stat_get_visible_page_marks_cleared(c.oid)
           FROM pg_class c
          WHERE c.relname = 'vestat';"
    );
}

#------------------------------------------------------------------------------
# save_vacuum_stats
#------------------------------------------------------------------------------
sub save_vacuum_stats {
    $vm_new_visible_frozen_pages_prev = $vm_new_visible_frozen_pages;
    $rev_all_frozen_pages_prev = $rev_all_frozen_pages;
    $rev_all_visible_pages_prev = $rev_all_visible_pages;
}

#------------------------------------------------------------------------------
# print_vacuum_stats_on_error
#------------------------------------------------------------------------------
sub print_vacuum_stats_on_error {
    diag(
            "Statistics in the failed test\n" .
            "Table statistics:\n" .
            "  Before test:\n" .
            "    vm_new_visible_frozen_pages = $vm_new_visible_frozen_pages_prev\n" .
            "    rev_all_frozen_pages = $rev_all_frozen_pages_prev\n" .
            "    rev_all_visible_pages = $rev_all_visible_pages_prev\n" .
            "  After test:\n" .
            "    vm_new_visible_frozen_pages = $vm_new_visible_frozen_pages\n" .
            "    rev_all_frozen_pages = $rev_all_frozen_pages\n" .
            "    rev_all_visible_pages = $rev_all_visible_pages\n"
    );
};

#------------------------------------------------------------------------------
# Test 1: Create test table, populate it and run an initial vacuum to force freezing
#------------------------------------------------------------------------------

subtest 'Test 1: Create test table, populate it and run an initial vacuum to force freezing' => sub
{
$node->safe_psql($dbname, q{
    CREATE TABLE vestat (x int)
        WITH (autovacuum_enabled = off, fillfactor = 10);
    INSERT INTO vestat SELECT x FROM generate_series(1, 1000) AS g(x);
    ANALYZE vestat;
    VACUUM (FREEZE, VERBOSE) vestat;
});

$updated = wait_for_vacuum_stats(
    tab_all_frozen_pages_count  => 0,
    tab_all_visible_pages_count => 0,
    run_vacuum                  => 1,
);

ok($updated,
   'vacuum stats updated after vacuuming the table (vm_new_visible_frozen_pages advanced)')
  or diag "Timeout waiting for ext_vacuum_statistics to update after $timeout seconds during vacuum";

fetch_vacuum_stats();

ok($vm_new_visible_frozen_pages > $vm_new_visible_frozen_pages_prev, 'table vm_new_visible_frozen_pages has increased');
ok($rev_all_frozen_pages == $rev_all_frozen_pages_prev, 'table rev_all_frozen_pages stay the same');
ok($rev_all_visible_pages == $rev_all_visible_pages_prev, 'table rev_all_visible_pages stay the same');
} or print_vacuum_stats_on_error();

#------------------------------------------------------------------------------
# Test 2: Trigger backend updates
# Backend activity should reset per-page visibility/freeze marks and increment revision counters
#------------------------------------------------------------------------------
subtest 'Test 2: Trigger backend updates' => sub
{
save_vacuum_stats();

$node->safe_psql($dbname, q{
    UPDATE vestat SET x = x + 1001;
});

$updated = wait_for_vacuum_stats(
    tab_all_frozen_pages_count  => 0,
    tab_all_visible_pages_count => 0,
    run_vacuum                  => 0,
);

ok($updated,
   'vacuum stats updated after backend tuple updates (rev_all_frozen_pages and rev_all_visible_pages advanced)')
  or diag "Timeout waiting for vacuum stats update after $timeout seconds";

fetch_vacuum_stats();

ok($vm_new_visible_frozen_pages == $vm_new_visible_frozen_pages_prev, 'table vm_new_visible_frozen_pages stay the same');
ok($rev_all_frozen_pages > $rev_all_frozen_pages_prev, 'table rev_all_frozen_pages has increased');
ok($rev_all_visible_pages > $rev_all_visible_pages_prev, 'table rev_all_visible_pages has increased');
} or print_vacuum_stats_on_error();

#------------------------------------------------------------------------------
# Test 3: Force another vacuum after backend modifications - vacuum should restore freeze/visibility
#------------------------------------------------------------------------------
subtest 'Test 3: Force another vacuum after backend modifications - vacuum should restore freeze/visibility' => sub
{
save_vacuum_stats();

$node->safe_psql($dbname, q{ VACUUM vestat; });

$updated = wait_for_vacuum_stats(
    tab_all_frozen_pages_count  => $vm_new_visible_frozen_pages,
    tab_all_visible_pages_count => 0,
    run_vacuum                  => 1,
);

ok($updated,
   'vacuum stats updated after vacuuming the all-updated table (vm_new_visible_frozen_pages advanced)')
  or diag "Timeout waiting for ext_vacuum_statistics to update after $timeout seconds during vacuum";

fetch_vacuum_stats();

ok($vm_new_visible_frozen_pages > $vm_new_visible_frozen_pages_prev, 'table vm_new_visible_frozen_pages has increased');
ok($rev_all_frozen_pages == $rev_all_frozen_pages_prev, 'table rev_all_frozen_pages stay the same');
ok($rev_all_visible_pages == $rev_all_visible_pages_prev, 'table rev_all_visible_pages stay the same');
} or print_vacuum_stats_on_error();

#------------------------------------------------------------------------------
# Cleanup
#------------------------------------------------------------------------------

$node->safe_psql('postgres', q{
    DROP DATABASE statistic_vacuum_database_regression;
});

$node->stop;
done_testing();
