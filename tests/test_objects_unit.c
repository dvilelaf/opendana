/* Object catalogue: every supported object type resolves to a class, and an
 * unsupported type resolves to nothing. Synthetic; no content and no SDL. */
#include "test.h"
#include "opendana/objects.h"

#include <stdint.h>

static void test_objects_unknown(void)
{
    CHECK(od_object_type(0x10) == NULL);
    CHECK(od_object_type(0x84) == NULL);
    CHECK(od_object_type(0xff) == NULL);
}

OD_TEST(test_objects_unknown)

/* The catalogue covers every supported type and does not guess: magic removes
 * only ice, and a pipe's entry mask is right=1, left=2, up=4, down=8. */
static void test_objects_catalogue(void)
{
    static const struct {
        uint8_t type;
        od_object_class cls;
        uint8_t magic;
        uint8_t entries;
    } expected[] = {
        {0x00, OD_OBJECT_EMPTY, 0, 0}, {0x01, OD_OBJECT_WALL, 0, 0},
        {0x02, OD_OBJECT_ICE, 1, 0},   {0x03, OD_OBJECT_ICE, 1, 0},
        {0x04, OD_OBJECT_ICE, 1, 0},   {0x05, OD_OBJECT_ICE, 1, 0},
        {0x06, OD_OBJECT_PIPE, 0, 12}, {0x07, OD_OBJECT_PIPE, 0, 3},
        {0x08, OD_OBJECT_PIPE, 0, 6},  {0x09, OD_OBJECT_PIPE, 0, 5},
        {0x0a, OD_OBJECT_PIPE, 0, 10}, {0x0b, OD_OBJECT_PIPE, 0, 9},
        {0x0c, OD_OBJECT_STONE, 0, 0}, {0x0d, OD_OBJECT_REACTIVE, 0, 0},
        {0x0e, OD_OBJECT_FIRE, 0, 0},  {0x0f, OD_OBJECT_HAZARD, 0, 0},
        {0x80, OD_OBJECT_SCROLL, 0, 0},
        {0x81, OD_OBJECT_FLAME, 0, 0}, {0x82, OD_OBJECT_FLAME, 0, 0},
        {0x83, OD_OBJECT_FLAME, 0, 0},
    };
    for (unsigned i = 0; i < sizeof expected / sizeof expected[0]; ++i) {
        const od_object_info *info = od_object_type(expected[i].type);
        CHECK(info != NULL);
        if (!info) continue;
        CHECK_EQ_U32(info->class_id, expected[i].cls);
        CHECK_EQ_U32(info->magic_removable, expected[i].magic);
        CHECK_EQ_U32(info->pipe_entries, expected[i].entries);
    }
}

OD_TEST(test_objects_catalogue)
