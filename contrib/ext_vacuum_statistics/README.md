# ext_vacuum_statistics

Extended vacuum statistics extension for PostgreSQL. It collects and exposes detailed per-table, per-index, and per-database vacuum statistics (buffer I/O, WAL, general, timing) via convenient views in the `ext_vacuum_statistics` schema.

## Installation

The extension requires a patched PostgreSQL build. Apply the vacuum statistics patch before building:

```
cd postgresql-source
git apply 0001-Add-machinery-for-grabbing-extended-vacuum-statistic.patch
./configure tmp_install="$(pwd)/my/inst"
make clean && make && make install
cd contrib/ext_vacuum_statistics
make && make install
```

It is essential that the extension is listed in `shared_preload_libraries` because it registers a vacuum hook at server startup.

In your `postgresql.conf`:

```
shared_preload_libraries = 'ext_vacuum_statistics'
```

Restart PostgreSQL.

In your database:

```sql
CREATE EXTENSION ext_vacuum_statistics;
```

## Usage

Query vacuum statistics via the provided views:

```sql
-- Per-table heap vacuum statistics
SELECT * FROM ext_vacuum_statistics.pg_stats_vacuum_tables;

-- Per-index vacuum statistics
SELECT * FROM ext_vacuum_statistics.pg_stats_vacuum_indexes;

-- Per-database aggregate vacuum statistics
SELECT * FROM ext_vacuum_statistics.pg_stats_vacuum_database;
```

Example output:

```
 relname   | total_blks_read | total_blks_hit | wal_records | tuples_deleted | pages_removed
-----------+-----------------+----------------+-------------+----------------+---------------
 mytable   |             120 |            340 |          15 |            500 |            10
```

Reset statistics when needed:

```sql
SELECT ext_vacuum_statistics.vacuum_statistics_reset();
```

## Configuration (GUCs)

| GUC | Default | Description |
|-----|---------|-------------|
| `vacuum_statistics.enabled` | on | Enable extended vacuum statistics collection |
| `vacuum_statistics.track` | all | What to track: `all`, `databases`, `relations` |
| `vacuum_statistics.track_relations` | all | When tracking relations: `all`, `system`, `user` |
| `vacuum_statistics.track_databases_from_list` | off | If on, track only databases added via add_track_database |
| `vacuum_statistics.track_relations_from_list` | off | If on, track only relations added via add_track_relation |
| `vacuum_statistics.collect` | all | Space-separated: buffers, wal, general, timing, or all |

## Advanced tuning

### Track only database-level stats

```sql
SET vacuum_statistics.track = 'databases';
```

Statistics are accumulated per database; per-relation views remain empty.

### Track only user or system tables

```sql
SET vacuum_statistics.track = 'relations';
SET vacuum_statistics.track_relations = 'user';   -- skip system catalogs
-- or
SET vacuum_statistics.track_relations = 'system'; -- only system catalogs
```

### Limit collected metrics

```sql
-- Only buffer I/O
SET vacuum_statistics.collect = 'buffers';

-- Only WAL
SET vacuum_statistics.collect = 'wal';

-- All except buffers
SET vacuum_statistics.collect = 'wal general timing';
```

### Filter by database or relation OIDs

Add OIDs via functions (persisted to `pg_stat/ext_vacuum_statistics_track.oid`) and enable filtering:

```sql
-- Add databases and relations to track
SELECT ext_vacuum_statistics.add_track_database(16384);
SELECT ext_vacuum_statistics.add_track_relation(16384, 16385);  -- dboid, reloid
SELECT ext_vacuum_statistics.add_track_relation(0, 16386);      -- rel 16386 in any db

-- Enable list-based filtering (off = track all)
SET vacuum_statistics.track_databases_from_list = on;
SET vacuum_statistics.track_relations_from_list = on;
```

Remove OIDs when no longer needed:

```sql
SELECT ext_vacuum_statistics.remove_track_database(16384);
SELECT ext_vacuum_statistics.remove_track_relation(16384, 16385);
```

Inspect the current tracking configuration:

```sql
SELECT * FROM ext_vacuum_statistics.track_list();
```

Returns `track_kind`, `dboid`, `reloid`. When `dboid` or `reloid` is NULL, statistics are collected for all.

## Recipes

**Reduce overhead by tracking only databases:**

```sql
SET vacuum_statistics.track = 'databases';
```

**Track only a specific table in a specific database:**

```sql
SELECT ext_vacuum_statistics.add_track_database(
    (SELECT oid FROM pg_database WHERE datname = current_database())
);
SELECT ext_vacuum_statistics.add_track_relation(
    (SELECT oid FROM pg_database WHERE datname = current_database()),
    'mytable'::regclass
);
SET vacuum_statistics.track_databases_from_list = on;
SET vacuum_statistics.track_relations_from_list = on;
```

**Disable statistics collection temporarily:**

```sql
SET vacuum_statistics.enabled = off;
```

## Views

| View | Description |
|------|-------------|
| `ext_vacuum_statistics.pg_stats_vacuum_tables` | Per-table heap vacuum stats (pages scanned, tuples deleted, WAL, timing, etc.) |
| `ext_vacuum_statistics.pg_stats_vacuum_indexes` | Per-index vacuum stats |
| `ext_vacuum_statistics.pg_stats_vacuum_database` | Per-database aggregate vacuum stats |

## Limitations

- Requires a patched PostgreSQL build (vacuum statistics patch).
- Must be loaded via `shared_preload_libraries`; it cannot be loaded on demand.
- Tracking configuration (`add_track_*`, `remove_track_*`) is stored in a file and shared across all databases in the cluster.

## Authors

The core vacuum statistics patch was developed by Alena Rybakina, Andrei Lepikhov, Andrei Zubkov, and reviewers. The extension was developed by Alena Rybakina.
