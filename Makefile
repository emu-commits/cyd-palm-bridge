CC      = cc
CFLAGS  = -std=gnu99 -Wall -O2 -g
CORE    = bridge/pdb.c bridge/datebook.c bridge/address.c bridge/ical.c bridge/vcard.c \
          bridge/tz.c bridge/charset.c bridge/appinfo.c bridge/todo.c bridge/dav_xml.c

all: roundtrip bridge_cli incremental synctoken category

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

test: roundtrip
	./roundtrip

# needs Radicale running on localhost:5232 (see README)
itest: incremental bridge_cli
	./incremental

stest: synctoken
	./synctoken

ctest: category
	./category

clean:
	rm -f roundtrip bridge_cli incremental synctoken category pdb/_rt_*.pdb

.PHONY: all dirs test itest stest ctest clean
