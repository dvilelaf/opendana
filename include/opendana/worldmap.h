/* worldmap.h - the "WORLD n" level-select screen and the progression model.
 *
 * Progression model:
 *
 *   - The 150 committed levels are grouped like the original game: levels
 *     0..99 are the ten worlds (world = level/10, round = level%10, both
 *     0-based) and levels 100..149 are the hidden rounds (worlds 11..15).
 *   - A level is unlocked when the previous one has been cleared (level 0 is
 *     always unlocked). Clearing a level marks it and moves the cursor to the
 *     next round, so it becomes selectable. Clearing a hidden level marks it
 *     but never moves the map cursor past level 99.
 *   - The map shows one world at a time with ten markers: rounds 1..9 drawn as
 *     oval markers and round 10 drawn as the world's castle. LEFT/RIGHT move
 *     the cursor within the world, UP/DOWN change world (when its first round
 *     is unlocked), A enters the selected round when it is unlocked.
 *   - Levels 100..149 are not on the map. They are reached through the hidden
 *     menu: on the title screen holding SELECT and pressing B ten times opens
 *     "ROUND 11-1 / SOUND 00"; A/B step the round by one (100..149 with wrap),
 *     SELECT+A/B step a whole world (10), and START loads it. See
 *     od_hidden_menu.
 *
 * The reference map is a decorative artwork we do not reproduce tile for tile;
 * the functional layout (title, marker row, castle, Hero cursor) is matched and
 * documented as an approximation.
 *
 * Pure C11, no SDL. */
#ifndef OD_WORLDMAP_H
#define OD_WORLDMAP_H

#include "opendana/common.h"
#include "opendana/compose.h"
#include "opendana/palette.h"

#define OD_WORLD_COUNT 10           /* worlds 1..10                     */
#define OD_WORLD_ROUNDS 10          /* rounds 1..10 per world           */
#define OD_MAP_SLOTS 10             /* 9 rounds + the castle            */
#define OD_HIDDEN_FIRST 100         /* first hidden level index         */
#define OD_HIDDEN_COUNT 50          /* hidden levels 100..149           */
#define OD_HIDDEN_LAST (OD_HIDDEN_FIRST + OD_HIDDEN_COUNT - 1)

/* ---- progression model ------------------------------------------------- */

typedef struct {
    uint8_t cleared[(OD_LEVEL_COUNT + 7) / 8]; /* one bit per level      */
    int world;   /* world shown, 0..OD_WORLD_COUNT-1                       */
    int cursor;  /* selected marker, 0..OD_MAP_SLOTS-1                     */
} od_progress;

void od_progress_reset(od_progress *p);

int  od_progress_cleared(const od_progress *p, int level);
void od_progress_set_cleared(od_progress *p, int level, int on);
int  od_progress_clear_count(const od_progress *p);

/* A level is unlocked when it is the first or the previous one is cleared. */
int  od_progress_unlocked(const od_progress *p, int level);

/* Marker state for `round` in the current world: 0 locked, 1 open, 2 cleared. */
int  od_progress_marker_state(const od_progress *p, int round);

/* Move the cursor: dx within the world (clamped to 0..9), dy across worlds
 * (only to a world whose round 1 is unlocked). Returns 1 if anything changed. */
int  od_progress_move(od_progress *p, int dx, int dy);

/* Level index under the cursor if it is unlocked; -1 otherwise. */
int  od_progress_enter(const od_progress *p, int *out_level);

/* Mark `level` cleared and advance the cursor to the following round, unless
 * the next level is hidden (100..149): the map cursor never leaves 0..99. */
void od_progress_complete(od_progress *p, int level);

/* The progress words exactly as the magicword validator leaves them. `out[w]` is
 * world `w+1`'s 16-bit word: its low ten bits are the cleared-round bitmap and
 * bits 14/15 are the original's world flags. They are stamped while walking the
 * ten words with a latch:
 *   - a world with all ten rounds cleared -> high byte with bits 14 and 15 set;
 *   - the first world with exactly rounds 1..9 cleared, when every earlier
 *     world is complete, -> high byte with bit 14 set;
 *   - any other world -> no high bits.
 * This is the password path's own value, not a re-derived approximation; it is
 * what a projection of the progress words must expose. */
void od_progress_map_words(const od_progress *p, uint16_t out[OD_WORLD_COUNT]);
uint16_t od_progress_map_word(const od_progress *p, int world);

