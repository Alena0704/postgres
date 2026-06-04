\echo Use "CREATE EXTENSION plan_trace" to load this file. \quit

-- Every joinrel the standard join search formed for the last planned query.
-- source = 'dp' (standard_join_search) or 'geqo' (the winning GEQO tour).
-- left/right are the two inputs of the cheapest join; bushy = both are joins.
CREATE FUNCTION plan_trace_joins(
    OUT id        int,
    OUT relids    text,
    OUT level     int,      -- DP level, or GEQO joinrel size
    OUT source    text,     -- 'dp' | 'geqo'
    OUT est_rows  float8,   -- estimated cardinality
    OUT cost      float8,   -- cheapest_total_path cost
    OUT "left"    text,     -- outer side of the cheapest top join
    OUT "right"   text,     -- inner side
    OUT bushy     bool,     -- both sides are joins (bushy plan)?
    OUT eval_seq  int       -- GEQO tour-eval id (groups a tour's joinrels)
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'plan_trace_joins'
LANGUAGE C VOLATILE;

-- The GEQO genetic search, generation by generation: the two parents, the
-- recombined child (compare its tour against the parents to see inherited
-- traits), the child's fitness and the pool's best fitness.
CREATE FUNCTION plan_trace_geqo(
    OUT generation int,
    OUT momma      text,     -- parent tour (relation aliases in order)
    OUT daddy      text,
    OUT kid        text,     -- recombined child tour
    OUT kid_worth  float8,   -- child fitness (cost)
    OUT best_worth float8    -- pool best so far
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'plan_trace_geqo'
LANGUAGE C VOLATILE;
