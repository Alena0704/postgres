# Три кейса ловли неправильно настроенного VACUUM

Все три сценария ловятся **только по снимкам** `pg_profile` —
никаких online-наблюдений, никаких хуков. Идея: расширение
`ext_vacuum_statistics` отдаёт кумулятивные счётчики на уровне
таблицы/индекса/БД, `pg_profile` сохраняет их в
`sample_stat_vacuum_*`, а детектор смотрит на дельты между соседними
семплами и сравнивает их с дельтой `vacuum_count + autovacuum_count`
из стандартного `sample_stat_tables`.

Разворачивается всё на pgbench-базе через один шелл-скрипт.

```
cases/
├── README.md                        ← этот файл (описание кейсов и паттернов)
├── RESULTS.md                       ← результаты прогонов (числа + графики)
├── detect_vacuum_misconfig.sql      ← три детектора + общий evs_window
├── run_cases.sh                     ← репродьюсер 3 misconfig на pgbench
├── run_fixes.sh                     ← те же фазы с исправленными GUC
├── plot_cases.py                    ← matplotlib-плоттер для misconfig
├── plot_compare.py                  ← BROKEN vs FIXED для каждого кейса
├── aggregate_patterns.sql           ← разметка окон по паттерну UPDATE
├── run_update_patterns.sh           ← драйвер 4 паттернов обновлений
├── plot_update_patterns.py          ← графики динамики по паттернам
└── patterns/
    ├── pattern_random.sql           ← random uniform (baseline)
    ├── pattern_middle.sql           ← hotspot в середине таблицы
    ├── pattern_boundaries.sql       ← удары по границам страниц
    └── pattern_chunked.sql          ← rolling chunked sweep
```

## Кейс 1. VACUUM захлёбывается из-за cost-delay

**Что не так:** `autovacuum_vacuum_cost_delay` слишком велик
относительно `autovacuum_vacuum_cost_limit`. Воркер большую часть
wall-clock'а спит вместо работы, и на горячих таблицах
(`pgbench_accounts`) дед-тапл'ы накапливаются быстрее, чем вакуум их
вычищает.

**Что видно в снимках.** В каждой паре сэмплов считаем

```
delay_share = Δ delay_time / Δ total_time
```

Если `delay_share > 0.5` и при этом `Δ tuples_deleted > 0` (вакуум не
просто простаивал), параметры дросселирования настроены агрессивно.

В скрипте кейс воспроизводится так:

```sql
ALTER SYSTEM SET autovacuum_vacuum_cost_delay = '200ms';   -- норма ~2ms
ALTER SYSTEM SET autovacuum_vacuum_cost_limit = 20;        -- норма 200
```

После 90 секунд pgbench по `pgbench_accounts` детектор ловит
`pgbench_accounts` с `delay_share` ≈ 0.9.

**График:** [case1_throttled.png](out/case1_throttled.png) — две
панели: верхняя — `delay_share` по таблицам с порогом 0.5, нижняя —
скорость `tuples_deleted` (чтобы отличить «вакуум молотит впустую» от
«вакуум занят, но дросселит»).

## Кейс 2. `maintenance_work_mem` слишком мал → много проходов по индексам

**Что не так:** массив TID-ов мёртвых тапл'ов не помещается в
`maintenance_work_mem`, и один вызов вакуума делает **несколько** обходов
индексов (`index_vacuum_count > 1`) — это удваивает/утраивает I/O и
WAL.

**Что видно в снимках.** Берём из `ext_vacuum_statistics`
кумулятивный `index_vacuum_count`, из `sample_stat_tables` —
`vacuum_count + autovacuum_count`. Считаем на пару семплов:

```
passes_per_vacuum = Δ index_vacuum_count / Δ (vacuum_count + autovacuum_count)
```

В норме это ≈ 1. Стабильно > 1.5 — это уже сигнал поднимать
`maintenance_work_mem`.

В скрипте кейс воспроизводится так:

```sql
ALTER SYSTEM SET maintenance_work_mem = '64kB';   -- минимум
ALTER SYSTEM SET autovacuum_vacuum_threshold = 50000;
```

Дополнительно скрипт делает явный `VACUUM pgbench_accounts`,
чтобы получить детерминированный «толстый» проход.

**График:** [case2_mwm_small.png](out/case2_mwm_small.png) —
`passes_per_vacuum` по времени с маркерами flagged-точек, идеальной
линией = 1 и порогом = 1.5.

