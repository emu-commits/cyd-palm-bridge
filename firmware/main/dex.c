/* dex.c -- see dex.h. */
#include "dex.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mark(const uint8_t *set, uint32_t id){ return id < MG_MAX_DEX && set[id / 8] >> (id % 8) & 1; }

int dx_open(Dex *d, const char *game, const char *save){
    memset(d, 0, sizeof *d);
    int r = mg_open(&d->g, game);
    if(r) return r;
    d->n = (uint16_t)d->g.n_mons;
    /* one block: mon[n], caught[n], state[n] */
    d->mon = malloc((size_t)d->n * 5 + 1);
    if(!d->mon){ snprintf(d->g.err, sizeof d->g.err, "out of memory"); mg_close(&d->g); return MG_EIO; }
    d->caught = d->mon + d->n;
    d->state = (uint8_t *)(d->caught + d->n);
    d->has_save = !mp_read_marks(save, &d->marks);
    mg_burst_begin(&d->g);
    for(uint32_t i = 0; i < d->n; i++) d->mon[i] = (uint16_t)i;     /* a file without DEXE: MONS' order */
    for(uint32_t i = 0; d->g.has_dex && i < d->n; i++){
        MgDex x;
        if((r = mg_dex(&d->g, i, &x))) goto fail;
        d->mon[x.number - 1] = (uint16_t)i;             /* checked on open: 1..n, each once */
    }
    for(uint32_t p = 0; p < d->n; p++){
        MgMon m;
        if((r = mg_mon(&d->g, d->mon[p], &m))) goto fail;
        d->state[p] = !d->has_save ? DX_NEVER : mark(d->marks.caught, m.id) ? DX_CAUGHT :
                      mark(d->marks.seen, m.id) ? DX_SEEN : DX_NEVER;
        if(d->state[p]) d->n_seen++;
        if(d->state[p] == DX_CAUGHT) d->caught[d->n_caught++] = (uint16_t)p;
    }
    for(uint32_t i = 0; i < d->g.n_areas; i++){
        MgAreaDir a;
        if((r = mg_area_dir(&d->g, i, &a))) goto fail;
        if(a.head_len > d->head_cap) d->head_cap = a.head_len;
    }
    mg_burst_end(&d->g);
    return 0;
fail:
    free(d->mon);
    d->mon = NULL;
    mg_close(&d->g);            /* the burst goes with it */
    return r;
}

void dx_close(Dex *d){
    free(d->mon);
    d->mon = NULL;
    mg_close(&d->g);
}

/* ---- the Where search ---- */

static int flag_set(const Dex *d, uint16_t f){
    return f && f < MG_FLAGS && d->marks.flags[f / 8] >> (f % 8) & 1;
}

/* the table a zone naming `table` uses in the save's act (as mp_table) */
static uint16_t table_now(const Dex *d, uint16_t table){
    const MonGame *g = &d->g;
    for(uint32_t i = 0; i < g->n_steps; i++)
        if(g->steps[i].table == table && flag_set(d, g->steps[i].flag)) table = g->steps[i].to;
    return table;
}

static int where_scan(Dex *d, uint16_t mon, DxWhere *w, uint8_t *head){
    MonGame *g = &d->g;
    memset(w, 0, sizeof *w);
    w->lo = 255;
    for(uint32_t i = 0; i < g->n_areas; i++){
        MgArea a;
        int r = mg_area_lists(g, i, &a, head, d->head_cap);
        if(r) return r;
        int here = 0;
        for(uint32_t z = 0; z < a.n_zones; z++){
            MgZone zn;
            uint16_t rows;
            mg_zone(&a, z, &zn);
            uint16_t t = table_now(d, zn.table);
            if((r = mg_enc_rows(g, t, &rows))) return r;
            for(uint32_t j = 0; j < rows; j++){
                MgEncRow row;
                if((r = mg_enc_row(g, t, j, &row))) return r;
                if(row.mon != mon || !row.rate) continue;
                here = 1;
                if(row.lo < w->lo) w->lo = row.lo;
                if(row.hi > w->hi) w->hi = row.hi;
            }
        }
        if(here){
            if(w->n_areas < DX_AREAS){
                if(mg_text(g, a.dir.name, w->area[w->n_areas], DX_NAME) < 0) w->area[w->n_areas][0] = 0;
                w->n_areas++;
            }
            else w->more_areas++;
        }
        for(uint32_t k = 0; k < a.n_people; k++){
            MgPerson p;
            mg_person(&a, k, &p);
            if(p.kind != MG_P_TRAINER) continue;
            if((p.show_if && !flag_set(d, p.show_if)) || (p.hide_if && flag_set(d, p.hide_if))) continue;
            int has = 0;
            for(int t = 0; t < MG_TEAM; t++) has |= p.mon[t] == mon;
            if(!has) continue;
            if(w->n_keepers < DX_KEEPERS){
                if(mg_text(g, p.name, w->keeper[w->n_keepers], DX_NAME) < 0) w->keeper[w->n_keepers][0] = 0;
                w->n_keepers++;
            }
            else w->more_keepers++;
        }
    }
    return 0;
}

