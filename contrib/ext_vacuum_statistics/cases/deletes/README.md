# DELETE-нагрузки и вакуумные мисконфиги

Шесть фаз, каждая = «один DELETE-паттерн × одна вакуумная мисконфигурация».
Прогон делается дважды: BROKEN (поломанные GUC) и FIXED (поправленные).
Все наблюдения берутся **только** из `ext_vacuum_statistics`
(через снимки `pg_profile`): по куче — `pg_stats_vacuum_tables`,
по индексам — `pg_stats_vacuum_indexes`, по БД —
`pg_stats_vacuum_database`. Никаких `pg_stat_user_*`, `pg_class.relpages`
или внешних счётчиков.

```
deletes/
├── README.md                       ← этот файл
├── patterns/
│   ├── pattern_delete_border.sql   ← удаления только по краям таблицы
│   ├── pattern_delete_middle.sql   ← hotspot ±5000 в середине
│   ├── pattern_delete_sparse.sql   ← каждая ~80-я строка (один dead tup на страницу)
│   └── pattern_xid_burn.sql        ← txid_current() для форсажа failsafe
├── detect_delete_cases.sql         ← evs_window_with_indexes + детекторы D, E, F
├── run_delete_phases.sh            ← движок (берёт MODE=broken|fixed)
├── run_delete_cases.sh             ← обёртка → MODE=broken
├── run_delete_fixes.sh             ← обёртка → MODE=fixed
├── plot_delete_cases.py            ← графики broken vs fixed
└── out_broken/  out_fixed/         ← CSV-выгрузки и PNG
```

## Как удаляем (паттерны)

| паттерн | где удаляем | что заодно делаем | что ожидаем в счётчиках |
|---|---|---|---|
| **border** | первые 5% и последние 5% `aid` | INSERT нового tuple в хвост через `evs_delete_seq` | `pages_removed > 0` (страницы у нижнего края пустеют → relation truncate), индексные `pages_deleted > 0` (листы у нижнего края btree высвобождаются) |
| **middle** | `±5000` около центра | INSERT в хвост | `pages_scanned` низкий, `tuples_deleted` высокий, `pages_removed ≈ 0`, `recently_dead_tuples` растёт |
| **sparse** | каждая 80-я строка (~один dead tup на 8 KiB-страницу) | INSERT в хвост | `pages_scanned ≫ pages_removed`, всплеск `vm_new_visible_pages`, `wal_fpi` высокий, индексные `total_blks_dirtied` и `wal_bytes` максимальны |
| **xid burn** | ничего не удаляем | `SELECT txid_current()` × N клиентов | XID-ы сжигаются → срабатывает failsafe → `wraparound_failsafe_count > 0`, всплеск `tuples_frozen` и `vm_new_frozen_pages`, **индексный cleanup пропускается** |

DELETE везде в паре с INSERT в хвост через единую последовательность
`evs_delete_seq` (стартует от `max_aid+1`). Это даёт две полезные
вещи: (а) heap не стечёт к нулю за фазу, (б) индекс получает
монотонно растущие ключи в хвосте при разреженных удалениях слева —
классический сценарий btree-bloat.

## Шесть фаз

| фаза | вакуумный мисконфиг (BROKEN) | паттерн | главный сигнал в `ext_vacuum_statistics` |
|---|---|---|---|
| **A-sparse** | `maintenance_work_mem = 64kB` (global) | sparse | `Δ index_vacuum_count` ≫ 1 (TID-массив переполняется → много проходов по индексу), `pages_per_pass < 500` |
| **B-middle** | `autovacuum_vacuum_cost_delay = 100ms`, `cost_limit = 10` (global) | middle | `Δ delay_time / Δ total_time ≈ 0.9`, `tuples_deleted/sec` растёт скачком после fix |
| **C-border** | `ALTER TABLE pgbench_accounts SET (autovacuum_vacuum_scale_factor=0.5, autovacuum_vacuum_threshold=1e9)` | border | долгая «тишина» по `pages_scanned`, потом гигантский бёрст; на индексе — крупный единичный `pages_deleted` |
| **D-sparse** | `ALTER TABLE pgbench_accounts SET (autovacuum_freeze_min_age=2e9, autovacuum_freeze_max_age=2e9, autovacuum_freeze_table_age=2e9)` | sparse | `Δ tuples_frozen = 0`, `Δ vm_new_frozen_pages = 0` всю фазу; после `RESET` — резкий двойной скачок |
| **E-sparse** | `ALTER TABLE pgbench_accounts SET (vacuum_index_cleanup = off)` | sparse | `Δ table.tuples_deleted` большой, **`Δ index.tuples_deleted = 0`**; после `RESET` — единичный громкий пик `idx_blks_dirty` и `idx_wal_bytes` (catch-up cleanup) |
| **F-xidburn** | `autovacuum_freeze_max_age=100000`, `vacuum_failsafe_age=80000` (global) | xid burn | `Δ wraparound_failsafe_count > 0` (прямой счётчик), всплеск `tuples_frozen` и `vm_new_frozen_pages`, **индексный cleanup пропущен** (failsafe!), `db_interrupts_count` может расти |

