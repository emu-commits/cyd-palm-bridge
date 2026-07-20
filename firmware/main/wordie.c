/* wordie.c -- see wordie.h. Pure C, host-testable. */
#include "wordie.h"
#include <string.h>

/* Common five-letter answer bank. Curated common words (no proper nouns); the daily
 * index wraps modulo the count, so the same date yields the same word for everyone. */
static const char WD_BANK[][6] = {
    "ABOUT","ABOVE","ABUSE","ACTOR","ACUTE","ADMIT","ADOPT","ADULT","AFTER","AGAIN","AGENT","AGREE",
    "AHEAD","ALARM","ALBUM","ALERT","ALIKE","ALIVE","ALLOW","ALONE","ALONG","ALTER","AMONG","ANGLE",
    "ANGRY","APART","APPLE","APPLY","ARENA","ARGUE","ARISE","ARMOR","ARRAY","ASIDE","ASSET","AUDIO",
    "AUDIT","AVOID","AWAKE","AWARD","AWARE","BADLY","BAKER","BASES","BASIC","BASIS","BEACH","BEARD",
    "BEAST","BEGIN","BEING","BELOW","BENCH","BIRTH","BLACK","BLAME","BLANK","BLAST","BLAZE","BLEED",
    "BLEND","BLESS","BLIND","BLOCK","BLOOD","BLOOM","BOARD","BOAST","BONUS","BOOST","BOOTH","BOUND",
    "BRAIN","BRAKE","BRAND","BRAVE","BREAD","BREAK","BREED","BRICK","BRIDE","BRIEF","BRING","BROAD",
    "BROKE","BROWN","BRUSH","BUILD","BUILT","BUNCH","BURST","BUYER","CABIN","CABLE","CANDY","CARGO",
    "CARRY","CATCH","CAUSE","CHAIN","CHAIR","CHALK","CHAOS","CHARM","CHART","CHASE","CHEAP","CHECK",
    "CHESS","CHEST","CHIEF","CHILD","CHILL","CHINA","CHOIR","CHORD","CHOSE","CIVIC","CIVIL","CLAIM",
    "CLASH","CLASS","CLEAN","CLEAR","CLERK","CLICK","CLIFF","CLIMB","CLOAK","CLOCK","CLOSE","CLOTH",
    "CLOUD","CLOWN","CLUED","COACH","COAST","COULD","COUNT","COURT","COVER","CRACK","CRAFT","CRASH",
    "CRAZY","CREAM","CREEK","CREST","CRIME","CRISP","CROSS","CROWD","CROWN","CRUDE","CRUEL","CURVE",
    "CYCLE","DAILY","DAIRY","DANCE","DEALT","DEATH","DEBUT","DELAY","DENSE","DEPTH","DIARY","DIRTY",
    "DITCH","DIZZY","DODGE","DOING","DONOR","DOUBT","DOZEN","DRAFT","DRAIN","DRAMA","DRANK","DRAWN",
    "DREAM","DRESS","DRIED","DRIFT","DRILL","DRINK","DRIVE","DRONE","DROVE","DROWN","DRUMS","DRYER",
    "EAGER","EAGLE","EARLY","EARTH","EATEN","EBONY","EIGHT","ELBOW","ELDER","ELECT","ELITE","EMAIL",
    "EMPTY","ENEMY","ENJOY","ENTER","ENTRY","EQUAL","ERROR","ESSAY","EVENT","EVERY","EXACT","EXAMS",
    "EXIST","EXTRA","FABLE","FAINT","FAIRY","FAITH","FALSE","FANCY","FATAL","FAULT","FAVOR","FEAST",
    "FENCE","FETCH","FEVER","FIBER","FIELD","FIERY","FIFTH","FIFTY","FIGHT","FINAL","FIRST","FIXED",
    "FLAME","FLASH","FLEET","FLESH","FLICK","FLING","FLOAT","FLOCK","FLOOD","FLOOR","FLOUR","FLUID",
    "FLUTE","FOCUS","FORCE","FORGE","FORTH","FORTY","FORUM","FOUND","FRAME","FRANK","FRAUD","FRESH",
    "FRONT","FROST","FROWN","FRUIT","FULLY","FUNNY","GAUGE","GEESE","GHOST","GIANT","GIVEN","GIVER",
    "GLASS","GLEAM","GLOBE","GLOOM","GLORY","GLOVE","GOING","GRACE","GRADE","GRAIN","GRAND","GRANT",
    "GRAPE","GRAPH","GRASP","GRASS","GRAVE","GRAVY","GRAZE","GREAT","GREED","GREEN","GREET","GRIEF",
    "GRILL","GRIND","GROAN","GROOM","GROSS","GROUP","GROVE","GROWN","GUARD","GUESS","GUEST","GUIDE",
    "HABIT","HAPPY","HARDY","HARSH","HASTE","HATCH","HAUNT","HEART","HEAVY","HEDGE","HELLO","HOBBY",
    "HONEY","HONOR","HORSE","HOTEL","HOUSE","HOVER","HUMAN","HUMOR","IDEAL","IMAGE","INBOX","INDEX",
    "INNER","INPUT","IRONY","ISSUE","IVORY","JAPAN","JEANS","JEWEL","JOINT","JOKER","JOLLY","JUDGE",
    "JUICE","JUICY","JUMBO","KNACK","KNEES","KNELT","KNIFE","KNOCK","KNOWN","LABEL","LABOR","LADEN",
    "LAGER","LANCE","LARGE","LASER","LATER","LAUGH","LAYER","LEARN","LEASE","LEAST","LEAVE","LEDGE",
    "LEGAL","LEMON","LEVEL","LEVER","LIGHT","LIMIT","LINEN","LIVER","LOBBY","LOCAL","LOGIC","LOOSE",
    "LORRY","LOSER","LOVER","LOWER","LOYAL","LUCKY","LUNAR","LUNCH","LYING","MACRO","MAGIC","MAJOR",
    "MAKER","MANGO","MANOR","MAPLE","MARCH","MARSH","MATCH","MAYBE","MAYOR","MEANT","MEDAL","MEDIA",
    "MELON","MERCY","MERGE","MERIT","MERRY","METAL","METER","MIDST","MIGHT","MINOR","MINUS","MIXED",
    "MODEL","MONEY","MONTH","MORAL","MOTOR","MOUND","MOUNT","MOURN","MOUSE","MOUTH","MOVER","MOVIE",
    "MUCKY","MUSIC","NAIVE","NAVAL","NAVEL","NEEDY","NERVE","NEVER","NEWLY","NIGHT","NOBLE","NOISE",
    "NORTH","NOTCH","NOVEL","NURSE","OCEAN","OFFER","OFTEN","OLIVE","ONION","OPERA","ORBIT","ORDER",
    "ORGAN","OTHER","OTTER","OUGHT","OUNCE","OUTER","OWNER","OXIDE","OZONE","PAINT","PANEL","PANIC",
    "PAPER","PARTY","PASTA","PATCH","PAUSE","PEACE","PEACH","PEARL","PENNY","PERCH","PETAL","PHOTO",
    "PIANO","PIECE","PILOT","PINCH","PITCH","PIVOT","PIXEL","PLACE","PLAIN","PLANE","PLANT","PLATE",
    "PLAZA","PLEAD","PLUCK","PLUMB","PLUMP","PLUSH","POEMS","POINT","POLAR","PORCH","POUND","POWER",
    "PRESS","PRICE","PRIDE","PRIME","PRINT","PRIOR","PRIZE","PROBE","PRONE","PROOF","PROUD","PROVE",
    "PROXY","PULSE","PUNCH","PUPIL","PUPPY","PURGE","PURSE","QUACK","QUAFF","QUAIL","QUAKE","QUEEN",
    "QUERY","QUEST","QUEUE","QUICK","QUIET","QUILT","QUIRK","QUITE","QUOTA","QUOTE","RADAR","RADIO",
    "RAISE","RALLY","RANCH","RANGE","RAPID","RAVEN","RAZOR","REACH","REACT","READY","REALM","REBEL",
    "REFER","REIGN","RELAX","RENEW","REPLY","RESET","RETRO","RHYME","RIDER","RIDGE","RIFLE","RIGHT",
    "RIGID","RINSE","RIPEN","RIVAL","RIVER","ROAST","ROBIN","ROBOT","ROCKY","ROGUE","ROMAN","ROUGH",
    "ROUND","ROUTE","ROYAL","RUGBY","RUINS","RULER","RUMOR","RURAL","SADLY","SAINT","SALAD","SALON",
    "SALSA","SALTY","SANDY","SATIN","SAUCE","SCALE","SCALP","SCARE","SCARF","SCENE","SCENT","SCOOP",
    "SCOPE","SCORE","SCOUT","SCRAP","SCREW","SCRUB","SEIZE","SENSE","SERVE","SEVEN","SHADE","SHAFT",
    "SHAKE","SHALL","SHAME","SHAPE","SHARE","SHARK","SHARP","SHEEP","SHEET","SHELF","SHELL","SHIFT",
    "SHINE","SHINY","SHIRT","SHOCK","SHONE","SHOOK","SHOOT","SHORE","SHORT","SHOUT","SHOWN","SHRUB",
    "SIGHT","SILKY","SILLY","SINCE","SIREN","SIXTH","SIXTY","SKATE","SKILL","SKIRT","SKULL","SLACK",
    "SLATE","SLEEP","SLICE","SLIDE","SLIME","SLING","SLOPE","SLUMP","SMART","SMASH","SMELL","SMILE",
    "SMOKE","SNACK","SNAKE","SNEAK","SNOWY","SOLAR","SOLID","SOLVE","SONIC","SOUND","SOUTH","SPACE",
    "SPARE","SPARK","SPEAK","SPEAR","SPEED","SPELL","SPEND","SPENT","SPICE","SPICY","SPIKE","SPINE",
    "SPIRE","SPITE","SPLAT","SPLIT","SPOKE","SPOON","SPORT","SPRAY","SQUAD","SQUAT","STACK","STAFF",
    "STAGE","STAIR","STAKE","STALE","STALK","STALL","STAMP","STAND","STARE","START","STATE","STEAK",
    "STEAL","STEAM","STEEL","STEEP","STEER","STERN","STICK","STIFF","STILL","STING","STOCK","STONE",
    "STOOD","STOOL","STORE","STORM","STORY","STOVE","STRAP","STRAW","STRAY","STRIP","STUCK","STUDY",
    "STUFF","STUMP","STYLE","SUGAR","SUITE","SUNNY","SUPER","SURGE","SWAMP","SWARM","SWEAR","SWEAT",
    "SWEEP","SWEET","SWEPT","SWIFT","SWING","SWORD","SYRUP","TABLE","TAKEN","TALLY","TANGO","TAPER",
    "TASTE","TEACH","TEETH","TEMPO","TENOR","TENSE","TENTH","THANK","THEFT","THEIR","THEME","THERE",
    "THESE","THICK","THIEF","THING","THINK","THIRD","THOSE","THREE","THREW","THROW","THUMB","TIDAL",
    "TIGER","TIGHT","TIMER","TITLE","TOAST","TODAY","TOKEN","TONIC","TOOTH","TOPIC","TORCH","TOTAL",
    "TOUCH","TOUGH","TOWER","TOXIC","TRACE","TRACK","TRADE","TRAIL","TRAIN","TRAIT","TRAMP","TRASH",
    "TREAD","TREAT","TREND","TRIAL","TRIBE","TRICK","TRIED","TRIPE","TROOP","TROUT","TRUCE","TRUCK",
    "TRULY","TRUNK","TRUST","TRUTH","TULIP","TUMOR","TUNIC","TURBO","TUTOR","TWICE","TWINS","TWIST",
    "TYING","UDDER","ULTRA","UNCLE","UNDER","UNDUE","UNFIT","UNION","UNITE","UNITY","UNTIL","UPPER",
    "UPSET","URBAN","USAGE","USHER","USUAL","VAGUE","VALID","VALUE","VALVE","VAPOR","VAULT","VENUE",
    "VERGE","VERSE","VICAR","VIDEO","VIGOR","VILLA","VINYL","VIOLA","VIRAL","VIRUS","VISIT","VITAL",
    "VIVID","VOCAL","VODKA","VOGUE","VOICE","VOTER","VOWEL","WAFER","WAGER","WAGON","WAIST","WALTZ",
    "WASTE","WATCH","WATER","WAVER","WEARY","WEAVE","WEDGE","WEIGH","WEIRD","WHALE","WHEAT","WHEEL",
    "WHERE","WHICH","WHILE","WHITE","WHOLE","WHOSE","WIDEN","WIDOW","WIDTH","WIELD","WITCH","WOKEN",
    "WOMAN","WORLD","WORRY","WORSE","WORST","WORTH","WOULD","WOUND","WOVEN","WRATH","WRECK","WRIST",
    "WRITE","WRONG","WROTE","YACHT","YEARN","YEAST","YIELD","YOUNG","YOUTH","ZEBRA","ZESTY",
};
#define WD_NBANK ((int)(sizeof(WD_BANK) / sizeof(WD_BANK[0])))