int dx_where(Dex *d, uint16_t mon, DxWhere *w, uint8_t *head){
    mg_burst_begin(&d->g);
    int r = where_scan(d, mon, w, head);
    mg_burst_end(&d->g);
    return r;
}

/* ---- his words ---- */

void dx_number(unsigned n, char *out, size_t cap){
    static const char *const ONES[] = { "Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine",
        "Ten", "Eleven", "Twelve", "Thirteen", "Fourteen", "Fifteen", "Sixteen", "Seventeen", "Eighteen", "Nineteen" };
    static const char *const TENS[] = { "", "", "Twenty", "Thirty", "Forty", "Fifty", "Sixty", "Seventy", "Eighty", "Ninety" };
    if(n < 20) snprintf(out, cap, "%s", ONES[n]);
    else if(n < 100 && !(n % 10)) snprintf(out, cap, "%s", TENS[n / 10]);
    else if(n < 100){
        char lo[16];
        snprintf(lo, sizeof lo, "%s", ONES[n % 10]);
        lo[0] = (char)tolower((unsigned char)lo[0]);
        snprintf(out, cap, "%s-%s", TENS[n / 10], lo);
    }
    else snprintf(out, cap, "%u", n);
}

static void lower_first(char *s){ s[0] = (char)tolower((unsigned char)s[0]); }

/* a list of up to four names: "A", "A and B", "A, B and C"; more than that says so */
static void list_names(char *out, size_t cap, char (*names)[DX_NAME], int n, int more){
    size_t l = 0;
    int shown = n > 3 ? 3 : n, rest = n - shown + more;
    out[0] = 0;
    for(int i = 0; i < shown; i++){
        const char *sep = !i ? "" : i == shown - 1 && !rest ? " and " : ", ";
        int k = snprintf(out + l, l < cap ? cap - l : 0, "%s%s", sep, names[i]);
        if(k > 0) l += (size_t)k;
    }
    if(rest && l < cap) snprintf(out + l, cap - l, " and %d more", rest);
}

/* A word for a critter you've not seen, from its first type. Not its name:
 * he's giving a hint, and an unseen critter is ??? everywhere else. */
static const char *type_word(MonGame *g, const MgMon *m){
    static const struct { const char *type, *word; } W[] = {
        { "wood", "leafy" }, { "fire", "hot-tempered" }, { "water", "damp" }, { "earth", "heavy-footed" },
        { "metal", "clanking" }, { "venom", "sour-smelling" }, { "lightning", "crackling" }, { "aether", "odd" },
        { "normal", "quick" },
    };
    MgStr s;
    char t[24];
    if(m->type[0] != MG_NONE8 && !mg_type_name(g, m->type[0], &s) && mg_text(g, s, t, sizeof t) >= 0){
        for(char *c = t; *c; c++) *c = (char)tolower((unsigned char)*c);
        for(size_t i = 0; i < sizeof W / sizeof W[0]; i++) if(!strcmp(t, W[i].type)) return W[i].word;
    }
    return "strange";
}

/* the first critter in Dex order not yet seen that the act's tables can give, and
 * the first area it's in: one pass over the areas, not one for every critter (that
 * was 1,255 ms on the board with 23 unseen) */
static int hint(Dex *d, uint8_t *head, char *word, size_t wcap, char *area, size_t acap){
    MonGame *g = &d->g;
    int best = d->n, where = -1;
    for(uint32_t i = 0; i < g->n_areas; i++){
        MgArea a;
        if(mg_area_lists(g, i, &a, head, d->head_cap)) return 0;
        for(uint32_t z = 0; z < a.n_zones; z++){
            MgZone zn;
            uint16_t rows;
            mg_zone(&a, z, &zn);
            uint16_t t = table_now(d, zn.table);
            if(mg_enc_rows(g, t, &rows)) return 0;
            for(uint32_t j = 0; j < rows; j++){
                MgEncRow row;
                if(mg_enc_row(g, t, j, &row)) return 0;
                if(!row.rate) continue;
                for(int p = 0; p < best; p++)
                    if(d->mon[p] == row.mon){
                        if(!d->state[p]){ best = p; where = (int)i; }
                        break;
                    }
            }
        }
    }
    MgMon m;
    MgAreaDir ad;
    if(where < 0 || mg_mon(g, d->mon[best], &m) || mg_area_dir(g, (uint32_t)where, &ad)) return 0;
    snprintf(word, wcap, "%s", type_word(g, &m));
    if(mg_text(g, ad.name, area, (uint32_t)(acap < DX_NAME ? acap : DX_NAME)) < 0) area[0] = 0;
    return 1;
}