## Кейс 3. Ленивый автовакуум (или выключенный для таблицы)

**Что не так:** `autovacuum_vacuum_scale_factor` / `_threshold`
настолько большие (либо `autovacuum_enabled = off` на конкретной
таблице), что вакуум на горячей таблице долго **вообще не
запускается**, а потом отрабатывает один большой «бёрст» на сильно
вспухшей таблице. Худший сценарий — срабатывает failsafe wraparound.

**Что видно в снимках.** Один и тот же кейс ловим тремя
независимыми сигналами в `evs_window`:

| сигнал | условие |
|---|---|
| failsafe запускался | `Δ wraparound_failsafe_count > 0` |
| блокировки/прерывания | `Δ missed_dead_pages > 0` или `Δ missed_dead_tuples > 0` |
| ленивый вакуум: гигантский «бёрст» | `Δ vac_runs > 0 AND Δ pages_scanned / Δ vac_runs > 50000` |
| ленивый вакуум: bloat без вакуума | `Δ vac_runs = 0 AND n_dead_tup > max(50k, n_live_tup / 5)` |

В скрипте кейс воспроизводится самым жёстким способом — выключенным
автовакуумом на `pgbench_accounts`:

```sql
ALTER TABLE pgbench_accounts SET (autovacuum_enabled = off);
```

После прокрутки нагрузки скрипт явно зовёт `VACUUM pgbench_accounts`,
чтобы один из детекторов (`pages_per_vacuum > 50k`) гарантированно
сработал.

**График:** [case3_lazy_av.png](out/case3_lazy_av.png) — две
панели: сверху `n_dead_tup` (видно, как блоут растёт), снизу
`pages_scanned / vacuum` с пороговой линией 50 000 и красными
крестиками для каждого срабатывания failsafe.

## Запуск

Минимальные требования: PostgreSQL 18, расширения `pg_profile`,
`ext_vacuum_statistics`, `pg_stat_statements`, `dblink` собраны и
установлены в кластер; `pgbench` в `$PATH`; Python с
`matplotlib` (`pip install matplotlib`).

```bash
# 1. Воспроизвести три кейса и собрать снимки pg_profile
PG_DSN='postgres://localhost/postgres' \
BENCH_DB=pgbench_evs \
SCALE=50 PHASE_SECONDS=90 CLIENTS=16 \
./run_cases.sh

# 2. Построить графики из CSV-выгрузок
./plot_cases.py --in-dir ./out
```

После прогонки в `./out/` лежат:

```
all_window.csv          ← общий evs_window (полные дельты)
case1_throttled.csv     ← хиты case 1
case2_mwm_small.csv     ← хиты case 2
case3_lazy_av.csv       ← хиты case 3
case1_throttled.png
case2_mwm_small.png
case3_lazy_av.png
```

## Как пользоваться вне демо

`detect_vacuum_misconfig.sql` независим от pgbench: его можно
запускать на любой базе с pg_profile + ext_vacuum_statistics.
В типовом мониторинге достаточно посматривать в три представления:

```sql
SELECT * FROM evs_case1_throttled WHERE t_to > now() - interval '1 day';
SELECT * FROM evs_case2_mwm_small WHERE t_to > now() - interval '1 day';
SELECT * FROM evs_case3_lazy_av   WHERE t_to > now() - interval '1 day';
```

Пороги (0.5 / 1.5 / 50000 / 1/5) намеренно жёсткие — их полезно
подкручивать под кластер, но эти значения не дают ложно-положительных
срабатываний на «здоровом» pgbench по 16 клиентов.

## Динамика вакуума под разными паттернами обновлений

Помимо «ловли поломок», полезно посмотреть, **как** разные сценарии
точечных UPDATE влияют на вакуумные счётчики во времени. Скрипт
[run_update_patterns.sh](run_update_patterns.sh) гоняет на одной и той
же `pgbench_accounts` четыре сценария по очереди, между ними и внутри
каждого делает периодические снимки `pg_profile`, потом
[plot_update_patterns.py](plot_update_patterns.py) рисует графики
динамики.

### Паттерны

