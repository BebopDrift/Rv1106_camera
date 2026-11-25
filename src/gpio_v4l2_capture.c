#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <linux/videodev2.h>

enum { WIDTH = 2304, HEIGHT = 1296, BUFFER_COUNT = 3, FRAME_COUNT = 30 };

struct mapping {
    unsigned char *data;
    size_t length;
};

static int save_nv12_frame(const struct mapping *mapped,
                           const struct v4l2_plane *plane,
                           unsigned int stride)
{
    const size_t frame_bytes = (size_t)WIDTH * HEIGHT * 3 / 2;
    const size_t offset = plane->data_offset;
    const size_t used = plane->bytesused;

    if (stride != WIDTH || offset > used || used > mapped->length ||
        used - offset != frame_bytes) {
        fprintf(stderr, "Unexpected NV12 layout: stride=%u offset=%zu used=%zu mapped=%zu\n",
                stride, offset, used, mapped->length);
        return -1;
    }

    const char *path = "/tmp/gpio-shot.nv12";
    char temporary[] = "/tmp/gpio-shot.nv12.XXXXXX";
    int out_fd = mkstemp(temporary);
    if (out_fd < 0) {
        perror("mkstemp");
        return -1;
    }

    FILE *out = fdopen(out_fd, "wb");
    if (!out) {
        perror("fdopen");
        close(out_fd);
        unlink(temporary);
        return -1;
    }

    size_t written = fwrite(mapped->data + offset, 1, frame_bytes, out);
    int closed = fclose(out);
    if (written != frame_bytes || closed != 0) {
        fprintf(stderr, "NV12 write failed: wrote %zu/%zu bytes\n",
                written, frame_bytes);
        unlink(temporary);
        return -1;
    }

    if (rename(temporary, path) != 0) {
        perror("rename snapshot");
        unlink(temporary);
        return -1;
    }

    printf("SAVED path=%s bytes=%zu\n", path, frame_bytes);
    return 0;
}

static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

static struct v4l2_buffer capture_buffer(struct v4l2_plane *plane)
{
    return (struct v4l2_buffer) {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
        .length = 1,
        .m.planes = plane,
    };
}

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "The GPIO event format is little-endian"
#endif

struct gpio_event {
    uint64_t seq;
    uint64_t ts_ns;
    uint32_t value;
    uint32_t reserved;
};

_Static_assert(sizeof(struct gpio_event) == 24, "Wrong GPIO event size");

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;

    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