int wd_nbank(void){ return WD_NBANK; }

/* xorshift32 -- a seed reproduces a puzzle exactly (matches minesweeper.c). */
static uint32_t xs(uint32_t *s){
    uint32_t x = *s ? *s : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

static void wd_start(WdGame *g, int idx){
    memset(g, 0, sizeof *g);
    if(idx < 0) idx = 0;
    idx %= WD_NBANK;
    memcpy(g->answer, WD_BANK[idx], WD_LEN + 1);
    g->state = WD_PLAY;
}

void wd_daily(WdGame *g, long day){
    long idx = day % WD_NBANK;
    if(idx < 0) idx += WD_NBANK;           /* well-defined for pre-epoch days too */
    wd_start(g, (int)idx);
}

void wd_random(WdGame *g, uint32_t seed){
    uint32_t s = seed;
    wd_start(g, (int)(xs(&s) % (uint32_t)WD_NBANK));
}

int wd_addch(WdGame *g, char c){
    if(g->state != WD_PLAY) return 0;
    if(g->cur >= WD_LEN) return 0;
    if(c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if(c < 'A' || c > 'Z') return 0;
    g->row[g->cur++] = c;
    g->row[g->cur] = 0;
    return 1;
}

int wd_del(WdGame *g){
    if(g->state != WD_PLAY) return 0;
    if(g->cur == 0) return 0;
    g->row[--g->cur] = 0;
    return 1;
}

/* Wordle two-pass scoring: pass 1 marks exact-position hits and tallies the answer's
 * remaining (non-hit) letters; pass 2 marks PRESENT only while that tally lasts, so a
 * doubled guess letter is not over-credited against a single answer letter. */
static void wd_score(const char *ans, const char *guess, uint8_t *m){
    int cnt[26] = {0};
    for(int i = 0; i < WD_LEN; i++){
        if(guess[i] == ans[i]) m[i] = WD_CORRECT;
        else { m[i] = WD_ABSENT; cnt[ans[i] - 'A']++; }
    }
    for(int i = 0; i < WD_LEN; i++){
        if(m[i] == WD_CORRECT) continue;
        int k = guess[i] - 'A';
        if(k >= 0 && k < 26 && cnt[k] > 0){ m[i] = WD_PRESENT; cnt[k]--; }
    }
}

int wd_enter(WdGame *g){
    if(g->state != WD_PLAY) return 0;
    if(g->cur != WD_LEN) return 0;                 /* need a full row */
    int r = g->nrows;
    memcpy(g->guess[r], g->row, WD_LEN + 1);
    wd_score(g->answer, g->guess[r], g->mark[r]);

    for(int i = 0; i < WD_LEN; i++){               /* fold into keyboard states */
        int k = g->guess[r][i] - 'A';
        if(k < 0 || k >= 26) continue;
        uint8_t want = g->mark[r][i] == WD_CORRECT ? WK_CORRECT :
                       g->mark[r][i] == WD_PRESENT ? WK_PRESENT : WK_ABSENT;
        if(want > g->key[k]) g->key[k] = want;
    }

    g->nrows++;
    g->cur = 0; g->row[0] = 0;
    if(memcmp(g->guess[r], g->answer, WD_LEN) == 0) g->state = WD_WON;
    else if(g->nrows >= WD_ROWS)                    g->state = WD_LOST;
    return 1;
}
