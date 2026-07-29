PREFIX ?= $(HOME)/pginstall
PG_CONFIG ?= $(PREFIX)/bin/pg_config

.PHONY: build-pg build-ext build test benchmark clean

build: build-pg build-ext

build-pg:
	scripts/build-postgres.sh

build-ext:
	cd extension/xp_batch && $(MAKE) PG_CONFIG=$(PG_CONFIG)
	cd extension/xp_batch && $(MAKE) install PG_CONFIG=$(PG_CONFIG)

test:
	scripts/run-smoke-tests.sh

benchmark:
	scripts/run-all-experiments.sh

clean:
	cd extension/xp_batch && $(MAKE) clean PG_CONFIG=$(PG_CONFIG) 2>/dev/null || true
