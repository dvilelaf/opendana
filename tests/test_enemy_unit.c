/* Enemy model: movement and collision on a small synthetic grid, and the
 * semantic entity-type API. Own grids and entities; no content and no SDL. */
#include "test.h"

#include "opendana/enemy.h"
#include "opendana/level.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t cells[OD_GRID_H][OD_GRID_W];
} test_grid;

static int grid_blocked(void *ctx, int x, int y)
{
    if (x < 0 || x >= OD_GRID_W || y < 0 || y >= OD_GRID_H)
        return 1;
    test_grid *g = (test_grid *)ctx;
    return g->cells[y][x] == OD_OBJ_SOLID;
}

static void grid_fill(test_grid *g, uint8_t value)
{
    for (int y = 0; y < OD_GRID_H; y++)
        for (int x = 0; x < OD_GRID_W; x++)
            g->cells[y][x] = value;
}

static od_enemy *push_entity(od_enemy_list *li, uint8_t type, od_enemy_kind kind,
                              int x, int y, int dir)
{
    od_enemy *e = &li->items[li->count++];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->kind = (uint8_t)kind;
    e->alive = 1;
    e->x = x;
    e->y = y;
    e->home_x = x;
    e->home_y = y;
    e->dir = dir;
    return e;
}

static void test_enemy_movement(void)
{
    printf("- enemy movement/collision\n");

    test_grid grid;
    grid_fill(&grid, OD_OBJ_EMPTY);
    for (int x = 0; x < OD_GRID_W; x++)
        grid.cells[5][x] = OD_OBJ_SOLID; /* floor on row 5 */

    od_enemy_list li;
    li.count = 0;

    /* A faller drops to the floor and then stays put. */
    od_enemy *f = push_entity(&li, 0x80, OD_EKIND_FALLER, 3, 1, 1);
    for (int i = 0; i < 10; i++)
        od_enemy_tick(&li, grid_blocked, &grid);
    CHECK_EQ_U32(f->x, 3);
    CHECK_EQ_U32(f->y, 4); /* rests directly on the floor */

    /* A walker patrols row 4 and turns at the wall it meets on the right. */
    li.count = 0;
    for (int x = 0; x < OD_GRID_W; x++) {
        grid.cells[5][x] = OD_OBJ_SOLID;
        grid.cells[3][x] = OD_OBJ_EMPTY;
    }
    grid.cells[4][6] = OD_OBJ_SOLID; /* wall the walker bumps into */
    od_enemy *w = push_entity(&li, 0x81, OD_EKIND_WALKER, 4, 4, 1);
    od_enemy_tick(&li, grid_blocked, &grid); /* 4 -> 5 */
    CHECK_EQ_U32(w->x, 5);
    CHECK_EQ_U32(w->dir, 1);
    od_enemy_tick(&li, grid_blocked, &grid); /* blocked at 6, turns, steps back */
    CHECK_EQ_U32(w->dir, -1);
    for (int i = 0; i < 3; i++)
        od_enemy_tick(&li, grid_blocked, &grid);
    CHECK_EQ_U32(w->x, 1);
    CHECK_EQ_U32(w->dir, -1);

    /* A walker refuses to step off a ledge (no floor in front of it). */
    li.count = 0;
    grid_fill(&grid, OD_OBJ_EMPTY);
    for (int x = 0; x <= 4; x++)
        grid.cells[5][x] = OD_OBJ_SOLID;
    od_enemy *l = push_entity(&li, 0x81, OD_EKIND_WALKER, 3, 4, 1);
    od_enemy_tick(&li, grid_blocked, &grid); /* 3 -> 4, at the edge */
    CHECK_EQ_U32(l->x, 4);
    od_enemy_tick(&li, grid_blocked, &grid); /* would fall: turns left and back */
    CHECK_EQ_U32(l->dir, -1);
    CHECK_EQ_U32(l->x, 3);
    od_enemy_tick(&li, grid_blocked, &grid);
    CHECK_EQ_U32(l->x, 2);

    /* A static entity never moves. */
    li.count = 0;
    grid_fill(&grid, OD_OBJ_EMPTY);
    od_enemy *s = push_entity(&li, 0x06, OD_EKIND_STATIC, 2, 2, 1);
    for (int i = 0; i < 8; i++)
        od_enemy_tick(&li, grid_blocked, &grid);
    CHECK_EQ_U32(s->x, 2);
    CHECK_EQ_U32(s->y, 2);

    /* With the default bounds-only collision a faller stops on the bottom. */
    li.count = 0;
    od_enemy *g = push_entity(&li, 0x80, OD_EKIND_FALLER, 0, 0, 1);
    for (int i = 0; i < 40; i++)
        od_enemy_tick(&li, NULL, NULL);
    CHECK_EQ_U32(g->y, OD_GRID_H - 1);
}

static void test_enemy_types(void)
{
    printf("- enemy type catalogue\n");
    CHECK_EQ_U32(od_enemy_type(0x00)->kind, OD_EKIND_STATIC);
    CHECK_EQ_U32(od_enemy_type(0x01)->kind, OD_EKIND_STATIC);
    CHECK_EQ_U32(od_enemy_type(0x05)->kind, OD_EKIND_STATIC);
    CHECK_EQ_U32(od_enemy_type(0x06)->kind, OD_EKIND_STATIC);
    CHECK_EQ_U32(od_enemy_type(0x0E)->kind, OD_EKIND_STATIC);
    CHECK_EQ_U32(od_enemy_type(0x1F) != NULL, 1);
    CHECK_EQ_U32(od_enemy_type(0x80)->kind, OD_EKIND_STATIC); /* parameter */
    CHECK_EQ_U32(od_enemy_type(0x81)->kind, OD_EKIND_PATROL_H);
    CHECK_EQ_U32(od_enemy_type(0x82)->kind, OD_EKIND_PATROL_V);
    CHECK_EQ_U32(od_enemy_type(0x83)->kind, OD_EKIND_SPECIAL);
}

OD_TEST(test_enemy_movement)

OD_TEST(test_enemy_types)
