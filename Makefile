CC      = cc
CFLAGS  = -std=gnu99 -Wall -O2 -g
CORE    = bridge/pdb.c bridge/datebook.c bridge/address.c bridge/ical.c bridge/vcard.c \
          bridge/tz.c bridge/charset.c bridge/appinfo.c bridge/todo.c bridge/dav_xml.c \
          bridge/find.c

all: roundtrip bridge_cli incremental synctoken category bigsync multiapp \
     find_test calc_test config_test

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

# offline unit tests (no server needed)
find_test: tests/find_test.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

calc_test: tests/calc_test.c bridge/calc.c | dirs
	$(CC) $(CFLAGS) -o $@ $^ -lm

config_test: tests/config_test.c bridge/config.c | dirs
	$(CC) $(CFLAGS) -o $@ $^

# malformed-input hardening, built with AddressSanitizer + UBSan
fuzz_test: tests/fuzz_test.c $(CORE) | dirs
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $^

test: roundtrip find_test calc_test config_test
	./roundtrip
	./find_test
	./calc_test
	./config_test

# parser hardening sweep (sanitizer build; a bit slower)
ftest: fuzz_test
	./fuzz_test

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
	      find_test calc_test config_test pdb/_rt_*.pdb

.PHONY: all dirs test itest stest ctest btest mtest clean
