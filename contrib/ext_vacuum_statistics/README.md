# ext_vacuum_statistics

PostgreSQL extension providing convenience views for **extended vacuum statistics**.

## Overview

This extension provides a dedicated schema `ext_vacuum_statistics` with views:

- **ext_vacuum_statistics.pg_stats_vacuum_tables** — per-table (heap) vacuum stats
- **ext_vacuum_statistics.pg_stats_vacuum_indexes** — per-index vacuum stats
- **ext_vacuum_statistics.pg_stats_vacuum_database** — per-database vacuum stats

## Requirements

1. **PostgreSQL with v27 vacuum statistics patch** applied:
   - 0001-Add-machinery-for-grabbing-extended-vacuum-statistic.patch

2. Rebuild PostgreSQL after applying patches: `make clean && make install`

## Installation

```bash
cd contrib/ext_vacuum_statistics
make install
```

## Usage

```sql
CREATE EXTENSION ext_vacuum_statistics;
```

Enable statistics collection (in `postgresql.conf` or per session):

```sql
SET vacuum_statistics.enabled = on;
```

## Configuration (GUCs)

| GUC | Default | Description |
|-----|---------|-------------|
| `vacuum_statistics.enabled` | on | Enable extended vacuum statistics collection |
| `vacuum_statistics.track` | all | What to track: `all`, `databases`, `relations` |
| `vacuum_statistics.track_relations` | all | When tracking relations: `all`, `system`, `user` |
| `vacuum_statistics.track_databases` | (empty) | Comma-separated database OIDs; empty = all |
| `vacuum_statistics.track_relations_list` | (empty) | Comma-separated relation OIDs; empty = all |
| `vacuum_statistics.collect` | all | Space-separated list: buffers, wal, tuples, timing, or all |

Examples:

```sql
-- Track only database-level stats
SET vacuum_statistics.track = 'databases';

-- Track only user tables (not system catalogs)
SET vacuum_statistics.track = 'relations';
SET vacuum_statistics.track_relations = 'user';

-- Only buffers and WAL
SET vacuum_statistics.collect = 'buffers wal';

-- All except buffers
SET vacuum_statistics.collect = 'wal tuples timing';

-- Only buffers
SET vacuum_statistics.collect = 'buffers';

-- Only for specific database OID
SET vacuum_statistics.track_databases = '16384';

-- Add OIDs via functions (persisted to pg_stat/ext_vacuum_statistics_track.oid)
SELECT ext_vacuum_statistics.add_track_database(16384);
SELECT ext_vacuum_statistics.add_track_relation(16385);
SELECT ext_vacuum_statistics.remove_track_database(16384);
SELECT ext_vacuum_statistics.remove_track_relation(16385);
```

Query the views:

```sql
-- Per-table vacuum statistics
SELECT * FROM ext_vacuum_statistics.pg_stats_vacuum_tables;

-- Per-index vacuum statistics
SELECT * FROM ext_vacuum_statistics.pg_stats_vacuum_indexes;

-- Per-database vacuum statistics
SELECT * FROM ext_vacuum_statistics.pg_stats_vacuum_database;
```

## Views

| View | Description |
|------|-------------|
| `ext_vacuum_statistics.pg_stats_vacuum_tables` | Wraps `pg_stat_vacuum_tables` — heap vacuum stats (pages scanned, tuples deleted, WAL, timing, etc.) |
| `ext_vacuum_statistics.pg_stats_vacuum_indexes` | Wraps `pg_stat_vacuum_indexes` — index vacuum stats |
| `ext_vacuum_statistics.pg_stats_vacuum_database` | Wraps `pg_stat_vacuum_database` — aggregated DB vacuum stats |

## Authors

Based on the extended vacuum statistics work by Alena Rybakina, Andrei Lepikhov, Andrei Zubkov, and reviewers.
