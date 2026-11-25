#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "This example expects a little-endian target"
#endif

struct gpio_event {
    uint64_t seq;
    uint64_t ts_ns;
    uint32_t value;
    uint32_t reserved;
};

_Static_assert(sizeof(struct gpio_event) == 24, "Wrong event size");

int main(void)
{
    struct gpio_event event;
    int fd = open("/dev/hello-gpio-events", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    ssize_t bytes = read(fd, &event, sizeof(event));
    if (bytes != -1 || errno != EAGAIN) {
        fprintf(stderr, "Expected EAGAIN, read returned %zd\n", bytes);
        return 1;
    }
    puts("empty read: EAGAIN");

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ready = poll(&pfd, 1, 300);
    if (ready < 0) {
        perror("poll");
        return 1;
    }
    if (ready != 0) {
        fprintf(stderr, "Expected timeout, revents=0x%x\n",
                (unsigned int)pfd.revents);
        return 1;
    }
    puts("empty poll: timeout");
    puts("POLL_READY");
    fflush(stdout);

    uint64_t previous_ts = 0;
    for (unsigned int seq = 1; seq <= 2; ++seq) {
        ready = poll(&pfd, 1, 5000);
        if (ready < 0) {
            perror("poll");
            return 1;
        }
        if (ready == 0 || !(pfd.revents & POLLIN)) {
            fprintf(stderr, "No readable event, revents=0x%x\n",
                    (unsigned int)pfd.revents);
            return 1;
        }

        bytes = read(fd, &event, sizeof(event));
        if (bytes < 0) {
            perror("read");
            return 1;
        }
        if (bytes != (ssize_t)sizeof(event)) {
            fprintf(stderr, "Unexpected read size: %zd\n", bytes);
            return 1;
        }

        printf("seq=%" PRIu64 " ts_ns=%" PRIu64 " input=%" PRIu32 "\n",
               event.seq, event.ts_ns, event.value);

        if (event.seq != seq || event.value != 2 - seq ||
            event.ts_ns <= previous_ts || event.reserved != 0) {
            fputs("Unexpected event fields\n", stderr);
            return 1;
        }
        previous_ts = event.ts_ns;
    }

    close(fd);
    puts("POLL_TEST_OK");
    return 0;
}
