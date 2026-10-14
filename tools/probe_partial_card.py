#!/usr/bin/env python3
"""probe_partial_card.py -- does the address book honour partial address-data?

A read-only probe, run on a computer against a real CardDAV account (iCloud).
It answers the two questions that decide whether the device can stop
downloading contact photos (docs/BACKLOG.md, Engine 8):

  1. Asked for only the properties the Palm reads (RFC 6352 §10.4.2), does
     the server leave PHOTO and everything else out?
  2. Does what comes back still carry Apple's grouped properties
     ("item1.TEL"), exactly as in the full contact?

It also measures the address book: how many contacts, how big, and how many
are over the device's 8 KB fetch buffer and 16 KB PUT limit, which is what a
full resync of the device costs.

READ-ONLY. It sends PROPFIND and REPORT and nothing else (`request()` refuses
any other method), so it can't change or delete anything in the account. The
report holds counts, sizes and property names, never a name, number or
address, so it can be pasted into an issue as it is.

Usage:
  python3 tools/probe_partial_card.py --user you@icloud.com
  python3 tools/probe_partial_card.py --user you@icloud.com --all

The password is an app-specific password (appleid.apple.com, Sign-In and
Security); it's asked for, or read from $PROBE_PASS. Python 3.8+, standard
library only.
"""
import argparse
import base64
import getpass
import json
import os
import re
import statistics
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

DAV = "DAV:"
CARD = "urn:ietf:params:xml:ns:carddav"
NS = {"d": DAV, "c": CARD}

# What the device reads from a vCard: bridge/objfile.c, RD_CARD. Keep in step.
PALM_PROPS = ["UID", "VERSION", "N", "FN", "ORG", "TITLE", "TEL", "EMAIL", "ADR", "NOTE"]

FETCH_CAP = 8 * 1024     # the device's object buffer (a Palm view must fit)
PUT_MAX = 16 * 1024      # the most the device can send today (bridge/sync.c)
ALLOWED = ("PROPFIND", "REPORT")


class Probe:
    def __init__(self, user, password, timeout=60):
        tok = base64.b64encode(("%s:%s" % (user, password)).encode()).decode()
        self.auth = "Basic " + tok
        self.timeout = timeout
        self.auth_host = None

    def request(self, method, url, body, depth=None):
        """One request, following redirects with the same method and body.
        Returns (status, bytes, final url)."""
        if method not in ALLOWED:
            raise RuntimeError("refused: %s (this probe only reads)" % method)
        data = body.encode("utf-8")
        for _ in range(6):
            host = urllib.parse.urlsplit(url).hostname or ""
            if self.auth_host is None:
                self.auth_host = host
            req = urllib.request.Request(url, data=data, method=method)
            req.add_header("Content-Type", 'application/xml; charset="utf-8"')
            if depth is not None:
                req.add_header("Depth", str(depth))
            # the credentials go only to the host they were given for, or
            # one of its siblings (iCloud moves an account to pNN-contacts)
            if host == self.auth_host or (host.endswith(".icloud.com")
                                          and self.auth_host.endswith(".icloud.com")):
                req.add_header("Authorization", self.auth)
            try:
                with _NoRedirect.open(req, timeout=self.timeout) as r:
                    return r.status, r.read(), url
            except urllib.error.HTTPError as e:
                if e.code in (301, 302, 307, 308) and e.headers.get("Location"):
                    url = urllib.parse.urljoin(url, e.headers["Location"])
                    continue
                return e.code, e.read(), url
        raise RuntimeError("too many redirects")


class _KeepRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None          # handled by Probe.request, method and body kept


_NoRedirect = urllib.request.build_opener(_KeepRedirects)


def propfind_body(props):
    return ('<?xml version="1.0" encoding="utf-8"?>'
            '<d:propfind xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:carddav">'
            "<d:prop>%s</d:prop></d:propfind>" % props)


def multiget_body(hrefs, partial):
    if partial:
        data = "<c:address-data>%s</c:address-data>" % "".join(
            '<c:prop name="%s"/>' % p for p in PALM_PROPS)
    else:
        data = "<c:address-data/>"
    return ('<?xml version="1.0" encoding="utf-8"?>'
            '<c:addressbook-multiget xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:carddav">'
            "<d:prop><d:getetag/>%s</d:prop>%s</c:addressbook-multiget>"
            % (data, "".join("<d:href>%s</d:href>" % esc(h) for h in hrefs)))


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def responses(xml_bytes):
    try:
        root = ET.fromstring(xml_bytes)
    except ET.ParseError as e:
        raise RuntimeError("the server's reply isn't XML (%s)" % e)
    return root.findall("d:response", NS)


