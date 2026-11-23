/* 在 Luckfox 开发板上运行：connector_one_frame MODEL.rknn FRAME.rgb */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rknn_api.h"
#include "connector_decode.h"

enum { IMAGE_SIDE = 640, RGB_CHANNELS = 3, HEADS = 3, HEAD_CHANNELS = 21 };

static int check_call(const char *name, int rc)
{
    if (rc == RKNN_SUCC)
        return 0;
    fprintf(stderr, "%s failed (ret=%d)\n", name, rc);
    return -1;
}

static int bind_buffer(rknn_context ctx, rknn_tensor_attr *attr,
                       rknn_tensor_mem **mem)
{
    uint32_t bytes = attr->size_with_stride ? attr->size_with_stride : attr->size;
    *mem = rknn_create_mem(ctx, bytes);
    if (!*mem || !(*mem)->virt_addr) {
        fprintf(stderr, "rknn_create_mem failed (bytes=%u)\n", bytes);
        return -1;
    }
    return check_call("rknn_set_io_mem", rknn_set_io_mem(ctx, *mem, attr));
}

static int load_rgb(const char *path, const rknn_tensor_attr *attr,
                    rknn_tensor_mem *mem)
{
    uint32_t stride = attr->w_stride ? attr->w_stride : IMAGE_SIDE;
    if (stride < IMAGE_SIDE || (uint64_t)stride * RGB_CHANNELS * IMAGE_SIDE > mem->size) {
        fprintf(stderr, "unexpected input stride=%u or allocation=%u\n", stride, mem->size);
        return -1;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        return -1;
    }
    uint8_t *dst = mem->virt_addr;
    memset(dst, 0, mem->size);
    size_t row_bytes = IMAGE_SIDE * RGB_CHANNELS;
    unsigned y;
    for (y = 0; y < IMAGE_SIDE; ++y) {
        if (fread(dst + (size_t)y * stride * RGB_CHANNELS, 1, row_bytes, fp) != row_bytes)
            break;
    }
    int valid = y == IMAGE_SIDE && fgetc(fp) == EOF && !ferror(fp);
    fclose(fp);
    if (!valid)
        fprintf(stderr, "expected exactly 1228800 RGB bytes in %s\n", path);
    return valid ? 0 : -1;
}

static int check_head_shape(rknn_context ctx, unsigned i)
{
    rknn_tensor_attr a = { .index = i };
    if (check_call("query output shape", rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a))))
        return -1;
    const uint32_t side = 80u >> i;
    if (a.n_dims == 4 && a.dims[0] == 1) {
        if (a.fmt == RKNN_TENSOR_NCHW && a.dims[1] == HEAD_CHANNELS &&
            a.dims[2] == side && a.dims[3] == side)
            return 0;
        if (a.fmt == RKNN_TENSOR_NHWC && a.dims[1] == side &&
            a.dims[2] == side && a.dims[3] == HEAD_CHANNELS)
            return 0;
    }
    fprintf(stderr, "output[%u]: expected %u channels and a %ux%u grid, got %s", i,
            HEAD_CHANNELS, side, side, get_format_string(a.fmt));
    for (unsigned d = 0; d < a.n_dims; ++d) fprintf(stderr, " %u", a.dims[d]);
    fputc('\n', stderr);
    return -1;
}

/* 打印全部有效通道的范围。NC1HWC2 的最后一组通道可能包含对齐填充。 */
static int report_head(unsigned index, const rknn_tensor_attr *a, const rknn_tensor_mem *mem,
                       struct decoder *decoder)
{
    const uint32_t side = 80u >> index;
    const uint32_t stride = a->w_stride ? a->w_stride : side;
    uint32_t groups = 0, lanes = 0;
    if (a->fmt == RKNN_TENSOR_NC1HWC2 && a->n_dims == 5 &&
        a->dims[2] == side && a->dims[3] == side) {
        groups = a->dims[1];
        lanes = a->dims[4];
    } else if (a->fmt == RKNN_TENSOR_NCHW && a->n_dims == 4 &&
               a->dims[1] == HEAD_CHANNELS && a->dims[2] == side && a->dims[3] == side) {
        groups = HEAD_CHANNELS;
        lanes = 1;
    } else if (a->fmt == RKNN_TENSOR_NHWC && a->n_dims == 4 &&
               a->dims[1] == side && a->dims[2] == side && a->dims[3] >= HEAD_CHANNELS) {
        groups = 1;
        lanes = a->dims[3];
    }
    if (a->dims[0] != 1 || a->type != RKNN_TENSOR_INT8 ||
        a->qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC || !(a->scale > 0) ||
        !lanes || groups != (HEAD_CHANNELS + (uint64_t)lanes - 1) / lanes ||
        stride < side || (uint64_t)groups * side * stride * lanes > mem->size) {
        fprintf(stderr, "unexpected output[%u]: format=%s dims=", index, get_format_string(a->fmt));
        for (unsigned i = 0; i < a->n_dims; ++i)
            fprintf(stderr, "%s%u", i ? "x" : "", a->dims[i]);
        fprintf(stderr, " type=%d stride=%u bytes=%u\n", a->type, stride, mem->size);
        return -1;
    }