> Фаза F скрыта за `RUN_WRAPAROUND=1`. Без флага она тихо
> пропускается — снижение `autovacuum_freeze_max_age` глобально
> мешает любым другим базам в кластере.

## Как читать графики `delete_<phase>.png`

Каждая картинка — 4 строки × 2 столбца (BROKEN | FIXED) на одной таблице
`pgbench_accounts`:

| строка | что | что искать |
|---|---|---|
| 1 | **heap blocks/sec** (read / hit / dirtied / written) | как меняется I/O по куче — sparse даст пик `dirtied`, middle — пик `hit` |
| 2 | **WAL** records / FPI / bytes per sec | `FPI` = full-page images: после checkpoint первый touch страницы создаёт FPI, sparse даёт максимум |
| 3 | **freeze & VM transitions** + `pages_removed` | для D — broken: всё в нуле; fixed: резкий двойной всплеск `tuples_frozen` + `vm_new_frozen` |
| 4 | **INDEX vacuum** — idx blks dirtied/sec, idx tuples deleted/sec, idx WAL bytes/sec, кумулятивные idx pages_deleted | для E — broken: всё в нуле; fixed: один громкий пик после первого fixed-VACUUM |

И сводный `delete_summary_bars.png` — broken vs fixed по 10 ключевым
суммам на фазу. Это быстрый «smoke»-взгляд: где разница есть,
где нет.

## Детекторы (как в проде)

`detect_delete_cases.sql` строит три выборки поверх `evs_window` и
нового `evs_window_with_indexes`:

```sql
-- D: heap чистится, заморозка не идёт
SELECT * FROM evs_case4_freeze_lag    WHERE t_to > now() - '1 day'::interval;

-- E: heap чистится, индекс — нет
SELECT * FROM evs_case5_index_bloat   WHERE t_to > now() - '1 day'::interval;

-- F: failsafe был; либо большой freeze-burst при пропущенном idx-cleanup
SELECT * FROM evs_case6_wraparound    WHERE t_to > now() - '1 day'::interval;
SELECT * FROM evs_case6_wraparound_db WHERE t_to > now() - '1 day'::interval;
```

Пороги в детекторах подобраны так, чтобы на «здоровой» fixed-фазе
выборки были пустыми, а на broken-фазе — содержали хотя бы одно окно.

## Запуск

```bash
PG_BASE_CONN='host=/tmp port=5499' \
BENCH_DB=pgbench_evs_del \
SCALE=50 PHASE_SECONDS=90 SAMPLE_PERIOD=15 CLIENTS=8 \
./run_delete_cases.sh

PG_BASE_CONN='host=/tmp port=5499' \
BENCH_DB=pgbench_evs_del \
SCALE=50 PHASE_SECONDS=90 SAMPLE_PERIOD=15 CLIENTS=8 \
./run_delete_fixes.sh

./plot_delete_cases.py \
    --broken-dir ./out_broken \
    --fixed-dir  ./out_fixed \
    --out-dir    .
```

Прогон фазы F (wraparound):

```bash
RUN_WRAPAROUND=1 ./run_delete_cases.sh
RUN_WRAPAROUND=1 ./run_delete_fixes.sh
```

Подмножество фаз:

```bash
PHASES=E-sparse,D-sparse ./run_delete_cases.sh
```

## Что положено в `out_*/`

```
delete_phases.csv                  ← список фаз с временами и sample_id
delete_window.csv                  ← evs_window (только heap, как и раньше)
delete_window_with_indexes.csv     ← evs_window + агрегированные дельты по индексам
delete_index_window.csv            ← per-index дельты
delete_case4_freeze_lag.csv        ← хиты детектора D
delete_case5_index_bloat.csv       ← хиты детектора E
delete_case6_wraparound.csv        ← хиты детектора F (per-table)
delete_case6_wraparound_db.csv     ← хиты детектора F (per-database)
vac_<mode>_<phase>.log             ← вывод финального VACUUM (VERBOSE) для фаз A/D/E
```

## Что расширение НЕ покажет

- Физический размер индекса (`pg_class.relpages`) — это вне scope.
  Кейс E ловится по «cleanup не делался», а не по «индекс раздут на N МБ».
- Скорость пользовательских index scan'ов в OLTP. Расширение собирает
  стоимость **VACUUM** (heap и index) — то, что bloat влечёт за собой
  для следующего vacuum-цикла. Косвенно: если `idx_blks_dirty/sec`
  при VACUUM растёт, значит в индексе много мусора, и обычные
  index scan'ы тоже заплатят cache-misses. Но это уже интерпретация,
  а не прямой сигнал.
- Точное время `index_vacuum_count`-внутреннего прохода: в расширении
  есть только суммарный `total_time` per index. Чтобы ответить
  «сколько секунд один проход», нужно поделить на `Δ index_vacuum_count`
  родительской таблицы (см. `evs_case2_mwm_small`).
