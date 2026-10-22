# FreeSWITCH call router
#
#   make                 release build -> ./router
#   make asan            build with AddressSanitizer + UndefinedBehaviorSanitizer -> ./router-asan
#   make test            C unit tests (under ASan/UBSan)
#   make test-db         CLI, SQL and Lua tests (needs PostgreSQL, see docs/OPERATIONS.md)
#   make test-e2e        SIP end-to-end test against the daemon (needs PostgreSQL)
#   make bench           DID allocation benchmark, old vs new (needs pgbench)
#   make lint            cppcheck + shellcheck + luac -p
#   make install         binary, Lua handler and SQL files under $(PREFIX)
#
# WERROR=1 turns warnings into errors (used by CI).

CC      ?= cc
PREFIX  ?= /usr/local
WERROR  ?= 0

PKGS     := libpq json-c libmicrohttpd uuid
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS))

WARN     := -Wall -Wextra -Wformat=2 -Wno-format-truncation -Wno-unused-parameter
ifeq ($(WERROR),1)
WARN     += -Werror
endif
HARDEN   := -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE
CPPFLAGS += -Iinclude -D_GNU_SOURCE $(PKG_CFLAGS)
CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -pthread $(WARN)
LDLIBS   += $(PKG_LIBS) -lreadline -pthread
LDFLAGS  += -pie -Wl,-z,relro,-z,now

SRC      := $(sort $(wildcard src/*.c src/*/*.c))
OBJ      := $(SRC:src/%.c=build/release/%.o)
ASAN_OBJ := $(SRC:src/%.c=build/asan/%.o)
SAN      := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

# Pure modules the unit tests link against (no database, no network).
UNIT_SRC := src/core/safety.c src/sip/sip_parse.c

.PHONY: all asan test test-db test-e2e bench lint install uninstall clean

all: router

router: $(OBJ)
	$(CC) $(CFLAGS) $(HARDEN) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/release/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(HARDEN) -MMD -MP -c $< -o $@

asan: router-asan

router-asan: $(ASAN_OBJ)
	$(CC) $(CFLAGS) $(SAN) -o $@ $^ $(LDLIBS)

build/asan/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 $(SAN) -MMD -MP -c $< -o $@

build/test_units: tests/unit/test_units.c $(UNIT_SRC)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 $(SAN) -o $@ $^

test: build/test_units
	./build/test_units

# The database tests rebuild PGDATABASE: point it at a throwaway database.
test-db: router
	ROUTER_TEST_DB=1 tests/cli/test_cli.sh ./router
	psql -X -q -c 'DROP SCHEMA public CASCADE; CREATE SCHEMA public;' 2>/dev/null
	psql -X -q -v ON_ERROR_STOP=1 -f sql/001_schema.sql -f sql/002_functions.sql -f sql/seed_demo.sql
	tests/sql/test_concurrency.sh
	lua5.4 tests/lua/test_route_handler.lua

test-e2e: router-asan
	ROUTER_TEST_DB=1 python3 tests/e2e/test_sip_flow.py ./router-asan

bench:
	tests/bench/did_alloc_bench.sh 50000 8 10

lint:
	cppcheck --quiet --error-exitcode=1 --enable=warning,portability --inline-suppr \
	    --std=c11 -D_GNU_SOURCE -Iinclude --suppress=missingIncludeSystem src
	shellcheck tests/sql/*.sh tests/bench/*.sh tests/cli/*.sh tools/*.sh
	luac5.4 -p freeswitch/scripts/route_handler.lua tests/lua/test_route_handler.lua

install: router
	install -Dm755 router $(DESTDIR)$(PREFIX)/bin/freeswitch-router
	install -Dm644 freeswitch/scripts/route_handler.lua $(DESTDIR)$(PREFIX)/share/freeswitch-router/route_handler.lua
	install -Dm644 -t $(DESTDIR)$(PREFIX)/share/freeswitch-router/sql sql/*.sql

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/freeswitch-router
	rm -rf $(DESTDIR)$(PREFIX)/share/freeswitch-router

clean:
	rm -rf build router router-asan

-include $(OBJ:.o=.d) $(ASAN_OBJ:.o=.d)