| паттерн | где живут UPDATE | что ожидаем в `ext_vacuum_statistics` |
|---|---|---|
| **random** ([pattern_random.sql](patterns/pattern_random.sql)) | равномерно по всей таблице | baseline; широкий диапазон `pages_scanned`, разреженные мёртвые тапл'ы |
| **middle** ([pattern_middle.sql](patterns/pattern_middle.sql)) | hotspot ±5000 строк около середины таблицы | мало `pages_scanned`, много повторных `blks_dirtied` на одних и тех же страницах, HOT-pruning доминирует |
| **boundaries** ([pattern_boundaries.sql](patterns/pattern_boundaries.sql)) | каждая ~80-я строка (≈одна на страницу 8 KiB) | максимально широкий разлёт по страницам: `pages_scanned ≫ pages_removed`, скачок `vm_new_visible_pages`, индексам тяжело |
| **chunked** ([pattern_chunked.sql](patterns/pattern_chunked.sql)) | сквозной sweep по 100 строк через `nextval('evs_chunk_seq')` | волна dirty-страниц катится по куче; `vm_new_frozen_pages` растёт монотонно по мере прохождения волны |

Размер шага в `boundaries` = 80 не случайный: строка
`pgbench_accounts` ≈ 100 байт, на странице 8 KiB помещается ~80 живых
тапл'ов. Шаг 80 «выбивает» примерно по одному тапл'у на каждую
страницу — самое «дорогое» для вакуума распределение мёртвых строк.

### Что ловится в графиках

Для каждого паттерна `plot_update_patterns.py` рисует:

* `dynamics_<pattern>.png` — три панели по времени:
  * **pages_scanned vs pages_removed / sec** — сколько страниц вакуум
    смотрит и сколько реально освобождает;
  * **tuples_deleted vs tuples_frozen / sec** — есть ли работа по
    замораживанию вместе с очисткой мёртвых;
  * **blks_dirtied / sec** + **wal_bytes / sec** — стоимость в I/O и
    WAL.
* `dynamics_overlay.png` — все четыре паттерна на одной шкале, сдвинуто
  к началу фазы. Удобно сравнить «форму» нагрузки.
* `pattern_comparison.png` — bar-chart по 6 ключевым метрикам, нормализованный
  на длительность фазы.

Например, на **boundaries** ожидаем максимум `pages_scanned/sec`, а
`pages_removed/sec` — почти ноль (страница не пустеет от одного
мёртвого тапл'а), при этом `vm_new_visible_pages/sec` высокий, потому
что после уборки страница становится all-visible. На **middle**
ровно наоборот: `pages_scanned/sec` низкий, но `tuples_deleted/sec`
большой — вакуум долбится в одни и те же страницы.

### Запуск

```bash
PG_DSN='postgres://localhost/postgres' \
BENCH_DB=pgbench_evs \
SCALE=50 PATTERN_SECONDS=60 SAMPLE_PERIOD=15 CLIENTS=8 \
PATTERNS=random,middle,boundaries,chunked \
./run_update_patterns.sh

./plot_update_patterns.py --in-dir ./out_patterns
```

Можно гонять подмножество, например только два:

```bash
PATTERNS=middle,boundaries ./run_update_patterns.sh
```

CSV-выгрузки в `./out_patterns/`:

```
pattern_phases.csv             ← (phase_id, pattern, started_at, ended_at, sample_id_*)
pattern_window_labeled.csv     ← evs_window + колонка pattern
pattern_summary.csv            ← на pattern × relname агрегаты дельт
```

`pattern_window_labeled.csv` — это полный сырой dataset, удобно
прогонять собственные ad-hoc запросы или строить дополнительные
графики поверх.

### Как это собрано на стороне SQL

Драйвер кладёт в `evs_pattern_phases` интервал `[started_at, ended_at]`
для каждой фазы. Скрипт [aggregate_patterns.sql](aggregate_patterns.sql)
заново строит `evs_window` (через `\i detect_vacuum_misconfig.sql`),
после чего LEFT JOIN'ом по интервалам приклеивает имя паттерна к
каждому окну. Окна, которые попали в зазор между фазами (служебный
`VACUUM (FREEZE)` плюс паузы для семплирования), помечаются
`pattern IS NULL` и в графики не попадают.

## Вспомогательное: схема evs_window

Скрипт собирает один материализованный CTE — `evs_window` — где для
каждой пары соседних семплов на каждую таблицу посчитаны все нужные
дельты. Это удобный entry-point для исследовательских запросов
вне трёх готовых детекторов: например, можно посмотреть, какая
доля грязных блоков идёт через WAL FPI, или какие таблицы дают
львиную долю `delay_time` в кластере.
