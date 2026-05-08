--
-- Final pass/fail verdict for the MDAM paper test queries.
-- Each row prints ok/FAIL based on (count, ordered md5) agreement
-- between MDAM mode and the seqscan oracle.
--

\pset format aligned
\pset border 1

CREATE OR REPLACE FUNCTION mdam_test_verdict(label text, sql_select text)
RETURNS TABLE(test text, mdam_rows bigint, oracle_rows bigint,
              md5_match boolean, plan_first_node text, verdict text)
AS $$
DECLARE
    mdam_count bigint;  mdam_md5 text;  mdam_plan text;
    oracle_count bigint; oracle_md5 text;
BEGIN
    -- MDAM mode
    EXECUTE 'SET LOCAL enable_mdam = on';
    EXECUTE 'SET LOCAL enable_seqscan = off';
    EXECUTE 'SET LOCAL enable_bitmapscan = off';
    EXECUTE 'SET LOCAL enable_indexscan = on';
    EXECUTE 'SET LOCAL enable_indexonlyscan = on';
    EXECUTE
      'SELECT count(*), md5(string_agg(d::text || '','' || s::text || '','' || c::text || '','' || st::text, ''|''))
       FROM (' || sql_select || ') t(d, s, c, st)'
    INTO mdam_count, mdam_md5;
    EXECUTE 'EXPLAIN (FORMAT TEXT) ' || sql_select INTO mdam_plan;

    -- Oracle (seqscan)
    EXECUTE 'SET LOCAL enable_mdam = off';
    EXECUTE 'SET LOCAL enable_indexscan = off';
    EXECUTE 'SET LOCAL enable_indexonlyscan = off';
    EXECUTE 'SET LOCAL enable_seqscan = on';
    EXECUTE 'SET LOCAL enable_bitmapscan = off';
    EXECUTE
      'SELECT count(*), md5(string_agg(d::text || '','' || s::text || '','' || c::text || '','' || st::text, ''|''))
       FROM (' || sql_select || ') t(d, s, c, st)'
    INTO oracle_count, oracle_md5;

    RETURN QUERY SELECT
        label,
        mdam_count,
        oracle_count,
        (mdam_md5 IS NOT DISTINCT FROM oracle_md5),
        regexp_replace(split_part(mdam_plan, E'\n', 1), '\(cost.*\)', '', 'g'),
        CASE
          WHEN mdam_count = oracle_count
               AND (mdam_md5 IS NOT DISTINCT FROM oracle_md5)
            THEN 'PASS'
          ELSE 'FAIL'
        END;
END;
$$ LANGUAGE plpgsql;

\echo '=================================================================='
\echo '== MDAM PAPER REGRESSION VERDICT                                =='
\echo '=================================================================='

WITH cases(label, q) AS (VALUES
    ('T1 paper relaxed (dept IN, IN)',
     'SELECT dept, sdate, item_class, store FROM sales_mdam_paper
      WHERE dept IN (2,4,5) AND item_class IN (5,10)
      ORDER BY dept, sdate, item_class, store'),
    ('T2 paper FULL flagship',
     'SELECT dept, sdate, item_class, store FROM sales_mdam_paper
      WHERE ((item_class = 10 AND sdate >= ''1995-06-04'' AND sdate <= ''1995-06-25'')
             OR dept IN (2,4,5))
        AND ((dept = 4 AND item_class = 5)
             OR (item_class IN (5,10) AND (sdate = ''1995-06-04'' OR dept = 2)))
      ORDER BY dept, sdate, item_class, store'),
    ('T3 skip-scan range',
     'SELECT dept, sdate, item_class, store FROM sales_mdam_paper
      WHERE (dept = 1 AND sdate < ''1995-02-01'') OR (dept > 50)
      ORDER BY dept, sdate, item_class, store'),
    ('T4 cross-column SAOP',
     'SELECT dept, sdate, item_class, store FROM sales_mdam_paper
      WHERE (dept = 5 OR dept = 10) AND (item_class = 3 OR item_class = 7)
      ORDER BY dept, sdate, item_class, store'),
    ('T5 range shattering',
     'SELECT dept, sdate, item_class, store FROM sales_mdam_paper
      WHERE (dept BETWEEN 4 AND 7 AND item_class IN (5, 10, 15))
         OR (dept BETWEEN 9 AND 11 AND item_class IN (7, 12))
      ORDER BY dept, sdate, item_class, store'),
    ('T6 ordering-conflict (fallback)',
     'SELECT dept, sdate, item_class, store FROM sales_mdam_paper
      WHERE dept > 10 AND sdate = ''1995-03-01''
        AND (item_class = 5 OR store = 50)
      ORDER BY dept, sdate, item_class, store'),
    ('T7 ordering-conflict later col (fallback)',
     'SELECT dept, sdate, item_class, store FROM sales_mdam_paper
      WHERE dept = 10 AND sdate > ''1995-02-01''
        AND (item_class = 5 OR store = 50)
      ORDER BY dept, sdate, item_class, store'),
    ('T8 no-conflict same-col OR+ineq',
     'SELECT dept, sdate, item_class, store FROM sales_mdam_paper
      WHERE dept > 10 AND sdate = ''1995-03-01''
        AND (store = 5 OR store = 50)
      ORDER BY dept, sdate, item_class, store'),
    ('T9 no-conflict point leading',
     'SELECT dept, sdate, item_class, store FROM sales_mdam_paper
      WHERE dept = 10 AND sdate = ''1995-03-01''
        AND (item_class = 5 OR store = 50)
      ORDER BY dept, sdate, item_class, store'),
    ('T10 backward DESC',
     'SELECT dept, sdate, item_class, store FROM sales_mdam_paper
      WHERE (dept = 5 AND item_class = 10) OR (dept = 10 AND sdate = ''1995-06-04'')
      ORDER BY dept DESC, sdate DESC, item_class DESC, store DESC'),
    ('T11 SQL docs',
     'SELECT dept, sdate, item_class, store FROM sales_mdam_paper
      WHERE dept = 4 AND (sdate = ''1995-06-01'' OR sdate = ''1995-06-15'')
        AND item_class IN (5,10)
      ORDER BY dept, sdate, item_class, store')
)
SELECT v.test, v.mdam_rows, v.oracle_rows, v.md5_match,
       v.plan_first_node, v.verdict
FROM cases, mdam_test_verdict(cases.label, cases.q) v;
