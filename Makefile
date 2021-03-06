EXTENSION    = pg_sampletolog
EXTVERSION = $(shell grep default_version $(EXTENSION).control | sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")
TESTS        = $(wildcard sql/*.sql)
REGRESS      = $(patsubst sql/%.sql,%,$(TESTS))

MODULE_big = pg_sampletolog
OBJS = pg_sampletolog.o

all:

release-zip: all
	git archive --format zip --prefix=$(EXTENSION)-${EXTVERSION}/ --output ./$(EXTENSION)-${EXTVERSION}.zip HEAD
	unzip ./$(EXTENSION)-$(EXTVERSION).zip
	rm ./$(EXTENSION)-$(EXTVERSION).zip
	rm ./$(EXTENSION)-$(EXTVERSION)/.gitignore
	sed -i -e "s/__VERSION__/$(EXTVERSION)/g"  ./$(EXTENSION)-$(EXTVERSION)/META.json
	zip -r ./$(EXTENSION)-$(EXTVERSION).zip ./$(EXTENSION)-$(EXTVERSION)/
	rm ./$(EXTENSION)-$(EXTVERSION) -rf


PG_CONFIG = pg_config

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

DEBUILD_ROOT = /tmp/$(EXTENSION)

deb: release-zip
		mkdir -p $(DEBUILD_ROOT) && rm -rf $(DEBUILD_ROOT)/*
		unzip ./${EXTENSION}-$(EXTVERSION).zip -d $(DEBUILD_ROOT)
		cd $(DEBUILD_ROOT)/${EXTENSION}-$(EXTVERSION) && make -f debian/rules orig
		cd $(DEBUILD_ROOT)/${EXTENSION}-$(EXTVERSION) && debuild -us -uc -sa