    const int8_t *data = mem->virt_addr;
    int low = INT8_MAX, high = INT8_MIN;
    for (unsigned c = 0; c < HEAD_CHANNELS; ++c) {
        for (unsigned y = 0; y < side; ++y) {
            size_t row = ((size_t)(c / lanes) * side + y) * stride;
            for (unsigned x = 0; x < side; ++x) {
                int q = data[(row + x) * lanes + c % lanes];
                if (q < low) low = q;
                if (q > high) high = q;
            }
        }
    }
    printf("output[%u] logical=1x%ux%ux%u native=%s min=%.6f max=%.6f\n",
           index, HEAD_CHANNELS, side, side, get_format_string(a->fmt),
           (low - a->zp) * a->scale, (high - a->zp) * a->scale);
    if (decoder)
        decode_head(decoder, index, data, side, stride, lanes, a->zp, a->scale);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s MODEL.rknn FRAME.rgb [RK_anchors.txt]\n", argv[0]);
        return EXIT_FAILURE;
    }
    rknn_context ctx = 0;
    if (check_call("rknn_init", rknn_init(&ctx, argv[1], 0, 0, NULL)))
        return EXIT_FAILURE;

    int result = EXIT_FAILURE;
    rknn_tensor_mem *input_mem = NULL, *outputs[HEADS] = {0};
    rknn_tensor_attr input = {0}, heads[HEADS] = {0};
    rknn_input_output_num count = {0};
    struct decoder *decoder = NULL;
    if (argc == 4) {
        decoder = calloc(1, sizeof(*decoder));
        if (!decoder) {
            perror("allocate decoder");
            goto done;
        }
        if (load_anchors(decoder, argv[3])) goto done;
    }
    if (check_call("query I/O count", rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &count, sizeof(count))))
        goto done;
    printf("model inputs=%u outputs=%u\n", count.n_input, count.n_output);
    if (count.n_input != 1 || count.n_output != HEADS)
        goto done;

    if (check_call("query native input", rknn_query(ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &input, sizeof(input))))
        goto done;
    if (input.fmt != RKNN_TENSOR_NHWC || input.n_dims != 4 ||
        input.dims[0] != 1 || input.dims[1] != IMAGE_SIDE ||
        input.dims[2] != IMAGE_SIDE || input.dims[3] != RGB_CHANNELS) {
        fprintf(stderr, "expected native input 1x640x640x3 NHWC\n");
        goto done;
    }
    input.type = RKNN_TENSOR_UINT8;
    input.pass_through = 0;
    if (bind_buffer(ctx, &input, &input_mem) || load_rgb(argv[2], &input, input_mem))
        goto done;

    for (unsigned i = 0; i < HEADS; ++i) {
        heads[i].index = i;
        if (check_head_shape(ctx, i) ||
            check_call("query native output", rknn_query(ctx, RKNN_QUERY_NATIVE_OUTPUT_ATTR,
                                                       &heads[i], sizeof(heads[i]))) ||
            bind_buffer(ctx, &heads[i], &outputs[i]))
            goto done;
    }

    if (check_call("rknn_run", rknn_run(ctx, NULL)))
        goto done;
    for (unsigned i = 0; i < HEADS; ++i) {
        if (report_head(i, &heads[i], outputs[i], decoder))
            goto done;
    }
    if (decoder) print_boxes(decoder);
    puts("PASS native one-frame inference");
    result = EXIT_SUCCESS;

done:
    free(decoder);
    for (unsigned i = 0; i < HEADS; ++i)
        if (outputs[i]) rknn_destroy_mem(ctx, outputs[i]);
    if (input_mem) rknn_destroy_mem(ctx, input_mem);
    rknn_destroy(ctx);
    return result;
}
