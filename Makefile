CC      = cc
CFLAGS  = -std=gnu99 -Wall -O2 -g
# every program goes into build/, so the repo root stays clean; `make roundtrip`
# and the rest still work as names (see PROGS below)
B       = build
CORE    = bridge/pdb.c bridge/appinfo.c bridge/find.c bridge/dav_break.c

# The iCloud sync engine, its DAV client, the Date Book / Address / To Do
# codecs and their gates (some against a local Radicale) were archived on
# 2026-11-07: git tag archive/pim-caldav.
PROGS   = find_test calc_test config_test rss_test news_test wx_test \
          feeds_test break_test geoip_test safefile_test fuzz_test course_fuzz monster_fuzz rss_asan

all: $(addprefix $(B)/,$(filter-out fuzz_test course_fuzz monster_fuzz rss_asan,$(PROGS)))

# `make wx_test` builds build/wx_test
$(PROGS): %: $(B)/%

dirs:
	@mkdir -p pdb state $(B)

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

# malformed-input hardening, built with AddressSanitizer + UBSan
$(B)/fuzz_test: tests/fuzz_test.c $(CORE) | dirs
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $^

# the Study course reader eats files off the SD card: truncated, bit-flipped and
# hostile (CRC-fixed) copies of the committed courses, under sanitizers
$(B)/course_fuzz: tests/course_fuzz.c firmware/main/course.c firmware/main/course.h | dirs
	$(CC) $(CFLAGS) -Ifirmware/main -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ tests/course_fuzz.c firmware/main/course.c

# the monster game's file comes off the SD card too: every truncation, bit
# flips, and hostile (CRC-fixed) copies of the fixture, each one that opens
# then played (steps, battles, a save and a load, and Auto-play), under sanitizers
$(B)/monster_fuzz: tests/monster_fuzz.c firmware/main/monster.c firmware/main/monster.h \
                   firmware/main/monster_play.c firmware/main/monster_play.h \
                   firmware/main/monster_bot.c firmware/main/monster_bot.h bridge/safefile.h | dirs
	$(CC) $(CFLAGS) -Ifirmware/main -Ibridge -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ \
	    tests/monster_fuzz.c firmware/main/monster.c firmware/main/monster_play.c firmware/main/monster_bot.c

# the RSS parser eats untrusted network bytes -> also run its gate under sanitizers
$(B)/rss_asan: tests/rss_test.c bridge/rss.c | dirs
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $^

test: find_test calc_test config_test rss_test news_test wx_test feeds_test break_test geoip_test safefile_test
	./$(B)/find_test
	./$(B)/calc_test
	./$(B)/config_test
	./$(B)/geoip_test
	./$(B)/rss_test
	./$(B)/news_test
	./$(B)/safefile_test
	cd tests && ../$(B)/wx_test
	./$(B)/feeds_test
	./$(B)/break_test

# parser hardening sweep (sanitizer build; a bit slower)
ftest: fuzz_test rss_asan course_fuzz monster_fuzz
	./$(B)/fuzz_test
	./$(B)/rss_asan
	./$(B)/course_fuzz
	MONSTER_SAVE=$(B)/monster_fuzz_save.bin ./$(B)/monster_fuzz

clean:
	rm -rf $(B) pdb/_rt_*.pdb

.PHONY: all dirs test ftest clean $(PROGS)
