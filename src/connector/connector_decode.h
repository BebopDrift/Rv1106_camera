#ifndef CONNECTOR_DECODE_H
#define CONNECTOR_DECODE_H

#include <stdint.h>

/* Three grids of 80, 40 and 20 cells per side, each with three anchors. */
enum { HEAD_COUNT = 3, ANCHOR_COUNT = 3, CANDIDATE_COUNT = 3 * (80*80 + 40*40 + 20*20) };
struct box {
    float x1, y1, x2, y2;
    float score;
    unsigned cls, order;
};
struct decoder {
    float anchors[HEAD_COUNT][ANCHOR_COUNT][2];
    struct box best;
    unsigned scanned, count;
    struct box candidates[CANDIDATE_COUNT];
};

int load_anchors(struct decoder *d, const char *path);
void decode_head(struct decoder *d, unsigned head, const int8_t *data,
                 unsigned side, unsigned row_stride, unsigned lanes, int zp, float scale);
unsigned suppress_overlaps(struct decoder *d);
void print_boxes(struct decoder *d);

#endif
