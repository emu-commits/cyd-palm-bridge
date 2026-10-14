#!/usr/bin/env python3
"""probe_partial_test.py -- tools/probe_partial_card.py, offline and live.

Offline: the probe refuses every method but PROPFIND and REPORT; vCards are
unfolded and compared by property name; each verdict comes out where it
should (a server that honours partial address-data, one that changes the
item1. groups, one that ignores it, one that refuses it).

Live, when $PROBE_BASE names a local Radicale (tests/run_gates.sh sets it):
two contacts, one with a 30 KB photo and an item1. phone, put in a fresh
address book; the probe discovers it, measures it and compares both replies.
Radicale ignores partial address-data, so the verdict is IGNORED. The probe
itself never writes: only this test's setup does, and only to localhost.
"""
import base64
import json
import os
import subprocess
import sys
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import probe_partial_card as pc  # noqa: E402

fails = 0


def ck(cond, what):
    global fails
    print("  %s: %s" % ("ok" if cond else "FAIL", what))
    if not cond:
        fails += 1


def card(*lines):
    return "BEGIN:VCARD\r\n" + "".join(l + "\r\n" for l in lines) + "END:VCARD\r\n"


PHOTO = "PHOTO;ENCODING=b;TYPE=JPEG:" + "QUJD" * 40 + "\r\n " + "REVG" * 40
FULL = card("VERSION:3.0", "UID:a", "N:Doe;Jane;;;", "FN:Jane Doe",
            "item1.TEL;type=CELL:+1 555 0100", "item1.X-ABLabel:_$!<Mobile>!$_",
            "EMAIL:j@example.com", "BDAY:1980-01-01", PHOTO)
PART = card("VERSION:3.0", "UID:a", "N:Doe;Jane;;;", "FN:Jane Doe",
            "item1.TEL;type=CELL:+1 555 0100", "EMAIL:j@example.com")
PART_NOGROUP = card("VERSION:3.0", "UID:a", "N:Doe;Jane;;;", "FN:Jane Doe",
                    "TEL;type=CELL:+1 555 0100", "EMAIL:j@example.com")


def offline():
    print("-- offline --")
    p = pc.Probe("u", "p")
    for m in ("PUT", "DELETE", "MOVE", "POST", "PROPPATCH", "MKCOL", "GET"):
        try:
            p.request(m, "http://localhost:1/", "")
            ck(False, "%s refused" % m)
        except RuntimeError as e:
            ck("refused" in str(e), "%s refused before anything is sent" % m)

    ck(pc.unfold("A:1\r\n 2\r\nB:3\n\t4\n") == ["A:12", "B:34"], "folded lines unfold (CR LF and LF)")
    ck(pc.prop_of("item1.TEL;type=CELL:+1") == ("item1", "TEL"), "a grouped property's group and name")
    ck(pc.prop_of("x-ablabel:foo") == ("", "X-ABLABEL"), "names compare in capitals")

    r = pc.analyse(FULL, PART)
    ck(r["full_photo"] and not r["part_photo"], "the photo is seen in the full card only")
    ck(r["palm_exact"] and r["part_extra"] == [], "the Palm's lines match, nothing extra")
    ck(r["full_grouped"] == 1 and r["part_grouped"] == 1, "the item1. phone counted on both sides")
    ck(pc.verdict([r], 207)[0] == "HONOURED", "verdict: HONOURED")

    r2 = pc.analyse(FULL, PART_NOGROUP)
    ck(not r2["palm_exact"] and r2["palm_nogroup"], "the groups dropped: equal only ungrouped")
    ck(pc.verdict([r2], 207)[0] == "HONOURED, GROUPS CHANGED", "verdict: HONOURED, GROUPS CHANGED")

    r3 = pc.analyse(FULL, FULL)
    ck(r3["part_photo"] and "PHOTO" in r3["part_extra"], "a whole card back: the photo and BDAY are extra")
    ck(pc.verdict([r3], 207)[0] == "IGNORED", "verdict: IGNORED")

    r4 = pc.analyse(FULL, card("VERSION:3.0", "UID:a", "FN:Jane Doe"))
    ck(r4["missing"] == ["EMAIL", "N", "TEL"], "the Palm's properties that didn't come back, by name")
    ck(pc.verdict([r4], 207)[0] == "DIFFERENT", "verdict: DIFFERENT")
    ck(pc.verdict([], 403)[0] == "REFUSED", "verdict: REFUSED on a non-207")

    body = pc.multiget_body(["/a/b c.vcf"], True)
    ck('<c:prop name="TEL"/>' in body and '<c:prop name="PHOTO"/>' not in body,
       "the partial request names the Palm's properties, and not PHOTO")
    ck("<d:href>/a/b c.vcf</d:href>" in body, "hrefs go in as they are")
    ck(pc.PALM_PROPS == ["UID", "VERSION", "N", "FN", "ORG", "TITLE", "TEL", "EMAIL", "ADR", "NOTE"],
       "the property list is objfile.c's RD_CARD")
    rd = open(os.path.join(ROOT, "bridge", "objfile.c")).read()
    ck('RD_CARD[] = {"UID","VERSION","N","FN","ORG","TITLE","TEL","EMAIL",\n'
       '                                      "ADR","NOTE",NULL};' in rd,
       "objfile.c's RD_CARD is still that list")


