/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "va_v4l2.h"
static unsigned queries, padding, short_plane;
static int hook(int fd, unsigned long request, void *arg)
{
    assert(fd == 42 && request == VIDIOC_QUERYBUF); queries++;
    struct v4l2_buffer *b = arg;
    assert(b->memory == V4L2_MEMORY_MMAP && b->length == 2);
    b->m.planes[0].length = 32768 + padding; b->m.planes[1].length = short_plane ? 32767 : 32768; return 0;
}
int main(void)
{
    struct vav4l2_context ctx = {.v4l2_fd = 42}; struct v4l2_pix_format_mplane fmt = {.num_planes = 2};
    fmt.plane_fmt[0].sizeimage = fmt.plane_fmt[1].sizeimage = 32768;
    vav4l2_set_ioctl_hook(hook); unsetenv("VAV4L2_CAPTURE_BUDGET_MIB");
    assert(!vav4l2_capture_budget(&ctx, &fmt, 16, 1) && !queries);
    setenv("VAV4L2_CAPTURE_BUDGET_MIB", "1", 1);
    assert(!vav4l2_capture_budget(&ctx, &fmt, 16, 0) && !queries);
    assert(vav4l2_capture_budget(&ctx, &fmt, 17, 0) < 0 && errno == ENOMEM && !queries);
    assert(!vav4l2_capture_budget(&ctx, &fmt, 16, 1) && queries == 16);
    padding = 4096; queries = 0;
    assert(vav4l2_capture_budget(&ctx, &fmt, 16, 1) < 0 && errno == ENOMEM && queries == 16);
    padding = 0; short_plane = 1;
    assert(vav4l2_capture_budget(&ctx, &fmt, 16, 1) < 0 && errno == EPROTO);
    assert(!vav4l2_capture_budget_bytes(1048576) && vav4l2_capture_budget_bytes(1048577) < 0 && errno == ENOMEM);
    const char *invalid[] = {"", "-1", "+1", "1x", " 1", "17592186044416", "99999999999999999999999999"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        setenv("VAV4L2_CAPTURE_BUDGET_MIB", invalid[i], 1);
        assert(vav4l2_capture_budget_bytes(1) < 0 && errno == EINVAL);
    }
    setenv("VAV4L2_CAPTURE_BUDGET_MIB", "0", 1); queries = 0;
    assert(!vav4l2_capture_budget_bytes(UINT64_MAX) && !vav4l2_capture_budget(&ctx, &fmt, 16, 1) && !queries);
    unsetenv("VAV4L2_CAPTURE_BUDGET_MIB"); vav4l2_set_ioctl_hook(NULL);
    puts("CAPTURE negotiated bytes, actual plane lengths, boundary and strict budget policy: PASS"); return 0;
}
