# ext_vacuum_statistics

Extended vacuum statistics extension for PostgreSQL. It collects and exposes detailed per-table, per-index, and per-database vacuum statistics (buffer I/O, WAL, general, timing) via convenient views in the `ext_vacuum_statistics` schema.

## Installation

```
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
| `vacuum_statistics.object_types` | all | Object types for statistics: `all`, `databases`, `relations` |
| `vacuum_statistics.track_relations` | all | When tracking relations: `all`, `system`, `user` |
| `vacuum_statistics.track_databases_from_list` | off | If on, track only databases added via add_track_database |
| `vacuum_statistics.track_relations_from_list` | off | If on, track only relations added via add_track_relation |

## Memory usage

Each tracked object (table, index, or database) uses approximately **232 bytes** of shared memory on Linux x86_64 (e.g. Ubuntu): common stats (buffers, WAL, timing) ~144 bytes; type + union ~88 bytes (union holds table-specific or index-specific fields, allocated size is the same for both).

The exact size depends on the platform. Call `ext_vacuum_statistics.shared_memory_size()` to get the total shared memory used by the extension. The GUCs provided by the extension allow controlling the amount of memory used: `vacuum_statistics.object_types` to track only databases or relations, `vacuum_statistics.track_relations` to restrict to user or system tables/indexes, and `track_*_from_list` to track only selected databases and relations.

Example: a database with 1000 tables and 2000 indexes, all tracked, uses about **700 KB** on Ubuntu (3001 entries × 232 bytes). Per-database entries add one entry per tracked database.

## Advanced tuning

### Track only database-level stats

```sql
SET vacuum_statistics.object_types = 'databases';
```

Statistics are accumulated per database; per-relation views remain empty.

### Track only user or system tables

```sql
SET vacuum_statistics.object_types = 'relations';
SET vacuum_statistics.track_relations = 'user';   -- skip system catalogs
-- or
SET vacuum_statistics.track_relations = 'system'; -- only system catalogs
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
SET vacuum_statistics.object_types = 'databases';
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

- Must be loaded via `shared_preload_libraries`; it cannot be loaded on demand.
- Tracking configuration (`add_track_*`, `remove_track_*`) is stored in a file and shared across all databases in the cluster.
