#include "connector_decode.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const float score_threshold = 0.25f;
static const float nms_threshold = 0.45f;

int load_anchors(struct decoder *d, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror(path);
        return -1;
    }
    int ok = 1;
    for (unsigned h = 0; h < HEAD_COUNT; ++h)
        for (unsigned a = 0; a < ANCHOR_COUNT; ++a)
            for (unsigned k = 0; k < 2; ++k) {
                float value;
                if (fscanf(fp, "%f", &value) != 1 || !isfinite(value) || value <= 0) {
                    ok = 0;
                    goto done;
                }
                d->anchors[h][a][k] = value;
            }
    char extra;
    ok = fscanf(fp, " %c", &extra) == EOF && !ferror(fp);
done:
    fclose(fp);
    if (!ok) fprintf(stderr, "%s: expected 18 positive anchor dimensions\n", path);
    return ok ? 0 : -1;
}

void decode_head(struct decoder *d, unsigned head, const int8_t *data,
                 unsigned side, unsigned row_stride, unsigned lanes, int zp, float scale)
{
    const float step = 640.0f / side;
    /* report_head has already checked the buffer size, format and quantization. */
    for (unsigned a = 0; a < ANCHOR_COUNT; ++a) {
        for (unsigned y = 0; y < side; ++y) {
            for (unsigned x = 0; x < side; ++x) {
                float v[7];
                for (unsigned field = 0; field < 7; ++field) {
                    unsigned c = a * 7 + field;
                    size_t pixel = ((size_t)(c / lanes) * side + y) * row_stride + x;
                    v[field] = (data[pixel * lanes + c % lanes] - zp) * scale;
                }
                unsigned cls = v[6] > v[5] ? 1 : 0;
                float score = v[4] * v[5 + cls];
                unsigned order = d->scanned++;
                if (score < score_threshold && order && score <= d->best.score)
                    continue;
                float cx = (x + 2*v[0] - 0.5f) * step;
                float cy = (y + 2*v[1] - 0.5f) * step;
                float half_w = 2*v[2]*v[2] * d->anchors[head][a][0];
                float half_h = 2*v[3]*v[3] * d->anchors[head][a][1];
                struct box b = {cx-half_w, cy-half_h, cx+half_w, cy+half_h, score, cls, order};
                if (!order || score > d->best.score) d->best = b;
                if (score >= score_threshold && half_w > 0 && half_h > 0)
                    d->candidates[d->count++] = b;
            }
        }
    }
}

static int highest_first(const void *left, const void *right)
{
    const struct box *a = left, *b = right;
    if (a->score != b->score) return a->score < b->score ? 1 : -1;
    return (a->order > b->order) - (a->order < b->order);
}

static float iou(const struct box *a, const struct box *b)
{
    float w = fminf(a->x2, b->x2) - fmaxf(a->x1, b->x1);
    float h = fminf(a->y2, b->y2) - fmaxf(a->y1, b->y1);
    if (w <= 0 || h <= 0) return 0;
    float intersection = w*h;
    float area_a = (a->x2-a->x1) * (a->y2-a->y1);
    float area_b = (b->x2-b->x1) * (b->y2-b->y1);
    return intersection / (area_a + area_b - intersection);
}

unsigned suppress_overlaps(struct decoder *d)
{
    qsort(d->candidates, d->count, sizeof(struct box), highest_first);
    unsigned kept = 0;
    for (unsigned j = 0; j < d->count && kept < 300; ++j) {
        struct box candidate = d->candidates[j];
        unsigned k = 0;
        for (; k < kept; ++k) {
            const struct box *accepted = &d->candidates[k];
            if (candidate.cls == accepted->cls && iou(&candidate, accepted) > nms_threshold) break;
        }
        if (k == kept) d->candidates[kept++] = candidate;
    }
    return kept;
}

static void print_box(const char *kind, const struct box *b)
{
    printf("%s class=%u score=%.8f xyxy=%.6f,%.6f,%.6f,%.6f\n",
           kind, b->cls, b->score, b->x1, b->y1, b->x2, b->y2);
}

void print_boxes(struct decoder *d)
{
    unsigned kept = suppress_overlaps(d);
    printf("DECODE scanned=%u above=%u kept=%u score_min=%.2f nms_iou=%.2f\n",
           d->scanned, d->count, kept, score_threshold, nms_threshold);
    print_box("BEST_RAW", &d->best);
    for (unsigned i = 0; i < kept; ++i)
        print_box("DETECTION", &d->candidates[i]);
}
