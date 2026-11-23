/* Synthetic YOLOv5 heads: coordinate mapping, quantization, padding and NMS. */
#include "../src/connector/connector_decode.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void put_anchor(int8_t *data, unsigned side, unsigned stride, unsigned lanes,
                       unsigned anchor, unsigned cls)
{
    const unsigned x = 6, y = 3;
    for (unsigned field = 0; field < 7; ++field) {
        unsigned c = 7*anchor + field;
        size_t offset = (((size_t)(c/lanes)*side + y)*stride + x)*lanes + c%lanes;
        int raw = field < 4 ? 64 : 0;
        if (field == 4 || field == 5 + cls) raw = 112;
        data[offset] = (int8_t)(raw - 128);
    }
}

int main(void)
{
    unsigned layouts[] = {1, 16, 21}; /* NCHW, NC1HWC2, NHWC */
    for (unsigned layout = 0; layout < 3; ++layout) {
        struct decoder *d = calloc(1, sizeof(*d));
        assert(d);
        for (unsigned h = 0; h < 3; ++h) {
            unsigned side = 80u >> h, stride = side + 3, lanes = layouts[layout];
            unsigned channels = ((21+lanes-1)/lanes)*lanes;
            size_t bytes = (size_t)channels*stride*side;
            int8_t *data = malloc(bytes);
            assert(data);
            memset(data, 127, bytes); /* poisonous values in unused channels / padded columns */
            for (unsigned c = 0; c < 21; ++c)
                for (unsigned y = 0; y < side; ++y)
                    for (unsigned x = 0; x < side; ++x)
                        data[(((size_t)(c/lanes)*side+y)*stride+x)*lanes+c%lanes] = -128;
            for (unsigned a = 0; a < 3; ++a) {
                d->anchors[h][a][0] = 10;
                d->anchors[h][a][1] = 12;
            }
            if (h == 1) {
                put_anchor(data, side, stride, lanes, 0, 0);
                put_anchor(data, side, stride, lanes, 1, 0);
                put_anchor(data, side, stride, lanes, 2, 1);
            }
            decode_head(d, h, data, side, stride, lanes, -128, 1.0f/128);
            free(data);
        }
        assert(d->scanned == 25200 && d->count == 3);
        assert(d->best.x1 == 99 && d->best.y1 == 50);
        assert(d->best.x2 == 109 && d->best.y2 == 62);
        assert(fabsf(d->best.score - 0.765625f) < 1e-6f);
        assert(suppress_overlaps(d) == 2);
        assert(d->candidates[0].cls == 0 && d->candidates[1].cls == 1);
        free(d);
    }
    return 0;
}
