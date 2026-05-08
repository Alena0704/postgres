--
-- sales_mdam_paper (v2): independent column distributions so the
-- MDAM paper test predicates actually match meaningful row counts.
--
-- Each column is computed from a DIFFERENT large prime so that the
-- four columns are statistically independent.  This matches the
-- assumptions of the MDAM paper test queries.
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
    1 + (abs(hashint8(i::bigint)) % 100)                   AS dept,
    '1995-01-01'::date
      + (abs(hashint8(i::bigint + 1000003)) % 400)         AS sdate,
    1 + (abs(hashint8(i::bigint + 2000003)) % 75)          AS item_class,
    1 + (abs(hashint8(i::bigint + 3000017)) % 300)         AS store
FROM generate_series(1, 1000000) i;

CREATE INDEX sales_mdam_paper_idx
ON sales_mdam_paper (dept, sdate, item_class, store);

ANALYZE sales_mdam_paper;

SELECT count(*) AS total_rows FROM sales_mdam_paper;

-- Sanity check: the predicates from the paper queries should now
-- actually hit some rows.
SELECT count(*) AS hit_dept_2 FROM sales_mdam_paper WHERE dept = 2;
SELECT count(*) AS hit_dept_4 FROM sales_mdam_paper WHERE dept = 4;
SELECT count(*) AS hit_dept_5 FROM sales_mdam_paper WHERE dept = 5;
SELECT count(*) AS hit_class_10 FROM sales_mdam_paper WHERE item_class = 10;
SELECT count(*) AS hit_paper_window FROM sales_mdam_paper
  WHERE sdate >= '1995-06-04' AND sdate <= '1995-06-25';
