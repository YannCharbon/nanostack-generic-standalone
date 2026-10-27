/* SPDX-License-Identifier: Apache-2.0
 *
 * Simulation port: stdout and stderr become streams that write whole lines
 * to fds 1 and 2, each prefixed with the virtual time as wall-clock seconds
 * ("%llu.%06llu ", from NSIM_EPOCH_S). The run scripts need no timestamping
 * wrapper, and the logs of two runs with the same seed are identical.
 *
 * The streams are installed before main() (constructor), so that lines
 * printed before ns_port_sim_attach() get virtual time 0. A line is stamped
 * when its newline is written.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "ns_port_sim.h"
#include "sim_internal.h"

struct line_stream {
    int fd;
    char *buf;
    size_t len;
    size_t cap;
};

static struct line_stream streams[2] = { { .fd = 1 }, { .fd = 2 } };

static void write_all(int fd, const char *buf, size_t len)
{
    ssize_t ret;

    while (len) {
        ret = write(fd, buf, len);
        if (ret < 0 && errno == EINTR)
            continue;
        if (ret <= 0)
            return; // nowhere to report it
        buf += ret;
        len -= ret;
    }
}

static void line_reserve(struct line_stream *s, size_t len)
{
    if (s->cap >= len)
        return;
    s->cap = len < 256 ? 256 : len * 2;
    s->buf = realloc(s->buf, s->cap);
    if (!s->buf)
        abort();
}

// The buffer holds the prefix room, then the line.
#define PREFIX_MAX 32

static void line_emit(struct line_stream *s)
{
    uint64_t now = ns_port_sim_now_us();
    char prefix[PREFIX_MAX];
    int n;

    n = snprintf(prefix, sizeof(prefix), "%llu.%06llu ",
                 (unsigned long long)(NSIM_EPOCH_S + now / 1000000),
                 (unsigned long long)(now % 1000000));
    memcpy(s->buf + PREFIX_MAX - n, prefix, n);
    write_all(s->fd, s->buf + PREFIX_MAX - n, n + s->len);
    s->len = 0;
}

static ssize_t line_write(void *cookie, const char *data, size_t size)
{
    struct line_stream *s = cookie;
    const char *nl;
    size_t len, done = 0;

    while (done < size) {
        nl = memchr(data + done, '\n', size - done);
        len = nl ? (size_t)(nl - (data + done)) + 1 : size - done;
        line_reserve(s, PREFIX_MAX + s->len + len);
        memcpy(s->buf + PREFIX_MAX + s->len, data + done, len);
        s->len += len;
        done += len;
        if (nl)
            line_emit(s);
    }
    return size;
}

// A last line without newline is written at exit, with one.
static void line_flush_partial(void)
{
    int i;

    fflush(stdout);
    fflush(stderr);
    for (i = 0; i < 2; i++) {
        if (!streams[i].len)
            continue;
        line_write(&streams[i], "\n", 1);
    }
}

static FILE *line_open(struct line_stream *s)
{
    cookie_io_functions_t io = { .write = line_write };
    FILE *f = fopencookie(s, "w", io);

    if (!f)
        abort();
    setvbuf(f, NULL, _IONBF, 0);
    return f;
}

void sim_stdio_install(void)
{
    static bool installed;

    if (installed)
        return;
    installed = true;
    fflush(stdout);
    fflush(stderr);
    stdout = line_open(&streams[0]);
    stderr = line_open(&streams[1]);
    atexit(line_flush_partial);
}

static void __attribute__((constructor)) sim_stdio_init(void)
{
    sim_stdio_install();
}