def put(base, path, user, pw, body, ctype):
    host = urllib.parse.urlsplit(base).hostname
    assert host in ("localhost", "127.0.0.1"), "setup writes to localhost only"
    req = urllib.request.Request(urllib.parse.urljoin(base, path), data=body.encode(), method="PUT")
    req.add_header("Content-Type", ctype)
    req.add_header("Authorization", "Basic " + base64.b64encode(("%s:%s" % (user, pw)).encode()).decode())
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code


def live(base, user, pw):
    print("-- live, against %s --" % base)
    book = "/%s/probe-%d/" % (user, os.getpid())
    req = urllib.request.Request(urllib.parse.urljoin(base, book), method="MKCOL", data=(
        '<D:mkcol xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:carddav"><D:set><D:prop>'
        "<D:resourcetype><D:collection/><C:addressbook/></D:resourcetype>"
        "</D:prop></D:set></D:mkcol>").encode())
    req.add_header("Authorization", "Basic " + base64.b64encode(("%s:%s" % (user, pw)).encode()).decode())
    req.add_header("Content-Type", "application/xml")
    assert urllib.parse.urlsplit(base).hostname in ("localhost", "127.0.0.1")
    with urllib.request.urlopen(req, timeout=10) as r:
        ck(r.status == 201, "a fresh address book")
    big = base64.b64encode(os.urandom(30000)).decode()
    photo = "PHOTO;ENCODING=b;TYPE=JPEG:" + "\r\n ".join(big[i:i + 72] for i in range(0, len(big), 72))
    ck(put(base, book + "a.vcf", user, pw, card("VERSION:3.0", "UID:a", "N:Doe;Jane;;;", "FN:Jane Doe",
                                                "item1.TEL;type=CELL:+1 555 0100",
                                                "item1.X-ABLabel:_$!<Mobile>!$_", photo),
           "text/vcard") == 201, "a contact with a 30 KB photo")
    ck(put(base, book + "b.vcf", user, pw, card("VERSION:3.0", "UID:b", "N:Roe;Rick;;;", "FN:Rick Roe",
                                                "TEL:+1 555 0200"), "text/vcard") == 201,
       "a contact without one")

    env = dict(os.environ, PROBE_PASS=pw)
    out = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "probe_partial_card.py"),
                          "--user", user, "--book", urllib.parse.urljoin(base, book), "--json"],
                         env=env, capture_output=True, text=True, timeout=60)
    ck(out.returncode == 0, "the probe ran (rc %d) %s" % (out.returncode, out.stderr.strip()[-200:]))
    r = json.loads(out.stdout)
    ck(r.get("contacts") == 2 and r.get("compared") == 2, "both contacts listed and compared")
    s = r.get("sizes_from_listing") or {}
    ck(s.get("over_16KB") == 1 and s.get("over_8KB") == 1, "the photo contact counted over 8 and 16 KB")
    ck(r.get("with_photo") == 1, "one contact with a photo")
    ck(r.get("verdict") == "IGNORED", "Radicale ignores partial address-data: IGNORED")
    ck(r["rows"][0]["full_bytes"] > 30000, "the biggest contact first")
    ck("Jane" not in out.stdout and "555" not in out.stdout, "the report holds no names or numbers")

    out = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "probe_partial_card.py"),
                          "--user", user, "--base", base], env=env,
                         capture_output=True, text=True, timeout=60)
    ck(out.returncode == 0 and "address books found" in out.stdout and "VERDICT" in out.stdout,
       "discovery from the server's root finds the address books")

    env["PROBE_PASS"] = "wrong"
    out = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "probe_partial_card.py"),
                          "--user", user, "--base", base], env=env,
                         capture_output=True, text=True, timeout=60)
    ck(out.returncode == 2 and "401" in out.stdout, "a wrong password: a clear error, rc 2")


def main():
    offline()
    base = os.environ.get("PROBE_BASE")
    if base:
        live(base, os.environ.get("PROBE_USER", "palm"), os.environ.get("PROBE_PW", "palm"))
    else:
        print("-- live: skipped (no $PROBE_BASE) --")
    print("probe_partial_test: %s" % ("OK" if not fails else "%d FAILED" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
