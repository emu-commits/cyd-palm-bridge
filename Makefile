CC      = cc
CFLAGS  = -std=gnu99 -Wall -O2 -g
# every program goes into build/, so the repo root stays clean; `make roundtrip`
# and the rest still work as names (see PROGS below)
B       = build
CORE    = bridge/pdb.c bridge/datebook.c bridge/address.c bridge/ical.c bridge/vcard.c \
          bridge/tz.c bridge/charset.c bridge/appinfo.c bridge/todo.c bridge/dav_xml.c \
          bridge/find.c bridge/dav_break.c

PROGS   = roundtrip bridge_cli incremental synctoken category bigsync multiapp \
          uidmatch idempotent massdel streamparse find_test calc_test config_test rss_test news_test wx_test \
          feeds_test break_test geoip_test toobig safefile_test sort_test window mget_test fuzz_test course_fuzz rss_asan mget_asan

all: $(addprefix $(B)/,$(filter-out fuzz_test course_fuzz rss_asan mget_asan,$(PROGS)))

# `make wx_test` builds build/wx_test
$(PROGS): %: $(B)/%

dirs:
	@mkdir -p pdb state $(B)

$(B)/roundtrip: tests/roundtrip.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

$(B)/bridge_cli: bridge/main.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

$(B)/incremental: tests/incremental.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

# the mass-delete guard's POSITIVE path -- it fires, deletions are held back,
# and the next sync heals the local database. Plus the negative control (a small
# deletion must still push) without which a guard stuck on would pass. The
# other gates covered this only by staying silent, which is not coverage.
$(B)/massdel: tests/massdel.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

$(B)/synctoken: tests/synctoken.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

$(B)/category: tests/category.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

# built with device working-set sizing (MAXR=96) to prove the streaming engine
# lifts the old 24-record / 8 KB-arena device cap. See tests/bigsync.c.
$(B)/bigsync: tests/bigsync.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -DSYNC_DEVICE_SIZES -o $@ $^

# per-app sync_collection coverage for To Do (VTODO) + Address (vCard) -- the
# exact per-collection path HotSync uses for each app. See tests/multiapp.c.
$(B)/multiapp: tests/multiapp.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

# reconciliation is keyed on the object UID, not the href: href relocation and
# foreign-object edits round-trip without dups. See tests/uidmatch.c.
$(B)/uidmatch: tests/uidmatch.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

# idempotency under real-iCloud behaviors Radicale's happy path misses: etag
# churn + an unresolvable relocation. Built with a TINY OBJ_FETCH_CAP so a bloated
# object overflows the fetch buffer on the host, reproducing the no-PSRAM device
# truncation that used to duplicate records. See tests/idempotent.c.
$(B)/idempotent: tests/idempotent.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -DOBJ_FETCH_CAP=4096 -o $@ $^

# offline unit tests (no server needed)
# sliding-window enumeration parsers == the in-RAM buffer parsers, across window
# boundaries and for the trailing sync-token. Proves the fix that removed the 8 KB
# enumeration truncation. See tests/streamparse.c.
$(B)/streamparse: tests/streamparse.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

$(B)/find_test: tests/find_test.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

$(B)/calc_test: tests/calc_test.c bridge/calc.c | dirs
	$(CC) $(CFLAGS) -o $@ $^ -lm

# dash.c joins the link because the gate now checks the STEP as well as the parse:
# a cache that parses perfectly and never advances is the bug this pair exists for.
$(B)/wx_test: tests/wx_test.c bridge/wxfetch.c bridge/dash.c | dirs
	$(CC) $(CFLAGS) -Ibridge -o $@ tests/wx_test.c bridge/wxfetch.c bridge/dash.c -lm

$(B)/config_test: tests/config_test.c bridge/config.c | dirs
	$(CC) $(CFLAGS) -o $@ $^

