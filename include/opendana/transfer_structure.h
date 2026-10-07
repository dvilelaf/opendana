#ifndef OD_TRANSFER_STRUCTURE_H
#define OD_TRANSFER_STRUCTURE_H
#include "opendana/platform.h"
#define OD_TRANSFER_STRUCTURE_API 1
typedef struct {
    unsigned count;
    uint8_t tile_storage_low, attribute_storage_low;
    uint8_t tiles[239][16], attributes[239];
} od_transfer_atlas;
typedef struct {
    uint8_t atlas_id, columns, rows, header_storage_low, metatiles[64];
} od_transfer_image;
typedef struct {
    uint16_t destinations[12];
    od_transfer_atlas atlases[2];
    od_transfer_image images[31];
} od_transfer_structure;
typedef struct od_transfer_structure_stage od_transfer_structure_stage;
struct cJSON;
od_status od_transfer_structure_prepare_doc(const struct cJSON *doc,
    od_transfer_structure_stage **out);
void od_transfer_structure_commit(od_transfer_structure_stage *s);
void od_transfer_structure_discard(od_transfer_structure_stage *s);
const od_transfer_structure *od_transfer_structure_get(void);
void od_transfer_structure_reset(void);
/* No timings or costs. *_storage_low is the byte-storage phase mod 256, used
 * only to derive where a 256-byte page boundary falls from geometry, IDs and
 * pattern reads. */
#endif
