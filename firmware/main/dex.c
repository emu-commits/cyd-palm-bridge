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

/* What he thinks you should do next: a hint, a line to finish, or how it stands. */
void dx_progress(Dex *d, char *out, size_t cap, uint8_t *head){
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

/* ---- who he is ----
 * MY DRAFTS, for the owner to change. Each piece opens at its share of the Dex
 * (STORY[i] at i / DX_STORY of it), and he tells it the moment it opens; the
 * Dex keeps no memory of what it's said, so the count is the only clock. */
static const char *const STORY[] = {
    "This gadget? I built it. Well. I complained, and Dr Wren did the wiring.",
    "You catch them faster than I did. I was fourteen, with a borrowed camera and a critter that bit me.",
    "That critter was an Anoleaf called Biscuit. Still bites. Mostly the postman now.",
    "The camera was my sister's. She went up the coast, and I said I'd send her pictures of everything.",
    "I took on the Kestrel Ring three times. Ivy's mother beat me three times. The third, she laughed.",
    "So I stopped battling and started writing things down. I was much better at that.",
    "I've walked this region for forty years. My boots are on their ninth set of soles.",
    "See the limp? People assume a dragon. It was a wet log in the Brideswood. Tell them it was a dragon.",
    "I was young once, and thin. Thin enough to fit through a crack in the old gate behind Ashe's post.",
    "I saw the Reach, through that crack. Every critter there is, and no fence. I stayed nine days.",
    "I told Wren what I saw. She's been at that machine of hers ever since. Thirty years, she's been at it.",
    "My sister wrote back, you know. Took her eleven years. She said: send more of the small ones.",
    "I never did fill this thing. Forty years, and I always stopped a few short. Knees, mostly.",
    "I give one of these to a new keeper every so often. They go in a drawer, mostly. Not yours.",
};
#define DX_STORY (sizeof STORY / sizeof STORY[0])

/* ---- where he met them: only of ones you've caught ---- */
static const struct { const char *name, *line; } MEMORY[] = {
    { "Agnite",       "My first Agnite was in Route 1's grass. It set fire to my lunch, then looked very sorry." },
    { "Anoleaf",      "Anoleaf sit so still on Route 1 you can tread on one. I know. Biscuit has never forgiven me." },
    { "Cardiling",    "I found a Cardiling on Route 1 singing at a fence post. It was still there the next morning." },
    { "Vamporm",      "Vamporm hide under Route 1's flowers. A picnic will bring them out. Ask Ben about his sandwiches." },
    { "Noctula",      "Noctula hang in the Brideswood by day. I once slept under forty of them. Never again in that hat." },
    { "Zunna",        "First Zunna I met walked beside me down the Brideswood for an hour, then left without a word." },
    { "Eyenemy",      "An Eyenemy followed me all through the Brideswood. It never blinked. I still check behind me." },
    { "Selket",       "I put my hand on a Selket in the Brideswood. I took it off again very quickly." },
    { "Pigabyte",     "Pigabyte will eat anything in the Brideswood. I lost two notebooks and a good left boot." },
    { "Gillpip",      "I found Gillpip in a ditch in the Brideswood, after rain. No water for a mile. Don't ask me." },
    { "Sprigling",    "Sprigling look like weeds. I pulled one out once, up in the Brideswood. It pulled back." },
    { "Sparkip",      "Your hair stands up before you see a Sparkip. On Route 1 I took it for a storm. It was smaller." },
    { "Cirrup",       "A Cirrup drifted over Route 1 and rained on my notes. Only my notes. I think it meant it." },
    { "Nut",          "Down the Cotton Tunnel the Nut roll to the lowest spot. Follow the slope and you'll find them." },
    { "Cateye",       "Two lights in the Cotton Tunnel: that's a Cateye. Or a miner's lamps. Say hello and see." },
    { "Dune Pincher", "I met a Dune Pincher in the Cotton Tunnel wearing somebody's scarf. I never found the somebody." },
    { "Djinnbo",      "A Djinnbo in the tunnel offered me something. No idea what. I said no, and I'm still here." },
    { "Cinderkit",    "A Cinderkit kept me warm one night in the Cotton Tunnel, then ran off with my matches." },
    { "Skullick",     "Skullick lick the tunnel walls for the salt. Don't lick them back. I was very young." },
    { "Rivetron",     "I heard the Rivetron before I saw it. Clank, clank, in the dark of the tunnel. I walked faster." },
    { "Propellercat", "On Route 2 a Propellercat landed on my pack and rode there till dusk. I never had the heart." },
    { "Wildbough",    "That tree on Route 2 wasn't there when I walked out. It was there when I walked back. Wildbough." },
    { "Maelstrotl",   "A Maelstrotl lives in the Old Keep's moat. Or the moat lives in it. I didn't stay to ask." },
    { "Fulminax",     "A Fulminax on the Old Keep's wall lit up the whole yard. Best picture I ever ruined." },
    { "Grovarch",     "I sat against a Grovarch at the Old Keep for an hour before it sighed. We both pretended it hadn't." },
    { "Scorchvix",    "A Scorchvix looked me over in Dragon's Cave, decided I wasn't worth it, and I've never been so glad." },
    { "Pyrewing",     "Pyrewing nest up in Dragon's Cave. Nobody's seen it twice from the same spot. I've tried." },
    { "Banewyrm",     "You smell a Banewyrm first, in Dragon's Cave. Then you stop smelling much at all, for a week." },
    { "Tempyre",      "I saw a Tempyre once, in Dragon's Cave. Weather indoors. I've no better way to say it." },
    { "Agnigon",      "Ashe calls them false dragons. Not to an Agnigon's face, she doesn't. Nobody in that cave does." },
};
#define DX_MEMORIES (sizeof MEMORY / sizeof MEMORY[0])

unsigned dx_memory_count(void){ return DX_MEMORIES; }
const char *dx_memory_name(unsigned i){ return i < DX_MEMORIES ? MEMORY[i].name : NULL; }

const char *dx_memory(const char *name){
    for(size_t i = 0; i < DX_MEMORIES; i++) if(!strcmp(MEMORY[i].name, name)) return MEMORY[i].line;
    return NULL;
}

/* the k-th caught critter he has a memory of, counting round; NULL if none */
static const char *memory_of_caught(Dex *d, unsigned k){
    const char *found[DX_MEMORIES];
    unsigned n = 0;
    char name[DX_NAME];
    mg_burst_begin(&d->g);
    for(int i = 0; i < d->n_caught && n < DX_MEMORIES; i++){
        MgMon m;
        if(mg_mon(&d->g, d->mon[d->caught[i]], &m) || mg_text(&d->g, m.name, name, sizeof name) < 0) continue;
        const char *l = dx_memory(name);
        if(l) found[n++] = l;
    }
    mg_burst_end(&d->g);
    return n ? found[k % n] : NULL;
}

/* Nearly there: %s is how many are left, as a word. */
static const char *const NEARLY[] = {
    "%s left. I've started clearing a space on the shelf. Don't let that hurry you.",
    "%s to go. I've never got this close with my own knees. Keep walking.",
    "%s. The last ones hide because everybody else already looked in the easy places.",
    "%s more. Biscuit's stopped biting the post to come and watch this. That's how you know.",
    "%s left, and you'll miss this part when it's over. I did, and I never even finished.",
};
#define DX_NEARLY (sizeof NEARLY / sizeof NEARLY[0])

/* Full: looking back. */
static const char *const FULL[] = {
    "I wrote to my sister. Told her someone finally finished it. She'll answer in about eleven years.",
    "Remember your first? Out of Wren's lab, too small for its own feet. Look at the two of you now.",
    "Forty years, I never filled this thing. You did it with the same knees everybody gets.",
    "I keep thinking about Route 1. All that walking, and it's still the grass I like best.",
    "Biscuit's asleep on my boot. Old fool. Nobody ever caught a better one, whatever this list says.",
    "That's all of them, and I've told you all of me. There's no more of either. Thank you for asking.",
    "Go on, then. Nothing to catch means you can just go and look at them. That was always the good part.",
};
#define DX_FULL (sizeof FULL / sizeof FULL[0])

/* left when he starts counting them down: three, or a tenth of the Dex */
static int nearly(const Dex *d){
    int left = d->n - d->n_caught, near = d->n / 10 < 3 ? 3 : d->n / 10;
    return left > 0 && left <= near && d->n_caught >= d->n / 2;
}

/* the piece of his story this many caught has opened, and whether it opened on this very count */
static unsigned story_at(const Dex *d, int *fresh){
    unsigned i = (unsigned)d->n_caught * DX_STORY / d->n;
    if(i >= DX_STORY) i = DX_STORY - 1;
    /* the first count that reaches piece i */
    *fresh = d->n_caught && (unsigned)(d->n_caught - 1) * DX_STORY / d->n < i;
    return i;
}

/* Each Talk moves on: what to do next, then a piece of who he is, then where he met
 * one of yours; on the catch that opens a new piece, he tells that first. */
void dx_greeting(Dex *d, char *out, size_t cap, uint8_t *head){
    unsigned k = d->said++;
    if(!d->has_save || !d->n_caught || !d->n){ dx_progress(d, out, cap, head); return; }
    if(d->n_caught >= d->n){
        if(!k) dx_progress(d, out, cap, head);
        else snprintf(out, cap, "%s", FULL[(k - 1) % DX_FULL]);
        return;
    }
    if(nearly(d)){
        char left[24];
        if(k % 2 && d->n_seen < d->n){ dx_progress(d, out, cap, head); return; }
        dx_number((unsigned)(d->n - d->n_caught), left, sizeof left);
        const char *f = NEARLY[(k / 2 + (unsigned)d->n_caught) % DX_NEARLY];
        const char *p = strstr(f, "%s");
        snprintf(out, cap, "%.*s%s%s", (int)(p - f), f, left, p + 2);
        return;
    }
    int fresh;
    unsigned piece = story_at(d, &fresh), step = k + (fresh ? 1 : 0);
    switch(step % 3){
    case 0: dx_progress(d, out, cap, head); return;
    case 2: {
        const char *l = memory_of_caught(d, step / 3);
        if(l){ snprintf(out, cap, "%s", l); return; }
        dx_progress(d, out, cap, head);         /* none of yours he's met: back to what to do next */
        return;
    }
    }
    /* he goes back over earlier pieces, newest first, once you've heard the latest */
    unsigned back = step / 3;
    snprintf(out, cap, "%s", STORY[piece - back % (piece + 1)]);
}

void dx_talk(Dex *d, int pos, char *out, size_t cap, uint8_t *head){
    if(pos < 0 || pos >= d->n || d->state[pos] != DX_CAUGHT){ snprintf(out, cap, "Nothing on this one yet."); return; }
    MgMon m;
    DxWhere w;
    char t[DX_NAME * 5], name[DX_NAME];
    int again = d->talk_pos == pos + 1 && d->talk_n % 2;
    d->talk_n = d->talk_pos == pos + 1 ? d->talk_n + 1 : 1;
    d->talk_pos = (int16_t)(pos + 1);
    if(mg_mon(&d->g, d->mon[pos], &m) || mg_text(&d->g, m.name, name, sizeof name) < 0 ||
       !head || dx_where(d, d->mon[pos], &w, head)){
        snprintf(out, cap, "My notes on that one are smudged. Ask me again.");
        return;
    }
    if(again && dx_memory(name)){ snprintf(out, cap, "%s", dx_memory(name)); return; }   /* Talk again: where he met one */
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
