/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Test-only pre-sync export and delayed reader; all calls use public libva. */
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <drm_fourcc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
struct held { VADRMPRIMESurfaceDescriptor d; uint8_t *pixels; size_t bytes; VASurfaceID surface; int destroyed; };
static struct held held[32];
static struct { VAContextID context; VASurfaceID surface; int active; } targets[64];
static unsigned count, exports;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int enabled(void) { return getenv("VAV4L2_EXPORT_TRACE_DIR") != NULL; }
static int record(const char *stage, unsigned value)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/export-%ld.tsv", getenv("VAV4L2_EXPORT_TRACE_DIR"), (long)getpid()) >= (int)sizeof(path)) return -1;
    FILE *f = fopen(path, "a"); if (!f) return -1;
    int bad = fprintf(f, "%s\t%u\n", stage, value) < 0; if (fclose(f)) bad = 1; return bad ? -1 : 0;
}
static void release(struct held *h)
{
    for (unsigned i = 0; i < h->d.num_objects && i < 4; i++) if (h->d.objects[i].fd >= 0) close(h->d.objects[i].fd);
    free(h->pixels); memset(h, 0, sizeof(*h));
}
static int read_export(const struct held *h, uint8_t *out)
{
    const VADRMPRIMESurfaceDescriptor *d = &h->d; void *maps[4] = {0}; unsigned synced = 0; int ok = 0;
    for (unsigned i = 0; i < d->num_objects; i++) {
        struct pollfd p = {.fd = d->objects[i].fd, .events = POLLIN};
        struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
        if (poll(&p, 1, 1000) != 1 || !(p.revents & POLLIN) || ioctl(p.fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) goto done;
        synced++;
        maps[i] = mmap(NULL, d->objects[i].size, PROT_READ, MAP_SHARED, p.fd, 0);
        if (maps[i] == MAP_FAILED) { maps[i] = NULL; goto done; }
    }
    unsigned row = d->width * (d->fourcc == VA_FOURCC_P010 ? 2 : 1); size_t offset = 0;
    for (unsigned p = 0; p < 2; p++) {
        unsigned object = d->layers[0].object_index[p], rows = p ? d->height / 2 : d->height;
        uint64_t end = d->layers[0].offset[p] + (uint64_t)d->layers[0].pitch[p] * (rows - 1) + row;
        if (object >= d->num_objects || d->layers[0].pitch[p] < row || end > d->objects[object].size) goto done;
        for (unsigned y = 0; y < rows; y++) {
            memcpy(out + offset, (const uint8_t *)maps[object] + d->layers[0].offset[p] + (size_t)y * d->layers[0].pitch[p], row); offset += row;
        }
    }
    ok = offset == h->bytes;
done:
    for (unsigned i = 0; i < d->num_objects; i++) if (maps[i] && munmap(maps[i], d->objects[i].size)) ok = 0;
    for (unsigned i = 0; i < synced; i++) {
        struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
        if (ioctl(d->objects[i].fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) ok = 0;
    }
    return ok ? 0 : -1;
}
static int check_held(void)
{
    for (unsigned i = 0; i < count; i++) {
        uint8_t *pixels = malloc(held[i].bytes); if (!pixels) return -1;
        int ok = !read_export(held + i, pixels) && !memcmp(pixels, held[i].pixels, held[i].bytes); free(pixels);
        if (!ok) return -1;
    }
    return 0;
}
static int retain(VADisplay dpy, VASurfaceID surface)
{
    if (count == 32) return -1;
    struct held h = {.surface = surface}; VAImage image; int have_image = 0, mapped = 0, ok = 0; void *source = NULL;
    for (unsigned i = 0; i < 4; i++) h.d.objects[i].fd = -1;
    if (vaExportSurfaceHandle(dpy, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &h.d)) goto done;
    if (!h.d.num_objects || h.d.num_objects > 4 || h.d.num_layers != 1 || h.d.layers[0].num_planes != 2 ||
        (h.d.fourcc != VA_FOURCC_NV12 && h.d.fourcc != VA_FOURCC_P010) || !h.d.width || !h.d.height || h.d.width > 4096 || h.d.height > 4096 || (h.d.width & 1) || (h.d.height & 1) ||
        h.d.layers[0].drm_format != (h.d.fourcc == VA_FOURCC_P010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12)) goto done;
    for (unsigned i = 0; i < h.d.num_objects; i++) if (h.d.objects[i].fd < 0 || !h.d.objects[i].size || h.d.objects[i].size > 268435456 || h.d.objects[i].drm_format_modifier != DRM_FORMAT_MOD_LINEAR) goto done;
    if (vaSyncSurface(dpy, surface) || check_held()) goto done;
    VAImageFormat format = {.fourcc = h.d.fourcc, .byte_order = VA_LSB_FIRST, .bits_per_pixel = h.d.fourcc == VA_FOURCC_P010 ? 24 : 12}; format.depth = format.bits_per_pixel;
    if (vaCreateImage(dpy, &format, h.d.width, h.d.height, &image)) goto done;
    have_image = 1;
    if (vaGetImage(dpy, surface, 0, 0, h.d.width, h.d.height, image.image_id) || vaMapBuffer(dpy, image.buf, &source)) goto done;
    mapped = 1;
    unsigned row = h.d.width * (h.d.fourcc == VA_FOURCC_P010 ? 2 : 1); h.bytes = (size_t)row * h.d.height * 3 / 2;
    h.pixels = malloc(h.bytes); if (!h.pixels || read_export(&h, h.pixels) < 0 || image.num_planes != 2) goto done;
    size_t offset = 0;
    for (unsigned p = 0; p < 2; p++) {
        unsigned rows = p ? h.d.height / 2 : h.d.height;
        if (image.pitches[p] < row || image.offsets[p] + (uint64_t)image.pitches[p] * (rows - 1) + row > image.data_size) goto done;
        for (unsigned y = 0; y < rows; y++) {
            if (memcmp(h.pixels + offset, (const uint8_t *)source + image.offsets[p] + (size_t)y * image.pitches[p], row)) goto done;
            offset += row;
        }
    }
    ok = !record("HOLD", exports + 1);
done:
    if (mapped && vaUnmapBuffer(dpy, image.buf)) ok = 0;
    if (have_image && vaDestroyImage(dpy, image.image_id)) ok = 0;
    if (ok) { held[count++] = h; exports++; } else release(&h);
    return ok ? 0 : -1;
}
VAStatus vaBeginPicture(VADisplay dpy, VAContextID context, VASurfaceID surface)
{
    VAStatus (*real)(VADisplay, VAContextID, VASurfaceID) = dlsym(RTLD_NEXT, "vaBeginPicture");
    if (!real) return VA_STATUS_ERROR_OPERATION_FAILED;
    if (enabled()) {
        pthread_mutex_lock(&lock); int failed = check_held(); pthread_mutex_unlock(&lock);
        if (failed) return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    VAStatus status = real(dpy, context, surface); if (status || !enabled()) return status;
    pthread_mutex_lock(&lock); unsigned slot = 64;
    for (unsigned i = 0; i < 64; i++) if (targets[i].active && targets[i].context == context) { slot = i; break; }
    if (slot == 64) for (unsigned i = 0; i < 64; i++) if (!targets[i].active) { slot = i; break; }
    for (unsigned i = 0; i < count;) {
        if (held[i].surface != surface || held[i].destroyed) { i++; continue; }
        /* Only successful reuse of the same live surface permits overwrite.
         * A recycled VA id does not end the old exported object's lifetime. */
        release(held + i); held[i] = held[--count];
    }
    if (slot < 64) { targets[slot].context = context; targets[slot].surface = surface; targets[slot].active = 1; } else status = VA_STATUS_ERROR_OPERATION_FAILED;
    pthread_mutex_unlock(&lock); return status;
}
VAStatus vaEndPicture(VADisplay dpy, VAContextID context)
{
    VAStatus (*real)(VADisplay, VAContextID) = dlsym(RTLD_NEXT, "vaEndPicture");
    if (!real) return VA_STATUS_ERROR_OPERATION_FAILED;
    VAStatus status = real(dpy, context); if (status || !enabled()) return status;
    pthread_mutex_lock(&lock); unsigned slot = 64;
    for (unsigned i = 0; i < 64; i++) if (targets[i].active && targets[i].context == context) { slot = i; break; }
    if (slot == 64 || retain(dpy, targets[slot].surface) < 0) status = VA_STATUS_ERROR_OPERATION_FAILED;
    pthread_mutex_unlock(&lock); return status;
}
VAStatus vaDestroyContext(VADisplay dpy, VAContextID context)
{
    VAStatus (*real)(VADisplay, VAContextID) = dlsym(RTLD_NEXT, "vaDestroyContext");
    if (!real) return VA_STATUS_ERROR_OPERATION_FAILED;
    VAStatus status = real(dpy, context); if (status || !enabled()) return status;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < 64; i++) if (targets[i].context == context) targets[i].active = 0;
    if (check_held()) status = VA_STATUS_ERROR_OPERATION_FAILED;
    pthread_mutex_unlock(&lock); return status;
}
VAStatus vaDestroySurfaces(VADisplay dpy, VASurfaceID *surfaces, int n)
{
    VAStatus (*real)(VADisplay, VASurfaceID *, int) = dlsym(RTLD_NEXT, "vaDestroySurfaces");
    if (!real) return VA_STATUS_ERROR_OPERATION_FAILED;
    VAStatus status = real(dpy, surfaces, n); if (status || !enabled()) return status;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < count; i++) for (int j = 0; j < n; j++) if (held[i].surface == surfaces[j]) held[i].destroyed = 1;
    if (check_held()) status = VA_STATUS_ERROR_OPERATION_FAILED;
    pthread_mutex_unlock(&lock); return status;
}
VAStatus vaTerminate(VADisplay dpy)
{
    VAStatus (*real)(VADisplay) = dlsym(RTLD_NEXT, "vaTerminate");
    if (!real) return VA_STATUS_ERROR_OPERATION_FAILED;
    int failed = 0;
    if (enabled()) {
        pthread_mutex_lock(&lock); failed = !exports || check_held() || record("COMPLETE", exports);
        for (unsigned i = 0; i < count; i++) release(held + i);
        count = exports = 0; pthread_mutex_unlock(&lock);
    }
    VAStatus status = real(dpy); return failed ? VA_STATUS_ERROR_OPERATION_FAILED : status;
}
