EXTENSION = plan_trace
EXTVERSION = 1.0

MODULE_big = plan_trace
OBJS = plan_trace.o

DATA = plan_trace--1.0.sql
PGFILEDESC = "plan_trace - trace standard planner (DP/GEQO) plan formation"

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