# The reply is POSITIONAL, so the query string and the parser are one decision --
# this gate holds them together as well as checking the parse.
$(B)/geoip_test: tests/geoip_test.c bridge/geoip.c | dirs
	$(CC) $(CFLAGS) -o $@ $^

$(B)/rss_test: tests/rss_test.c bridge/rss.c | dirs
	$(CC) $(CFLAGS) -o $@ $^

$(B)/news_test: tests/news_test.c bridge/news.c | dirs
	$(CC) $(CFLAGS) -o $@ $^

# every durable file is replaced whole: the crash states, and the PDB on top
$(B)/safefile_test: tests/safefile_test.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

$(B)/feeds_test: tests/feeds_test.c bridge/feeds.c | dirs
	$(CC) $(CFLAGS) -o $@ $^

$(B)/break_test: tests/break_test.c bridge/dav_break.c | dirs
	$(CC) $(CFLAGS) -o $@ $^

# the engine's sort, in RAM and in runs on the card, against a plain sort (offline)
$(B)/sort_test: tests/sort_test.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

# the multiget reply parser: entities, CDATA, big objects, every truncation (offline)
$(B)/mget_test: tests/mget_test.c bridge/dav_xml.c bridge/dav.c bridge/dav_break.c | dirs
	$(CC) $(CFLAGS) -o $@ $^
# ...and under sanitizers: it reads untrusted network bytes
$(B)/mget_asan: tests/mget_test.c bridge/dav_xml.c bridge/dav.c bridge/dav_break.c | dirs
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $^

# a calendar through a time window: what leaves it leaves the device, never the server
$(B)/window: tests/window.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

$(B)/toobig: tests/toobig.c bridge/dav.c bridge/sync.c $(CORE) | dirs
	$(CC) $(CFLAGS) -o $@ $^

# malformed-input hardening, built with AddressSanitizer + UBSan
$(B)/fuzz_test: tests/fuzz_test.c $(CORE) | dirs
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $^

# the Study course reader eats files off the SD card: truncated, bit-flipped and
# hostile (CRC-fixed) copies of the committed courses, under sanitizers
$(B)/course_fuzz: tests/course_fuzz.c firmware/main/course.c firmware/main/course.h | dirs
	$(CC) $(CFLAGS) -Ifirmware/main -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ tests/course_fuzz.c firmware/main/course.c

# the RSS parser eats untrusted network bytes -> also run its gate under sanitizers
$(B)/rss_asan: tests/rss_test.c bridge/rss.c | dirs
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $^

test: roundtrip find_test calc_test config_test streamparse rss_test news_test wx_test feeds_test break_test geoip_test safefile_test sort_test mget_test
	./$(B)/roundtrip
	./$(B)/find_test
	./$(B)/calc_test
	./$(B)/config_test
	./$(B)/geoip_test
	./$(B)/streamparse
	./$(B)/rss_test
	./$(B)/news_test
	./$(B)/safefile_test
	cd tests && ../$(B)/wx_test
	./$(B)/feeds_test
	./$(B)/break_test
	./$(B)/sort_test
	./$(B)/mget_test

# parser hardening sweep (sanitizer build; a bit slower)
ftest: fuzz_test rss_asan course_fuzz mget_asan
	./$(B)/fuzz_test
	./$(B)/rss_asan
	./$(B)/course_fuzz
	./$(B)/mget_asan

# needs Radicale running on localhost:5232 (see README)
itest: incremental bridge_cli
	./$(B)/incremental

stest: synctoken
	./$(B)/synctoken

ctest: category
	./$(B)/category

# device-sized large-collection stress test (needs Radicale)
btest: bigsync
	./$(B)/bigsync

# per-app (To Do + Address) sync_collection coverage (needs Radicale)
mtest: multiapp
	./$(B)/multiapp

clean:
	rm -rf $(B) pdb/_rt_*.pdb

.PHONY: all dirs test ftest itest stest ctest btest mtest clean $(PROGS)
