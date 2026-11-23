#define _POSIX_C_SOURCE 200809L
/* Convert the SC3336 capture program's packed, full-range NV12 frame to model RGB. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { WIDTH = 2304, HEIGHT = 1296, SIDE = 640, CONTENT_H = 360,
       PAD_Y = (SIDE-CONTENT_H)/2, NV12_BYTES = WIDTH*HEIGHT*3/2 };

struct sample {
    unsigned first, second;
    float fraction;
};

/* Map output pixel centers to input pixel centers, clamped at the image edge. */
static struct sample sample_at(unsigned dst, unsigned src_len, unsigned dst_len)
{
    float pos = ((float)dst + 0.5f) * src_len / dst_len - 0.5f;
    if (pos < 0) pos = 0;
    if (pos > src_len - 1) pos = src_len - 1;
    unsigned i = (unsigned)pos;
    return (struct sample){i, i+1 < src_len ? i+1 : i, pos-i};
}

static float interpolate(const uint8_t *plane, unsigned step, struct sample x, struct sample y)
{
    const uint8_t *upper = plane + (size_t)y.first * WIDTH;
    const uint8_t *lower = plane + (size_t)y.second * WIDTH;
    unsigned a = x.first * step, b = x.second * step;
    float top = upper[a] + (upper[b]-upper[a]) * x.fraction;
    float bottom = lower[a] + (lower[b]-lower[a]) * x.fraction;
    return top + (bottom-top) * y.fraction;
}

static uint8_t pixel_byte(float value)
{
    if (value <= 0) return 0;
    if (value >= 255) return 255;
    return (uint8_t)(value + 0.5f);
}

static int write_rgb(FILE *out, const uint8_t *nv12)
{
    const uint8_t *uv = nv12 + WIDTH*HEIGHT;
    struct sample y_x[SIDE], uv_x[SIDE];
    for (unsigned x = 0; x < SIDE; ++x) {
        y_x[x] = sample_at(x, WIDTH, SIDE);
        uv_x[x] = sample_at(x, WIDTH/2, SIDE);
    }
    uint8_t row[SIDE*3];
    for (unsigned y = 0; y < SIDE; ++y) {
        if (y < PAD_Y || y >= PAD_Y + CONTENT_H) {
            memset(row, 114, sizeof(row));
        } else {
            struct sample y_row = sample_at(y-PAD_Y, HEIGHT, CONTENT_H);
            struct sample uv_row = sample_at(y-PAD_Y, HEIGHT/2, CONTENT_H);
            for (unsigned x = 0; x < SIDE; ++x) {
                float luma = interpolate(nv12, 1, y_x[x], y_row);
                float u = interpolate(uv, 2, uv_x[x], uv_row) - 128.0f;
                float v = interpolate(uv+1, 2, uv_x[x], uv_row) - 128.0f;
                row[3*x]   = pixel_byte(luma + 1.402f*v);
                row[3*x+1] = pixel_byte(luma - 0.344136f*u - 0.714136f*v);
                row[3*x+2] = pixel_byte(luma + 1.772f*u);
            }
        }
        if (fwrite(row, 1, sizeof(row), out) != sizeof(row)) return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s SHOT.nv12 FRAME.rgb\n", argv[0]);
        return EXIT_FAILURE;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror(argv[1]); return EXIT_FAILURE; }
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return EXIT_FAILURE; }
    if (!S_ISREG(st.st_mode) || st.st_size != NV12_BYTES) {
        fprintf(stderr, "expected a packed 2304x1296 NV12 file (%d bytes)\n", NV12_BYTES);
        close(fd);
        return EXIT_FAILURE;
    }
    void *pixels = mmap(NULL, NV12_BYTES, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (pixels == MAP_FAILED) { perror("mmap"); return EXIT_FAILURE; }
    /* The caller gives us a new path; never truncate the input or an old result. */
    int out_fd = open(argv[2], O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (out_fd < 0) { perror(argv[2]); munmap(pixels, NV12_BYTES); return EXIT_FAILURE; }
    FILE *out = fdopen(out_fd, "wb");
    if (!out) {
        perror("fdopen");
        close(out_fd);
        unlink(argv[2]);
        munmap(pixels, NV12_BYTES);
        return EXIT_FAILURE;
    }
    int status = write_rgb(out, pixels);
    if (fclose(out) != 0) status = -1;
    munmap(pixels, NV12_BYTES);
    if (status != 0) {
        perror("write RGB");
        unlink(argv[2]);
        return EXIT_FAILURE;
    }
    puts("PREPROCESS rgb=640x640 content=640x360 pad_y=140 bytes=1228800");
    return EXIT_SUCCESS;
}
