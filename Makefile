EXTENSION    = pg_sampletolog
TESTS        = $(wildcard sql/*.sql)
REGRESS      = $(patsubst sql/%.sql,%,$(TESTS))

MODULE_big = pg_sampletolog
OBJS = pg_sampletolog.o


PG_CONFIG = pg_config

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
