CC      = cc
CFLAGS  = -std=gnu99 -Wall -O2 -g
CORE    = bridge/pdb.c bridge/datebook.c bridge/address.c bridge/ical.c bridge/vcard.c \
          bridge/tz.c bridge/charset.c bridge/appinfo.c bridge/todo.c bridge/dav_xml.c \
          bridge/find.c

all: roundtrip bridge_cli incremental synctoken category bigsync multiapp \
     uidmatch idempotent streamparse find_test calc_test config_test rss_test news_test \
     feeds_test

dirs:
	@mkdir -p pdb state

roundtrip: tests/roundtrip.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

bridge_cli: bridge/main.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

incremental: tests/incremental.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

synctoken: tests/synctoken.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

category: tests/category.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

# built with device working-set sizing (MAXR=96) to prove the streaming engine
# lifts the old 24-record / 8 KB-arena device cap. See tests/bigsync.c.
bigsync: tests/bigsync.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -DSYNC_DEVICE_SIZES -o $@ $^

# per-app sync_collection coverage for To Do (VTODO) + Address (vCard) -- the
# exact per-collection path HotSync uses for each app. See tests/multiapp.c.
multiapp: tests/multiapp.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

# reconciliation is keyed on the object UID, not the href: href relocation and
# foreign-object edits round-trip without dups. See tests/uidmatch.c.
uidmatch: tests/uidmatch.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

# idempotency under real-iCloud behaviors Radicale's happy path misses: etag
# churn + an unresolvable relocation. Built with a TINY OBJ_FETCH_CAP so a bloated
# object overflows the fetch buffer on the host, reproducing the no-PSRAM device
# truncation that used to duplicate records. See tests/idempotent.c.
idempotent: tests/idempotent.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -DOBJ_FETCH_CAP=4096 -o $@ $^

# offline unit tests (no server needed)
# sliding-window enumeration parsers == the in-RAM buffer parsers, across window
# boundaries and for the trailing sync-token. Proves the fix that removed the 8 KB
# enumeration truncation. See tests/streamparse.c.
streamparse: tests/streamparse.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

find_test: tests/find_test.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

calc_test: tests/calc_test.c bridge/calc.c | dirs
	$(CC) $(CFLAGS) -o $@ $^ -lm

config_test: tests/config_test.c bridge/config.c | dirs
	$(CC) $(CFLAGS) -o $@ $^

rss_test: tests/rss_test.c bridge/rss.c | dirs
	$(CC) $(CFLAGS) -o $@ $^

news_test: tests/news_test.c bridge/news.c | dirs
	$(CC) $(CFLAGS) -o $@ $^

feeds_test: tests/feeds_test.c bridge/feeds.c | dirs
	$(CC) $(CFLAGS) -o $@ $^

# malformed-input hardening, built with AddressSanitizer + UBSan
fuzz_test: tests/fuzz_test.c $(CORE) | dirs
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $^

# the RSS parser eats untrusted network bytes -> also run its gate under sanitizers
rss_asan: tests/rss_test.c bridge/rss.c | dirs
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $^

test: roundtrip find_test calc_test config_test streamparse rss_test news_test feeds_test
	./roundtrip
	./find_test
	./calc_test
	./config_test
	./streamparse
	./rss_test
	./news_test
	./feeds_test

# parser hardening sweep (sanitizer build; a bit slower)
ftest: fuzz_test rss_asan
	./fuzz_test
	./rss_asan

# needs Radicale running on localhost:5232 (see README)
itest: incremental bridge_cli
	./incremental

stest: synctoken
	./synctoken

ctest: category
	./category

# device-sized large-collection stress test (needs Radicale)
btest: bigsync
	./bigsync

# per-app (To Do + Address) sync_collection coverage (needs Radicale)
mtest: multiapp
	./multiapp

clean:
	rm -f roundtrip bridge_cli incremental synctoken category bigsync multiapp \
	      uidmatch idempotent streamparse find_test calc_test config_test fuzz_test \
	      rss_test rss_asan news_test feeds_test pdb/_rt_*.pdb

.PHONY: all dirs test itest stest ctest btest mtest clean