/* END-OF-WORLD: what the game does on the clear of `level` (0..99) given the
 * progress *before* it: the round's bit is OR'd into the progress words
 * (rebuilt with od_progress_map_words, which are the original's for a session
 * played in order or loaded from a magicword) and the worlds are walked from 0
 * until the first one without a full low byte or high byte. The first time a
 * world reaches a full low byte with round 9 or 10 done and bit 6 clear, the
 * castle-appearance sequence of that world plays (OD_CLEAR_CASTLE_APPEARS; not
 * reproduced by the product yet); a complete world with bit 7 clear plays the
 * end-of-world sequence of the cleared round's world
 * (OD_CLEAR_WORLD_END, `*world` = that world 0..8; world 10 is the combat,
 * handled by the flow) and, after world 9's, the castle opening
 * (`*castle_open` = 1). OD_CLEAR_NONE otherwise. */
typedef enum {
    OD_CLEAR_NONE = 0,
    OD_CLEAR_WORLD_END,
    OD_CLEAR_CASTLE_APPEARS
} od_clear_cinematic;
od_clear_cinematic od_progress_clear_cinematic(const od_progress *before, int level,
                                                 int *world, int *castle_open);

/* 1-based world/round label of any committed level (0..149):
 * 0 -> 1-1, 99 -> 10-10, 100 -> 11-1, 149 -> 15-10. Either pointer may be
 * NULL. Out-of-range levels are clamped to 0 so a caller always has a label. */
void od_level_world_round(int level, int *world, int *round);

static inline int od_level_hidden(int level)
{
    return level >= OD_HIDDEN_FIRST && level < OD_LEVEL_COUNT;
}

/* ---- hidden menu (levels 100..149) ------------------------------------- */

/* Trigger count measured on the original: with SELECT held, the first B arms the
 * counter and the tenth B switches to the hidden menu. */
#define OD_HIDDEN_B_PRESSES 10

/* State of the hidden menu. `level` is the original's level value (100..149);
 * `sound` is the SOUND value the screen shows (the game keeps it at 00). The
 * menu is pure state: input comes in as the raw `buttons` mask plus the freshly
 * `pressed` edges of the tick, so it can be driven from tests or the app's
 * single simulation clock. */
typedef struct {
    int active;   /* the hidden menu is showing                         */
    int counting; /* SELECT is held, the B counter is armed            */
    int presses;  /* B presses since SELECT was armed (0..10)           */
    int level;    /* 100..149, i.e. ROUND 11-1 .. 15-10                */
    int sound;    /* SOUND value shown; the original keeps 00           */
} od_hidden_menu;

void od_hidden_reset(od_hidden_menu *h);

/* One tick on the title screen. SELECT must be held and B pressed. Returns 1
 * on the tick the menu activates (level = 100), 0 otherwise. A released SELECT
 * resets the counter, so a stray press cannot arm the menu. */
int od_hidden_title_tick(od_hidden_menu *h, unsigned buttons, unsigned pressed);

/* One tick of the hidden menu. A/B step the level by one with wrap
 * across 100..149; with SELECT held they step a whole world (10). START
 * confirms. Returns the 100..149 level to load on the START tick, else -1. */
int od_hidden_menu_tick(od_hidden_menu *h, unsigned buttons, unsigned pressed);

/* "11-1" .. "15-10" for a hidden level index (100..149). */
void od_hidden_round_text(char *out, size_t cap, int level);

/* Draw the hidden menu into a full frame: black, "ROUND w-r" and "SOUND 00" at
 * the reference-measured positions, plus the blue ROUND cursor. The geometry
 * constants below were measured from the reference screen. */
#define OD_HIDDEN_LABEL_X 64       /* "ROUND"/"SOUND" tile column (x = 64)   */
#define OD_HIDDEN_VALUE_RIGHT 144  /* right edge of the SOUND value           */
/* The ROUND value starts at tile column 14 whatever its length: "11-1" ends at
 * x = 143 and "12-10" at x = 151 (reference f1518). */
#define OD_HIDDEN_ROUND_X 112
#define OD_HIDDEN_ROW0_Y 112       /* ROUND text row                          */
#define OD_HIDDEN_ROW1_Y 128       /* SOUND text row                          */
#define OD_HIDDEN_CURSOR_X 56      /* cursor arrow left column                */
#define OD_HIDDEN_CURSOR_Y 112     /* cursor arrow top (first pixel y+1)      */
#define OD_HIDDEN_SOUND_DIGITS 2   /* "SOUND 00" value width                  */

void od_hidden_menu_draw(od_index_frame *fb, const od_hidden_menu *h,
                          const uint8_t *chr, const od_palette_ram *pal);

#endif /* OD_WORLDMAP_H */