def href_text(resp, base_url):
    h = resp.findtext("d:href", default="", namespaces=NS).strip()
    return urllib.parse.urljoin(base_url, h)


def discover(p, start):
    """The address books under the account: [(url, name)]."""
    st, body, url = p.request("PROPFIND", start, propfind_body("<d:current-user-principal/>"), 0)
    if st == 401:
        raise RuntimeError("sign-in refused (401): check the Apple ID and the app-specific password")
    if st != 207:
        raise RuntimeError("current-user-principal: HTTP %d" % st)
    pr = None
    for r in responses(body):
        e = r.find(".//d:current-user-principal/d:href", NS)
        if e is not None and e.text:
            pr = urllib.parse.urljoin(url, e.text.strip())
    if not pr:
        raise RuntimeError("no principal in the reply")
    st, body, url = p.request("PROPFIND", pr, propfind_body("<c:addressbook-home-set/>"), 0)
    if st != 207:
        raise RuntimeError("addressbook-home-set: HTTP %d" % st)
    home = None
    for r in responses(body):
        e = r.find(".//c:addressbook-home-set/d:href", NS)
        if e is not None and e.text:
            home = urllib.parse.urljoin(url, e.text.strip())
    if not home:
        raise RuntimeError("no address book home in the reply")
    st, body, url = p.request("PROPFIND", home,
                              propfind_body("<d:resourcetype/><d:displayname/>"), 1)
    if st != 207:
        raise RuntimeError("listing the address books: HTTP %d" % st)
    books = []
    for r in responses(body):
        if r.find(".//d:resourcetype/c:addressbook", NS) is not None:
            books.append((href_text(r, url),
                          r.findtext(".//d:displayname", default="", namespaces=NS)))
    return books


def members(p, book):
    """[(href, etag, length or None)] for every contact in the book."""
    st, body, url = p.request("PROPFIND", book,
                              propfind_body("<d:getetag/><d:getcontentlength/><d:resourcetype/>"), 1)
    if st != 207:
        raise RuntimeError("listing the contacts: HTTP %d" % st)
    out = []
    for r in responses(body):
        href = href_text(r, url)
        if href.rstrip("/") == book.rstrip("/"):
            continue
        if r.find(".//d:resourcetype/d:collection", NS) is not None:
            continue
        ln = r.findtext(".//d:getcontentlength", default="", namespaces=NS).strip()
        out.append((href, r.findtext(".//d:getetag", default="", namespaces=NS).strip(),
                    int(ln) if ln.isdigit() else None))
    return out


def multiget(p, book, hrefs, partial):
    """{href: (etag, vcard text)}, the status, the bytes on the wire and the
    seconds it took."""
    paths = [urllib.parse.urlsplit(h).path for h in hrefs]
    t0 = time.monotonic()
    st, body, url = p.request("REPORT", book, multiget_body(paths, partial), 1)
    secs = time.monotonic() - t0
    got = {}
    if st == 207:
        for r in responses(body):
            data = r.find(".//c:address-data", NS)
            if data is None:
                continue
            got[href_text(r, url)] = (r.findtext(".//d:getetag", default="", namespaces=NS).strip(),
                                      data.text or "")
    return got, st, len(body), secs


def unfold(text):
    """The vCard's logical lines (RFC 6350 §3.2), CR LF or bare LF."""
    lines = []
    for raw in re.split(r"\r?\n", text):
        if raw[:1] in (" ", "\t") and lines:
            lines[-1] += raw[1:]
        elif raw:
            lines.append(raw)
    return lines


def prop_of(line):
    """(group or '', NAME) of a logical line."""
    m = re.match(r"([^:;]*)", line)
    name = m.group(1) if m else ""
    group, _, bare = name.rpartition(".")
    return group, bare.upper()


def palm_lines(lines, strip_groups=False):
    out = []
    for ln in lines:
        g, n = prop_of(ln)
        if n in PALM_PROPS:
            out.append(ln.split(".", 1)[1] if strip_groups and g else ln)
    return out


