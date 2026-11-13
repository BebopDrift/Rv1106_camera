#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>

enum { WIDTH = 2304, HEIGHT = 1296, BUFFER_COUNT = 3, FRAME_COUNT = 30 };

struct mapping {
    unsigned char *data;
    size_t length;
};

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

int main(void)
{
    const char *device = "/dev/video11";
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct mapping buffers[BUFFER_COUNT] = {0};
    int streaming = 0;
    int status = 1;

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

    for (unsigned int captured = 0; captured < FRAME_COUNT;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 2000);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0) {
            perror("poll");
            goto cleanup;
        }
        if (ready == 0 || !(pfd.revents & POLLIN)) {
            fprintf(stderr, "No readable frame (poll=%d, revents=%#x)\n",
                    ready, (unsigned int)pfd.revents);
            goto cleanup;
        }

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

        unsigned int y0 = buffers[buf.index].data[plane.data_offset];
        printf("seq=%u index=%u bytes=%u ts=%lld.%06ld flags=%#x y0=%u\n",
               buf.sequence, buf.index, plane.bytesused,
               (long long)buf.timestamp.tv_sec, (long)buf.timestamp.tv_usec,
               buf.flags, y0);

        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            goto cleanup;
        }
        ++captured;
    }
    status = 0;

cleanup:
    if (streaming && xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("VIDIOC_STREAMOFF");
        status = 1;
    }
    for (unsigned int i = 0; i < BUFFER_COUNT; ++i) {
        if (buffers[i].length != 0)
            munmap(buffers[i].data, buffers[i].length);
    }
    close(fd);
    if (status == 0)
        printf("captured=%d\n", FRAME_COUNT);
    return status;
}