int main(void)
{
    const char *device = "/dev/video11";
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct mapping buffers[BUFFER_COUNT] = {0};
    int streaming = 0;
    int status = 1;
		int gpio_fd = -1;

    int fd = open(device, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror(device);
        return 1;
    }

    struct v4l2_capability cap = {0};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        goto cleanup;
    }
    unsigned int caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS
                        ? cap.device_caps : cap.capabilities;
    unsigned int required = V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;
    if ((caps & required) != required) {
        fprintf(stderr, "Capture multiplanar streaming required\n");
        goto cleanup;
    }

    struct v4l2_format fmt = { .type = type };
    fmt.fmt.pix_mp.width = WIDTH;
    fmt.fmt.pix_mp.height = HEIGHT;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        goto cleanup;
    }
    if (fmt.fmt.pix_mp.width != WIDTH || fmt.fmt.pix_mp.height != HEIGHT ||
        fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
        fmt.fmt.pix_mp.num_planes != 1) {
        fprintf(stderr, "Expected %dx%d NV12 with one memory plane\n", WIDTH, HEIGHT);
        goto cleanup;
    }
    printf("format=NV12 %ux%u stride=%u sizeimage=%u\n",
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
           fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
           fmt.fmt.pix_mp.plane_fmt[0].sizeimage);

    struct v4l2_requestbuffers req = {
        .count = BUFFER_COUNT,
        .type = type,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        goto cleanup;
    }
    if (req.count != BUFFER_COUNT) {
        fprintf(stderr, "Expected %d buffers, got %u\n", BUFFER_COUNT, req.count);
        goto cleanup;
    }

    for (unsigned int i = 0; i < req.count; ++i) {
        struct v4l2_plane plane = {0};
        struct v4l2_buffer buf = capture_buffer(&plane);
        buf.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            goto cleanup;
        }
        buffers[i].data = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, plane.m.mem_offset);
        if (buffers[i].data == MAP_FAILED) {
            perror("mmap");
            goto cleanup;
        }
        buffers[i].length = plane.length;
        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            goto cleanup;
        }
    }

    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        goto cleanup;
    }
    streaming = 1;

    gpio_fd = open("/dev/hello-gpio-events", O_RDONLY | O_NONBLOCK);
    if (gpio_fd < 0) {
        perror("open GPIO events");
        goto cleanup;
    }

    struct pollfd fds[] = {
        { .fd = gpio_fd, .events = POLLIN },
        { .fd = fd,      .events = POLLIN },
    };
    struct gpio_event trigger = {0};
    uint64_t armed_ns = 0;
    uint64_t started_ns = monotonic_ns();
    if (!started_ns) {
        perror("clock_gettime");
        goto cleanup;
    }

    for (;;) {
        uint64_t now_ns = monotonic_ns();
        if (!now_ns) {
            perror("clock_gettime");
            goto cleanup;
        }
        if (now_ns - started_ns >= 10000000000ULL) {
            fprintf(stderr, "Timed out: armed=%d trigger_seq=%" PRIu64 "\n",
                    armed_ns != 0, trigger.seq);
            goto cleanup;
        }

        int ready = poll(fds, 2, 1000);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            goto cleanup;
        }
        if (ready == 0)
            continue;

        if ((fds[0].revents | fds[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "poll error: gpio=%#x video=%#x\n",
                    (unsigned int)fds[0].revents, (unsigned int)fds[1].revents);
            goto cleanup;
        }

        if (fds[0].revents & POLLIN) {
            struct gpio_event event;
            ssize_t bytes = read(gpio_fd, &event, sizeof(event));

            if (bytes < 0 && errno != EAGAIN && errno != EINTR) {
                perror("read GPIO event");
                goto cleanup;
            }
            if (bytes >= 0 && bytes != (ssize_t)sizeof(event)) {
                fprintf(stderr, "GPIO read returned %zd bytes\n", bytes);
                goto cleanup;
            }
            if (bytes == (ssize_t)sizeof(event) && !trigger.seq &&
                armed_ns && event.value == 1 && event.ts_ns >= armed_ns) {
                trigger = event;
                printf("TRIGGER gpio_seq=%" PRIu64 " gpio_ts_ns=%" PRIu64 "\n",
                       trigger.seq, trigger.ts_ns);
            }
        }

        if (!(fds[1].revents & POLLIN))
            continue;

        struct v4l2_plane plane = {0};
        struct v4l2_buffer buf = capture_buffer(&plane);
        if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN)
                continue;
            perror("VIDIOC_DQBUF");
            goto cleanup;
        }
        if (buf.index >= req.count || (buf.flags & V4L2_BUF_FLAG_ERROR) ||
            plane.bytesused > buffers[buf.index].length ||
            plane.data_offset >= plane.bytesused) {
            fprintf(stderr, "Invalid captured buffer, seq=%u\n", buf.sequence);
            goto cleanup;
        }

        if ((buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) !=
            V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC ||
            (buf.flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK) !=
            V4L2_BUF_FLAG_TSTAMP_SRC_EOF) {
            fprintf(stderr, "Expected monotonic EOF timestamp, flags=%#x\n", buf.flags);
            goto cleanup;
        }

        uint64_t frame_ts_ns = (uint64_t)buf.timestamp.tv_sec * 1000000000ULL
                             + (uint64_t)buf.timestamp.tv_usec * 1000ULL;
        unsigned int frame_seq = buf.sequence;

				const int frame_matched =
						armed_ns != 0 &&
						trigger.seq != 0 &&
						(buf.flags & V4L2_BUF_FLAG_ERROR) == 0 &&
						(buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
								V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC &&
						(buf.flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK) ==
								V4L2_BUF_FLAG_TSTAMP_SRC_EOF &&
						frame_ts_ns >= trigger.ts_ns;

				if (frame_matched &&
						save_nv12_frame(&buffers[buf.index], &plane,
														fmt.fmt.pix_mp.plane_fmt[0].bytesperline) < 0) {
						goto cleanup;
				}

				if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            goto cleanup;
        }

        if (!armed_ns) {
            armed_ns = monotonic_ns();
            if (!armed_ns) {
                perror("clock_gettime");
                goto cleanup;
            }
            puts("CAPTURE_READY");
            fflush(stdout);
        }

        if (trigger.seq && frame_ts_ns >= trigger.ts_ns) {
            if (!frame_matched)
                continue;

            printf("MATCH gpio_seq=%" PRIu64 " frame_seq=%u frame_ts_ns=%" PRIu64
                   " delta_ns=%" PRIu64 "\n",
                   trigger.seq, frame_seq, frame_ts_ns,
                   frame_ts_ns - trigger.ts_ns);
            status = 0;
            break;
        }
    }

cleanup:
    if (streaming && xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("VIDIOC_STREAMOFF");
        status = 1;
    }
    for (unsigned int i = 0; i < BUFFER_COUNT; ++i) {
        if (buffers[i].length != 0)
            munmap(buffers[i].data, buffers[i].length);
    }

    if (gpio_fd >= 0)
        close(gpio_fd);
    close(fd);
    if (status == 0)
        puts("GPIO_V4L2_TEST_OK");
    return status;
}
