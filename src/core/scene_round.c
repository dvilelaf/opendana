/* scene_round.c - ROUND focus of the WORLD map. */
#include "opendana/scene_round.h"

void od_scene_round_begin(od_scene_round *r, int round)
{
    if (round < 0)
        round = 0;
    if (round > OD_MAP_ROUND_COUNT - 1)
        round = OD_MAP_ROUND_COUNT - 1;
    r->round = round;
}

int od_scene_round_move(od_scene_round *r, unsigned dir)
{
    if ((dir & 0x01u) && r->round < OD_MAP_ROUND_COUNT - 1) {
        r->round++;
        return 1;
    }
    if ((dir & 0x02u) && r->round > 0) {
        r->round--;
        return 1;
    }
    return 0;
}

int od_scene_round_first(const uint16_t words[OD_MAP_WORLD_COUNT], int world)
{
    if (world < 0 || world >= OD_MAP_WORLD_COUNT)
        return 0;
    unsigned word = words[world] & 0x03ffu;
    int r = 0;
    while (r < OD_MAP_ROUND_COUNT && (word & (1u << r)))
        r++;
    return r < OD_MAP_ROUND_COUNT ? r : OD_MAP_ROUND_COUNT - 1;
}

int od_scene_round_level(int world, int round)
{
    if (world < 0 || world >= OD_MAP_WORLD_COUNT)
        return -1;
    if (round < 0 || round >= OD_MAP_ROUND_COUNT)
        return -1;
    return world * OD_MAP_ROUND_COUNT + round;
}

int od_scene_round_castle(const uint16_t words[OD_MAP_WORLD_COUNT], int world,
                           int round)
{
    if (world < 0 || world > 8 || round != 9)
        return 0;
    return ((words[world] >> 15) & 1u) == 0;
}
