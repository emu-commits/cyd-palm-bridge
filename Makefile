CC      = cc
CFLAGS  = -std=gnu99 -Wall -O2 -g
CORE    = bridge/pdb.c bridge/datebook.c bridge/address.c bridge/ical.c bridge/vcard.c \
          bridge/tz.c bridge/charset.c bridge/appinfo.c bridge/todo.c bridge/dav_xml.c

all: roundtrip bridge_cli incremental synctoken category bigsync multiapp

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

test: roundtrip
	./roundtrip

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
	rm -f roundtrip bridge_cli incremental synctoken category bigsync multiapp pdb/_rt_*.pdb

.PHONY: all dirs test itest stest ctest btest mtest clean