def analyse(full, part):
    """Compare one contact's full and partial bodies; names only, no values."""
    fl, pl = unfold(full), unfold(part)
    fprops = [prop_of(x)[1] for x in fl]
    pprops = [prop_of(x)[1] for x in pl]
    extra = sorted(set(pprops) - set(PALM_PROPS) - {"BEGIN", "END"})
    return {
        "full_bytes": len(full.encode("utf-8")),
        "part_bytes": len(part.encode("utf-8")),
        "full_photo": "PHOTO" in fprops,
        "part_photo": "PHOTO" in pprops,
        "part_extra": extra,
        "full_grouped": sum(1 for x in fl if prop_of(x)[0] and prop_of(x)[1] in PALM_PROPS),
        "part_grouped": sum(1 for x in pl if prop_of(x)[0] and prop_of(x)[1] in PALM_PROPS),
        "palm_exact": palm_lines(fl) == palm_lines(pl),
        "palm_nogroup": palm_lines(fl, True) == palm_lines(pl, True),
        "palm_view_bytes": sum(len(x) + 2 for x in palm_lines(fl)),
        "missing": sorted(set(p for p in fprops if p in PALM_PROPS) - set(pprops)),
    }


def verdict(rows, part_status):
    if part_status != 207:
        return "REFUSED", "the partial request was answered HTTP %d" % part_status
    if not rows:
        return "NO DATA", "no contact came back in both replies"
    if all(r["part_bytes"] == r["full_bytes"] for r in rows) or any(r["part_photo"] for r in rows):
        return "IGNORED", "the server sent whole contacts, photos included"
    if all(r["palm_exact"] and not r["part_extra"] for r in rows):
        return "HONOURED", "only the Palm's properties came back, exactly as in the full contacts"
    if all(r["palm_nogroup"] and not r["part_extra"] for r in rows):
        return "HONOURED, GROUPS CHANGED", ("only the Palm's properties came back, but the "
                                            "item1.-style groups differ from the full contacts")
    return "DIFFERENT", "the partial contacts don't match the full ones (see the rows)"


def batches(seq, n):
    for i in range(0, len(seq), n):
        yield seq[i:i + n]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--user", required=True, help="Apple ID (or the CardDAV user)")
    ap.add_argument("--base", default="https://contacts.icloud.com/",
                    help="where discovery starts (default: iCloud)")
    ap.add_argument("--book", help="an address book URL, skipping discovery")
    ap.add_argument("--book-index", type=int, default=1,
                    help="which of the address books found (1 = the first; "
                         "the report lists them by name)")
    ap.add_argument("--sample", type=int, default=20,
                    help="contacts compared, the biggest first (default 20)")
    ap.add_argument("--all", action="store_true",
                    help="fetch every contact both ways and total the bytes "
                         "(what a full resync of the device downloads)")
    ap.add_argument("--json", action="store_true", help="the report as JSON")
    a = ap.parse_args(argv)

    pw = os.environ.get("PROBE_PASS") or getpass.getpass("App-specific password: ")
    p = Probe(a.user, pw)
    rep = {"props_asked": PALM_PROPS}
    try:
        if a.book:
            book = a.book if a.book.endswith("/") else a.book + "/"
        else:
            books = discover(p, a.base)
            if not books:
                raise RuntimeError("no address book found under the account")
            rep["books_found"] = [n or "(no name)" for _, n in books]
            if not 1 <= a.book_index <= len(books):
                raise RuntimeError("--book-index %d: %d address book(s) found"
                                   % (a.book_index, len(books)))
            book = books[a.book_index - 1][0]
            rep["book_used"] = a.book_index
        mem = members(p, book)
        rep["contacts"] = len(mem)
        if not mem:
            raise RuntimeError("the address book is empty")
        lens = [m[2] for m in mem if m[2] is not None]
        if len(lens) == len(mem):
            rep["sizes_from_listing"] = {
                "total_bytes": sum(lens), "median_bytes": int(statistics.median(lens)),
                "max_bytes": max(lens),
                "over_8KB": sum(1 for x in lens if x > FETCH_CAP),
                "over_16KB": sum(1 for x in lens if x > PUT_MAX)}
            order = sorted(mem, key=lambda m: -m[2])
        else:
            order = mem
        pick = [m[0] for m in (order if a.all else order[:a.sample])]

        rows, full_wire, part_wire, full_s, part_s, part_status = [], 0, 0, 0.0, 0.0, 207
        for chunk in batches(pick, 50):
            full, st, n, s = multiget(p, book, chunk, False)
            if st != 207:
                raise RuntimeError("the full multiget: HTTP %d" % st)
            full_wire += n
            full_s += s
            part, pst, n, s = multiget(p, book, chunk, True)
            part_wire += n
            part_s += s
            if pst != 207:
                part_status = pst
                break
            for h in chunk:
                if h in full and h in part:
                    r = analyse(full[h][1], part[h][1])
                    r["same_etag"] = full[h][0] == part[h][0]
                    rows.append(r)
        v, why = verdict(rows, part_status)
        rep.update({
            "verdict": v, "why": why, "compared": len(rows),
            "full_reply_bytes": full_wire, "partial_reply_bytes": part_wire,
            "full_seconds": round(full_s, 2), "partial_seconds": round(part_s, 2),
            "with_photo": sum(1 for r in rows if r["full_photo"]),
            "palm_view_over_8KB": sum(1 for r in rows if r["palm_view_bytes"] >= FETCH_CAP),
            "full_contact_over_16KB": sum(1 for r in rows if r["full_bytes"] > PUT_MAX),
            "etag_differs": sum(1 for r in rows if not r["same_etag"]),
            "rows": rows})
    except (RuntimeError, urllib.error.URLError, OSError) as e:
        rep["error"] = str(e)
    if a.json:
        print(json.dumps(rep, indent=1))
    else:
        print_report(rep, a)
    return 2 if "error" in rep else 0