/* a line with one left to find: a caught critter whose evolution isn't caught,
 * and nothing past that. Its name, for "Your Vamporm's line has one left to find." */
static int line_short_one(Dex *d, char *name, size_t cap){
    for(int k = 0; k < d->n_caught; k++){
        MgMon m, next;
        uint16_t mon = d->mon[d->caught[k]];
        if(mg_mon(&d->g, mon, &m) || m.evolve_to == MG_NONE16 || mg_mon(&d->g, m.evolve_to, &next)) continue;
        if(mark(d->marks.caught, next.id) || next.evolve_to != MG_NONE16) continue;
        if(mg_text(&d->g, m.name, name, (uint32_t)cap) >= 0) return 1;
    }
    return 0;
}

void dx_greeting(Dex *d, char *out, size_t cap, uint8_t *head){
    char have[24], all[24], word[32], area[DX_NAME], name[DX_NAME];
    if(!d->has_save){ snprintf(out, cap, "No keeper's licence, no Dex. Start a game in Critters first."); return; }
    if(!d->n_caught){ snprintf(out, cap, "Not one yet. Dr Wren's lab is where it starts."); return; }
    dx_number(d->n_caught, have, sizeof have);
    dx_number(d->n, all, sizeof all);
    lower_first(all);
    if(d->n_caught >= d->n){ snprintf(out, cap, "%s of %s. I've nothing left to teach you.", have, all); return; }
    if(d->n_seen >= d->n){ snprintf(out, cap, "You've met them all. Catching them is another thing."); return; }
    mg_burst_begin(&d->g);
    int short_one = line_short_one(d, name, sizeof name), has_hint = head && hint(d, head, word, sizeof word, area, sizeof area);
    mg_burst_end(&d->g);
    if(short_one && (!has_hint || d->n_caught % 2)){ snprintf(out, cap, "Your %s's line has one left to find.", name); return; }
    if(has_hint){
        const char *tail = d->n_caught * 4 < d->n ? "Have a look." : d->n_caught * 2 < d->n ? "Worth the walk." : "Not many left now.";
        snprintf(out, cap, "%s of %s. Something %s keeps turning up around %s. %s", have, all, word, area, tail);
        return;
    }
    snprintf(out, cap, "%s of %s. Nothing new near you for now. Beat the next keeper and see what moves in.", have, all);
}

void dx_talk(Dex *d, int pos, char *out, size_t cap, uint8_t *head){
    if(pos < 0 || pos >= d->n || d->state[pos] != DX_CAUGHT){ snprintf(out, cap, "Nothing on this one yet."); return; }
    MgMon m;
    DxWhere w;
    char t[DX_NAME * 5], name[DX_NAME];
    if(mg_mon(&d->g, d->mon[pos], &m) || mg_text(&d->g, m.name, name, sizeof name) < 0 ||
       !head || dx_where(d, d->mon[pos], &w, head)){
        snprintf(out, cap, "My notes on that one are smudged. Ask me again.");
        return;
    }
    size_t l = 0;
    out[0] = 0;
    int yours = d->marks.starter && m.id == d->marks.starter;
    if(yours) l = (size_t)snprintf(out, cap, "Yours came from Dr Wren's lab. ");
    if(l >= cap) return;
    if(w.n_areas){
        list_names(t, sizeof t, w.area, w.n_areas, w.more_areas);
        if(w.lo == w.hi) snprintf(out + l, cap - l, "%s%s, at level %u.", yours ? "In the wild: " : "", t, w.lo);
        else snprintf(out + l, cap - l, "%s%s, levels %u to %u.", yours ? "In the wild: " : "", t, w.lo, w.hi);
    }
    else if(w.n_keepers){
        list_names(t, sizeof t, w.keeper, w.n_keepers, w.more_keepers);
        int many = w.n_keepers + w.more_keepers > 1;
        snprintf(out + l, cap - l, "No one's seen one wild. %s %s one; ask %s.", t, many ? "each keep" : "keeps", "them");
    }
    else if(yours) snprintf(out + l, cap - l, "No one's seen one wild.");
    else snprintf(out + l, cap - l, "Not wild, not just now. Whatever you did to get yours, keep it to yourself.");
}
