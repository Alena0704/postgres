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
SET track_vacuum_statistics = on;
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
