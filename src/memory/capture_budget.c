/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Optional byte ceiling for negotiated decoder CAPTURE queue storage. */
#include <errno.h>
#include <stdlib.h>
#include "va_v4l2.h"
static int budget_limit(uint64_t *limit)
{
    const char *value = getenv("VAV4L2_CAPTURE_BUDGET_MIB"); *limit = 0;
    if (!value) return 0;
    if (!*value) { errno = EINVAL; return -1; }
    uint64_t mib = 0;
    for (const char *p = value; *p; p++) {
        if (*p < '0' || *p > '9' || mib > (UINT64_MAX / 1048576 - (unsigned)(*p - '0')) / 10) { errno = EINVAL; return -1; }
        mib = mib * 10 + (*p - '0');
    }
    *limit = mib * 1048576; return 0;
}
int vav4l2_capture_budget_bytes(uint64_t bytes)
{
    uint64_t limit;
    if (budget_limit(&limit) < 0) return -1;
    if (limit && bytes > limit) { errno = ENOMEM; return -1; }
    return 0;
}
int vav4l2_capture_budget(struct vav4l2_context *ctx, const struct v4l2_pix_format_mplane *fmt, unsigned count, int allocated)
{
    uint64_t limit, bytes = 0;
    if (budget_limit(&limit) < 0) return -1;
    if (!limit) return 0;
    if (!fmt || !count || count > VAV4L2_MAX_CAPTURE_BUFS || !fmt->num_planes || fmt->num_planes > VIDEO_MAX_PLANES) { errno = EINVAL; return -1; }
    for (unsigned p = 0; p < fmt->num_planes; p++) {
        if (!fmt->plane_fmt[p].sizeimage) { errno = EINVAL; return -1; }
        bytes += (uint64_t)fmt->plane_fmt[p].sizeimage * count;
    }
    if (bytes > limit) { errno = ENOMEM; return -1; }
    if (!allocated) return 0;
    if (!ctx) { errno = EINVAL; return -1; }
    bytes = 0;
    for (unsigned i = 0; i < count; i++) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {{0}};
        struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP, .index = i, .length = fmt->num_planes, .m.planes = planes};
        if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_QUERYBUF, &b) < 0) return -1;
        if (b.length != fmt->num_planes) { errno = EPROTO; return -1; }
        for (unsigned p = 0; p < b.length; p++) {
            if (planes[p].length < fmt->plane_fmt[p].sizeimage) { errno = EPROTO; return -1; }
            bytes += planes[p].length;
        }
        if (bytes > limit) { errno = ENOMEM; return -1; }
    }
    return 0;
}
