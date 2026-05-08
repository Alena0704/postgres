--
-- Reload sales_mdam_paper with N rows.
-- Pass :rows on the command line (psql -v rows=100000 ...).
--

DROP TABLE IF EXISTS sales_mdam_paper;
CREATE TABLE sales_mdam_paper (
    dept       int,
    sdate      date,
    item_class int,
    store      int
);

INSERT INTO sales_mdam_paper
SELECT
    1 + (abs(hashint8(i::bigint)) % 100)               AS dept,
    '1995-01-01'::date
      + (abs(hashint8(i::bigint + 1000003)) % 400)     AS sdate,
    1 + (abs(hashint8(i::bigint + 2000003)) % 75)      AS item_class,
    1 + (abs(hashint8(i::bigint + 3000017)) % 300)     AS store
FROM generate_series(1, :rows) i;

CREATE INDEX sales_mdam_paper_idx
ON sales_mdam_paper (dept, sdate, item_class, store);

ANALYZE sales_mdam_paper;

DROP TABLE IF EXISTS sales_other;
CREATE TABLE sales_other (
    dept int,
    item_class int,
    rev numeric(10,2)
);
INSERT INTO sales_other
SELECT
    1 + (abs(hashint8(i::bigint + 4000003)) % 100),
    1 + (abs(hashint8(i::bigint + 5000003)) % 75),
    (abs(hashint8(i::bigint + 6000003)) % 100000) / 100.0
FROM generate_series(1, (:rows / 5)) i;
CREATE INDEX sales_other_idx ON sales_other (dept, item_class);
ANALYZE sales_other;

SELECT 'sales_mdam_paper' AS rel, count(*) FROM sales_mdam_paper
UNION ALL
SELECT 'sales_other', count(*) FROM sales_other;