def print_report(r, a):
    print("probe_partial_card: %s" % ("every contact" if a.all else "a sample, the biggest first"))
    if "error" in r:
        print("ERROR: %s" % r["error"])
        return
    if "books_found" in r:
        print("address books found: %s; probed #%d (--book-index picks another)"
              % (", ".join("#%d %s" % (i, n) for i, n in enumerate(r["books_found"], 1)),
                 r["book_used"]))
    print("contacts in the address book: %d" % r["contacts"])
    s = r.get("sizes_from_listing")
    if s:
        print("  sizes (from the listing): total %s, median %s, largest %s"
              % (kb(s["total_bytes"]), kb(s["median_bytes"]), kb(s["max_bytes"])))
        print("  over 8 KB: %d   over 16 KB (too big for the device to send): %d"
              % (s["over_8KB"], s["over_16KB"]))
    else:
        print("  the server didn't give sizes in the listing; --all measures them")
    print()
    print("VERDICT: %s -- %s" % (r["verdict"], r["why"]))
    print("compared %d contacts (%d with a photo)" % (r["compared"], r["with_photo"]))
    print("  full replies    %s in %.1f s" % (kb(r["full_reply_bytes"]), r["full_seconds"]))
    print("  partial replies %s in %.1f s" % (kb(r["partial_reply_bytes"]), r["partial_seconds"]))
    if r["full_reply_bytes"]:
        print("  the partial reply is %.0f%% of the full one"
              % (100.0 * r["partial_reply_bytes"] / r["full_reply_bytes"]))
    print("  Palm view over 8 KB: %d   ETag differs between the two: %d"
          % (r["palm_view_over_8KB"], r["etag_differs"]))
    print()
    print("  #  full   partial photo(f/p) groups(f/p) palm-lines extra / missing")
    for i, x in enumerate(r["rows"], 1):
        same = "same" if x["palm_exact"] else ("same-ungrouped" if x["palm_nogroup"] else "DIFFER")
        print("%3d %6s %6s   %s/%s       %2d/%-2d     %-14s %s%s" % (
            i, kb(x["full_bytes"]), kb(x["part_bytes"]),
            "y" if x["full_photo"] else "n", "y" if x["part_photo"] else "n",
            x["full_grouped"], x["part_grouped"], same,
            ",".join(x["part_extra"]) or "-",
            (" / " + ",".join(x["missing"])) if x["missing"] else ""))
    print()
    print("Nothing was changed in the account: this probe sends PROPFIND and REPORT only.")


def kb(n):
    return "%d B" % n if n < 1024 else "%.1f KB" % (n / 1024.0) if n < 1048576 else "%.1f MB" % (n / 1048576.0)


if __name__ == "__main__":
    sys.exit(main())
