/* srs.c -- Study's schedulers and progress files. See srs.h. */
#include "srs.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "safefile.h"

static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p){
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put16(uint8_t *p, uint16_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v){
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void srs_pack(const SrsRec *r, uint8_t out[SRS_REC_SIZE]){
    put32(out, r->id);
    put32(out + 4, r->due);
    put16(out + 8, r->ivl_h);
    out[10] = r->ease;
    out[11] = r->stage;
    out[12] = r->flags;
    out[13] = r->lapses;
    put16(out + 14, r->reviews);
}

void srs_unpack(const uint8_t in[SRS_REC_SIZE], SrsRec *r){
    r->id = le32(in);
    r->due = le32(in + 4);
    r->ivl_h = le16(in + 8);
    r->ease = in[10];
    r->stage = in[11];
    r->flags = in[12];
    r->lapses = in[13];
    r->reviews = le16(in + 14);
}

/* ---- the schedulers ---------------------------------------------------- */

static uint16_t hours_of(uint64_t s){
    uint64_t h = (s + 1800) / 3600;
    return h > 65535 ? 65535 : (uint16_t)h;
}

static uint32_t add_time(uint32_t now, uint64_t s){
    uint64_t t = (uint64_t)now + s;
    return t > 0xFFFFFFFEu ? 0xFFFFFFFEu : (uint32_t)t;
}

/* the start of the local day `now` falls in, as UTC */
static uint32_t start_of_day(uint32_t now, int32_t tz){
    int64_t local = (int64_t)now + tz;
    int64_t sod = local - (local % 86400 + 86400) % 86400;
    sod -= tz;
    return sod < 0 ? 0 : (uint32_t)sod;
}

void srs_start(const Course *c, SrsRec *r, uint32_t id, uint32_t now, int32_t tz){
    (void)tz;
    memset(r, 0, sizeof *r);
    r->id = id;
    r->flags = SRS_F_STARTED;
    if(c->scheduler == CS_STAGES){
        r->stage = 1;
        r->due = add_time(now, c->steps[0]);
        r->ivl_h = hours_of(c->steps[0]);
    } else {
        /* sm2: a new item is due at once, as in the web app; its first
         * review puts it on the ladder */
        r->stage = 0;
        r->ease = SRS_EASE_BYTE(c->ease[0]);
        r->due = now;
    }
}

static void grade_stages(const Course *c, SrsRec *r, int grade, uint32_t now){
    uint8_t n = c->n_steps, st = r->stage;
    if(r->flags & SRS_F_RETIRED) return;
    if(st < 1) st = 1;
    if(st > n - 1) st = n - 1;
    if(grade != SRS_AGAIN){
        st++;
        if(st >= n){                                /* the last stage: retired */
            r->stage = n;
            r->flags |= SRS_F_RETIRED;
            r->due = 0;
            r->ivl_h = 0;
            return;
        }
    } else {
        uint8_t d = st >= c->high_from ? c->drop_high : c->drop;
        st = st > d ? (uint8_t)(st - d) : 1;
        if(r->lapses < 255) r->lapses++;
    }
    r->stage = st;
    r->due = add_time(now, c->steps[st - 1]);
    r->ivl_h = hours_of(c->steps[st - 1]);
}

static void grade_sm2(const Course *c, SrsRec *r, int grade, uint32_t now, int32_t tz){
    int32_t ease = SRS_EASE(r->ease);
    if(grade == SRS_AGAIN){
        if(r->lapses < 255) r->lapses++;
        ease += c->ease_change[0];
        if(ease < c->ease[1]) ease = c->ease[1];
        r->ease = SRS_EASE_BYTE(ease);
        r->stage = 0;                              /* back to the first rung */
        r->ivl_h = 0;
        r->due = add_time(now, c->relearn);
        return;
    }
    uint8_t st = r->stage < c->n_steps ? (uint8_t)(r->stage + 1) : c->n_steps;
    uint64_t step = c->steps[st - 1];
    ease += c->ease_change[grade];
    if(ease < c->ease[1]) ease = c->ease[1];
    if(ease > SRS_EASE_MAX) ease = SRS_EASE_MAX;
    /* the interval grows from the larger of the rung's minimum and the last
     * interval times the ease, then the grade scales it: all in seconds and
     * hundredths, so nothing is left to floating point */
    uint64_t ivl = (uint64_t)r->ivl_h * 3600;
    uint64_t base = ivl ? ivl * (uint64_t)ease / 100 : step;
    if(base < step) base = step;
    uint64_t scaled = base * c->mult[grade - 1] / 100;
    r->ease = SRS_EASE_BYTE(ease);
    r->stage = st;
    if(c->day_aligned){
        uint64_t days = (scaled + 43200) / 86400;
        if(days < 1) days = 1;
        if(days > 2730) days = 2730;               /* 65,535 hours */
        r->ivl_h = (uint16_t)(days * 24);
        /* the web app's rule: due at the start of the day after the
         * interval ends, so never sooner than tomorrow */
        r->due = add_time(start_of_day(now, tz), (days + 1) * 86400);
    } else {
        uint16_t h = hours_of(scaled);
        if(!h) h = 1;
        r->ivl_h = h;
        r->due = add_time(now, (uint64_t)h * 3600);
    }
}

void srs_grade(const Course *c, SrsRec *r, int grade, uint32_t now, int32_t tz){
    if(grade < SRS_AGAIN || grade > SRS_EASY) grade = SRS_AGAIN;
    if(c->scheduler == CS_STAGES) grade_stages(c, r, grade, now);
    else grade_sm2(c, r, grade, now, tz);
    if(r->reviews < 65535) r->reviews++;
}

int srs_known(const Course *c, const SrsRec *r){
    if(!(r->flags & SRS_F_STARTED)) return 0;
    if(r->flags & SRS_F_RETIRED) return 1;
    return r->stage >= c->known;
}

int srs_group(const Course *c, const SrsRec *r){
    uint32_t v = c->scheduler == CS_STAGES ? r->stage : r->ivl_h;
    int g = 0;
    for(int i = 0; i < c->n_groups; i++) if(c->groups[i].from <= v) g = i;
    return g;
}

/* ---- the files ----------------------------------------------------------- */

static int err(Srs *s, int code, const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->err, sizeof s->err, fmt, ap);
    va_end(ap);
    return code;
}

static void set_state(Srs *s, uint32_t n, int v){
    uint8_t *b = &s->state[n >> 2];
    int sh = (n & 3) * 2;
    *b = (uint8_t)((*b & ~(3 << sh)) | (v << sh));
}
static int get_state(const Srs *s, uint32_t n){ return (s->state[n >> 2] >> ((n & 3) * 2)) & 3; }

/* progress.dat: "SRSP", u8 version 1, u8 0, u16 record size, u32 count,
 * u32 course fingerprint, u32 CRC-32 of the records; then the records. */
typedef struct { uint32_t count, fingerprint, crc; } DatHead;

static int read_head(FILE *f, long size, DatHead *h){
    uint8_t b[SRS_DAT_HEADER];
    if(size < SRS_DAT_HEADER || fseek(f, 0, SEEK_SET) || fread(b, 1, SRS_DAT_HEADER, f) != SRS_DAT_HEADER) return -1;
    if(memcmp(b, "SRSP", 4) || b[4] != 1 || le16(b + 6) != SRS_REC_SIZE) return -1;
    h->count = le32(b + 8);
    h->fingerprint = le32(b + 12);
    h->crc = le32(b + 16);
    if((uint64_t)SRS_DAT_HEADER + (uint64_t)h->count * SRS_REC_SIZE != (uint64_t)size) return -1;
    return 0;
}

static void make_head(uint8_t b[SRS_DAT_HEADER], const DatHead *h){
    memcpy(b, "SRSP", 4);
    b[4] = 1; b[5] = 0;
    put16(b + 6, SRS_REC_SIZE);
    put32(b + 8, h->count);
    put32(b + 12, h->fingerprint);
    put32(b + 16, h->crc);
}

static long file_size(FILE *f){
    if(fseek(f, 0, SEEK_END)) return -1;
    return ftell(f);
}

/* read log entry i of f into r: 1 = a whole entry whose CRC matches */
static int log_entry(FILE *f, long i, SrsRec *r){
    uint8_t e[SRS_LOG_ENTRY];
    if(fseek(f, i * SRS_LOG_ENTRY, SEEK_SET) || fread(e, 1, SRS_LOG_ENTRY, f) != SRS_LOG_ENTRY) return 0;
    if(course_crc32(0, e, SRS_REC_SIZE) != le32(e + SRS_REC_SIZE)) return 0;
    srs_unpack(e, r);
    return (r->flags & SRS_F_STARTED) && r->id;
}

static int by_id(const void *a, const void *b){
    uint32_t x = ((const SrsRec *)a)->id, y = ((const SrsRec *)b)->id;
    return x < y ? -1 : x > y;
}

/* keep a record in step with a course whose scheduler may have changed */
static void clamp(const Course *c, SrsRec *r){
    if(c->scheduler == CS_STAGES){
        if(r->stage < 1) r->stage = 1;
        if(r->stage >= c->n_steps){ r->stage = c->n_steps; r->flags |= SRS_F_RETIRED; r->due = 0; }
        else r->flags &= (uint8_t)~SRS_F_RETIRED;
    } else {
        if(r->stage > c->n_steps) r->stage = c->n_steps;
        r->flags &= (uint8_t)~SRS_F_RETIRED;
        if(SRS_EASE(r->ease) < c->ease[1]) r->ease = SRS_EASE_BYTE(c->ease[1]);
    }
}

/* One pass: progress.dat merged with `chunk` (sorted by id, one record per
 * id), written whole through safefile. With `remap`, old records whose id is
 * no longer in the course are dropped (a walk of IDX beside the file). */
static int merge_pass(Srs *s, SrsRec *chunk, int nchunk, int remap){
    FILE *in = fopen(s->dat, "rb");
    DatHead h = { 0, s->c->fingerprint, 0 }, oh = { 0, 0, 0 };
    if(in){
        long sz = file_size(in);
        if(read_head(in, sz, &oh)){ fclose(in); return err(s, SRS_EBAD, "progress.dat is damaged"); }
        if(fseek(in, SRS_DAT_HEADER, SEEK_SET)){ fclose(in); return err(s, SRS_EIO, "can't read progress.dat"); }
    }
    SafeFile sf;
    if(!sf_open(&sf, s->dat, "wb")){ if(in) fclose(in); return err(s, SRS_EIO, "can't write progress"); }
    uint8_t hb[SRS_DAT_HEADER], rb[SRS_REC_SIZE];
    make_head(hb, &h);
    int ok = fwrite(hb, 1, SRS_DAT_HEADER, sf.f) == SRS_DAT_HEADER;
    uint32_t old_crc = 0, crc = 0, prev = 0, j = 0, idx_id = 0, idx_n = 0;
    int ci = 0, have_idx = 0;
    SrsRec r;
    for(uint32_t k = 0; ok; ){
        int have_old = k < oh.count;
        if(have_old){
            if(fread(rb, 1, SRS_REC_SIZE, in) != SRS_REC_SIZE){ ok = 0; break; }
            old_crc = course_crc32(old_crc, rb, SRS_REC_SIZE);
            srs_unpack(rb, &r);
            if(r.id <= prev){ sf_abort(&sf); fclose(in); return err(s, SRS_EBAD, "progress.dat is out of order"); }
            prev = r.id;
            k++;
        }
        /* chunk records that come before this old one (or all that remain) */
        while(ci < nchunk && (!have_old || chunk[ci].id < r.id)){
            srs_pack(&chunk[ci++], rb);
            crc = course_crc32(crc, rb, SRS_REC_SIZE);
            ok &= fwrite(rb, 1, SRS_REC_SIZE, sf.f) == SRS_REC_SIZE;
            h.count++;
        }
        if(!have_old) break;
        if(ci < nchunk && chunk[ci].id == r.id) r = chunk[ci++];     /* the log's is newer */
        else if(remap){
            while(have_idx >= 0 && (!have_idx || idx_id < r.id)){
                if(j >= s->c->n_items){ have_idx = -1; break; }
                if(course_idx(s->c, j++, &idx_id, &idx_n)){ sf_abort(&sf); if(in) fclose(in); return err(s, SRS_EIO, "%s", s->c->err); }
                have_idx = 1;
            }
            if(have_idx < 0 || idx_id != r.id){ s->dropped++; continue; }   /* gone from the course */
            clamp(s->c, &r);
            s->remapped++;
        }
        srs_pack(&r, rb);
        crc = course_crc32(crc, rb, SRS_REC_SIZE);
        ok &= fwrite(rb, 1, SRS_REC_SIZE, sf.f) == SRS_REC_SIZE;
        h.count++;
    }
    if(in){
        fclose(in);
        if(ok && old_crc != oh.crc){ sf_abort(&sf); return err(s, SRS_EBAD, "progress.dat is damaged"); }
    }
    h.crc = crc;
    make_head(hb, &h);
    if(ok) ok = fseek(sf.f, 0, SEEK_SET) == 0 && fwrite(hb, 1, SRS_DAT_HEADER, sf.f) == SRS_DAT_HEADER;
    if(sf_commit(&sf, ok)) return err(s, SRS_EIO, "can't write progress");
    return 0;
}

/* Fold the log into progress.dat (and remap if the course changed), then
 * empty the log. Chunked, so a long log never needs more than one chunk in
 * RAM; each chunk is its own pass. */
static int fold(Srs *s){
    int remap = 0, have_dat = 0, r;
    FILE *d = fopen(s->dat, "rb");
    if(d){
        have_dat = 1;
        DatHead h;
        long sz = file_size(d);
        r = read_head(d, sz, &h);
        fclose(d);
        if(r) return err(s, SRS_EBAD, "progress.dat is damaged");
        remap = h.fingerprint != s->c->fingerprint;
    }
    FILE *lf = fopen(s->log, "rb");
    long entries = 0, bytes = 0;
    if(lf){
        bytes = file_size(lf);
        entries = bytes > 0 ? bytes / SRS_LOG_ENTRY : 0;
    }
    /* any bytes at all mean a fold, even less than one entry: a torn tail
     * left in place would put every later entry out of step */
    if(bytes <= 0 && !remap && have_dat){ if(lf) fclose(lf); return 0; }

    SrsRec *chunk = malloc(sizeof(SrsRec) * SRS_FOLD_CHUNK);
    if(!chunk){ if(lf) fclose(lf); return err(s, SRS_ENOMEM, "out of memory"); }
    long i = 0;
    int first = 1;
    for(;;){
        int n = 0;
        while(i < entries && n < SRS_FOLD_CHUNK){
            SrsRec e;
            if(!log_entry(lf, i, &e)){ i = entries; break; }    /* a torn tail: the rest is gone */
            i++;
            uint32_t item;
            if(course_find(s->c, e.id, &item)){ s->dropped++; continue; }   /* not in this course */
            clamp(s->c, &e);
            int k;
            for(k = 0; k < n && chunk[k].id != e.id; k++) {}
            chunk[k] = e;                                        /* the later one wins */
            if(k == n) n++;
        }
        if(!n && !first) break;
        qsort(chunk, n, sizeof *chunk, by_id);
        if((r = merge_pass(s, chunk, n, first && remap))){ free(chunk); if(lf) fclose(lf); return r; }
        first = 0;
        if(i >= entries) break;
    }
    free(chunk);
    if(lf) fclose(lf);
    /* only now is it safe to empty the log: progress.dat has everything */
    FILE *t = fopen(s->log, "wb");
    if(!t) return err(s, SRS_EIO, "can't reset the progress log");
    fclose(t);
    return 0;
}

static int scan_dat(Srs *s, uint32_t now){
    Course *c = s->c;
    memset(s->state, 0, (c->n_items + 3) / 4);
    s->n_due = 0; s->due_total = 0; s->next_due = 0; s->started = 0;
    memset(s->group_count, 0, sizeof s->group_count);
    FILE *f = fopen(s->dat, "rb");
    if(!f) return err(s, SRS_EIO, "can't read progress");
    DatHead h;
    if(read_head(f, file_size(f), &h) || fseek(f, SRS_DAT_HEADER, SEEK_SET)){ fclose(f); return err(s, SRS_EBAD, "progress.dat is damaged"); }
    uint32_t crc = 0, j = 0, idx_id = 0, idx_n = 0, prev = 0;
    int have = 0;
    uint8_t rb[SRS_REC_SIZE];
    for(uint32_t k = 0; k < h.count; k++){
        SrsRec r;
        if(fread(rb, 1, SRS_REC_SIZE, f) != SRS_REC_SIZE){ fclose(f); return err(s, SRS_EIO, "can't read progress"); }
        crc = course_crc32(crc, rb, SRS_REC_SIZE);
        srs_unpack(rb, &r);
        if(r.id <= prev){ fclose(f); return err(s, SRS_EBAD, "progress.dat is out of order"); }
        prev = r.id;
        /* IDX and progress.dat are both sorted by id: one walk matches them */
        while(have >= 0 && (!have || idx_id < r.id)){
            if(j >= c->n_items){ have = -1; break; }
            if(course_idx(c, j++, &idx_id, &idx_n)){ fclose(f); return err(s, SRS_EIO, "%s", c->err); }
            have = 1;
        }
        if(have < 0 || idx_id != r.id) continue;
        s->started++;
        set_state(s, idx_n, srs_known(c, &r) ? SRS_KNOWN : SRS_LEARNING);
        s->group_count[srs_group(c, &r)]++;
        if((r.flags & (SRS_F_RETIRED | SRS_F_SUSPENDED)) || !r.due) continue;
        if(r.due <= now){
            s->due_total++;
            if(s->n_due < SRS_DUE_MAX) s->due[s->n_due++] = (uint16_t)idx_n;
        } else if(!s->next_due || r.due < s->next_due) s->next_due = r.due;
    }
    fclose(f);
    if(crc != h.crc) return err(s, SRS_EBAD, "progress.dat is damaged");
    return 0;
}

/* Recount: the session's log is folded in first, so progress.dat is the
 * whole truth, then scanned. */
int srs_scan(Srs *s, uint32_t now){
    if(s->logf){ fclose(s->logf); s->logf = NULL; }
    int r = fold(s);
    if(!r) r = scan_dat(s, now);
    if(!s->logf && !(s->logf = fopen(s->log, "a+b")) && !r) r = err(s, SRS_EIO, "can't open the progress log");
    return r;
}

void srs_close(Srs *s){
    if(s->logf){ fclose(s->logf); s->logf = NULL; }
    free(s->state); s->state = NULL;
    free(s->due); s->due = NULL;
}

int srs_open(Srs *s, Course *c, const char *dir, uint32_t now, int32_t tz){
    (void)tz;
    memset(s, 0, sizeof *s);
    s->c = c;
    snprintf(s->dat, sizeof s->dat, "%s/progress.dat", dir);
    snprintf(s->log, sizeof s->log, "%s/progress.log", dir);
    s->state = calloc(1, (c->n_items + 3) / 4);
    s->due = malloc(sizeof(uint16_t) * SRS_DUE_MAX);
    if(!s->state || !s->due){ srs_close(s); return err(s, SRS_ENOMEM, "out of memory"); }
    sf_recover(s->dat);
    int r = srs_scan(s, now);
    if(r){ char e[80]; memcpy(e, s->err, sizeof e); srs_close(s); memcpy(s->err, e, sizeof e); }
    return r;
}

/* ---- one record ------------------------------------------------------- */

int srs_get(Srs *s, uint32_t n, SrsRec *out){
    CourseItem it;
    if(course_item(s->c, n, &it)) return err(s, SRS_EIO, "%s", s->c->err);
    /* newest first: this session's log, from the end */
    if(s->logf){
        long sz = file_size(s->logf);
        for(long i = sz > 0 ? sz / SRS_LOG_ENTRY - 1 : -1; i >= 0; i--){
            SrsRec r;
            if(log_entry(s->logf, i, &r) && r.id == it.id){ *out = r; return 1; }
        }
    }
    /* then progress.dat, by binary search */
    FILE *f = fopen(s->dat, "rb");
    if(!f) return 0;
    DatHead h;
    if(read_head(f, file_size(f), &h)){ fclose(f); return err(s, SRS_EBAD, "progress.dat is damaged"); }
    uint32_t lo = 0, hi = h.count;
    uint8_t rb[SRS_REC_SIZE];
    while(lo < hi){
        uint32_t mid = lo + (hi - lo) / 2;
        if(fseek(f, SRS_DAT_HEADER + (long)mid * SRS_REC_SIZE, SEEK_SET) || fread(rb, 1, SRS_REC_SIZE, f) != SRS_REC_SIZE){
            fclose(f); return err(s, SRS_EIO, "can't read progress");
        }
        uint32_t id = le32(rb);
        if(id == it.id){ srs_unpack(rb, out); fclose(f); return 1; }
        if(id < it.id) lo = mid + 1; else hi = mid;
    }
    fclose(f);
    return 0;
}

static void due_remove(Srs *s, uint32_t n){
    for(uint16_t i = 0; i < s->n_due; i++) if(s->due[i] == n){
        s->due[i] = s->due[--s->n_due];
        return;
    }
}

int srs_put(Srs *s, uint32_t n, const SrsRec *r, uint32_t now){
    CourseItem it;
    if(!s->logf) return err(s, SRS_EIO, "progress isn't open");
    if(course_item(s->c, n, &it)) return err(s, SRS_EIO, "%s", s->c->err);
    if(it.id != r->id || !(r->flags & SRS_F_STARTED)) return err(s, SRS_EBAD, "a record for the wrong item");
    SrsRec old;
    int had = srs_get(s, n, &old);
    if(had < 0) return had;
    uint8_t e[SRS_LOG_ENTRY];
    srs_pack(r, e);
    put32(e + SRS_REC_SIZE, course_crc32(0, e, SRS_REC_SIZE));
    if(fseek(s->logf, 0, SEEK_END) || fwrite(e, 1, SRS_LOG_ENTRY, s->logf) != SRS_LOG_ENTRY
       || fflush(s->logf) || fsync(fileno(s->logf)))
        return err(s, SRS_EIO, "can't save progress");
    /* keep the counts in step without a rescan */
    if(had == 1){
        s->group_count[srs_group(s->c, &old)]--;
        if(!(old.flags & (SRS_F_RETIRED | SRS_F_SUSPENDED)) && old.due && old.due <= now && s->due_total) s->due_total--;
    } else s->started++;
    s->group_count[srs_group(s->c, r)]++;
    set_state(s, n, srs_known(s->c, r) ? SRS_KNOWN : SRS_LEARNING);
    due_remove(s, n);
    if(!(r->flags & (SRS_F_RETIRED | SRS_F_SUSPENDED)) && r->due){
        if(r->due <= now){
            s->due_total++;
            if(s->n_due < SRS_DUE_MAX) s->due[s->n_due++] = (uint16_t)n;
        } else if(!s->next_due || r->due < s->next_due) s->next_due = r->due;
    }
    return 0;
}

/* ---- lessons and unlocking ---------------------------------------------- */

int srs_lessons(Srs *s, uint16_t *out, int max, uint16_t *level){
    Course *c = s->c;
    uint8_t *buf = NULL;
    int count = 0, open = 1;
    if(level) *level = 0;
    for(uint32_t li = 0; li < c->n_levels && open; li++){
        CourseLevel lv;
        if(course_level(c, li, &lv)){ free(buf); return err(s, SRS_EIO, "%s", c->err); }
        if(level) *level = lv.number;
        uint32_t lu_total = 0, lu_known = 0, all_known = 0;
        for(uint32_t n = lv.first; n < lv.first + lv.count; n++){
            CourseItem it;
            if(course_item(c, n, &it)){ free(buf); return err(s, SRS_EIO, "%s", c->err); }
            int st = get_state(s, n);
            if(st == SRS_KNOWN) all_known++;
            if(c->kinds[it.kind].flags & 1){ lu_total++; if(st == SRS_KNOWN) lu_known++; }
            if(st != SRS_NONE) continue;
            /* a lesson waits for the items it's built from (with by_links)
             * and those it must unlock after */
            int ok = 1;
            if(it.n_links){
                if(!buf && !(buf = malloc(COURSE_ITEM_BUF))) return err(s, SRS_ENOMEM, "out of memory");
                if(course_item_load(c, &it, buf, COURSE_ITEM_BUF) < 0){ free(buf); return err(s, SRS_EIO, "%s", c->err); }
                for(uint16_t k = 0; k < it.n_links && ok; k++){
                    CourseLink l;
                    course_link(buf, &it, k, &l);
                    if((l.type == CL_UNLOCK_AFTER || (l.type == CL_BUILT_FROM && c->by_links))
                       && get_state(s, l.target) != SRS_KNOWN)
                        ok = 0;
                }
            }
            if(!ok) continue;
            if(out && count < max) out[count] = (uint16_t)n;
            count++;
        }
        /* the next level opens when this one's level-up items (or all of
         * them, if it has none) are known to level_percent, rounded up */
        if(c->level_pct){
            uint32_t total = lu_total ? lu_total : lv.count, known = lu_total ? lu_known : all_known;
            open = known * 100 >= (uint32_t)c->level_pct * total;
        }
    }
    free(buf);
    return count;
}
