/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* memfd storage plus a mocked DMA-BUF ioctl tests the reader, not hardware. */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include "decode_surface_interpose.c"
static unsigned starts, ends, fail_sync;
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    assert(fd >= 0 && request == DMA_BUF_IOCTL_SYNC);
    va_list ap; va_start(ap, request); struct dma_buf_sync *s = va_arg(ap, void *); va_end(ap);
    assert(s->flags & DMA_BUF_SYNC_READ);
    if (s->flags & DMA_BUF_SYNC_END) ends++; else { starts++; if (fail_sync == starts) { errno = EIO; return -1; } }
    return 0;
}
int main(void)
{
    for (unsigned ten = 0; ten < 2; ten++) {
        struct held h = {.d = {.fourcc = ten ? VA_FOURCC_P010 : VA_FOURCC_NV12, .width = 8, .height = 4, .num_objects = 2, .num_layers = 1}};
        uint8_t *maps[2]; h.d.layers[0].num_planes = 2;
        for (unsigned p = 0; p < 2; p++) {
            int fd = memfd_create("export-reader", MFD_CLOEXEC); assert(fd >= 0 && !ftruncate(fd, 4096));
            h.d.objects[p].fd = fd; h.d.objects[p].size = 4096; h.d.objects[p].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
            h.d.layers[0].object_index[p] = p; h.d.layers[0].offset[p] = 16; h.d.layers[0].pitch[p] = 32;
            maps[p] = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); assert(maps[p] != MAP_FAILED); memset(maps[p], 0xa5, 4096);
            for (unsigned y = 0; y < (p ? 2u : 4u); y++) for (unsigned x = 0; x < (ten ? 16u : 8u); x++) maps[p][16 + y * 32 + x] = p * 61 + y * 3 + x;
        }
        h.bytes = (ten ? 16 : 8) * 6; h.pixels = malloc(h.bytes); assert(h.pixels);
        starts = ends = fail_sync = 0;
        assert(!read_export(&h, h.pixels) && starts == 2 && ends == 2);
        for (unsigned i = 0, at = 0; i < 2; i++) for (unsigned y = 0; y < (i ? 2u : 4u); y++) for (unsigned x = 0; x < (ten ? 16u : 8u); x++) assert(h.pixels[at++] == i * 61 + y * 3 + x);
        held[0] = h; count = 1; assert(!check_held()); maps[1][16] ^= 1; assert(check_held() < 0); maps[1][16] ^= 1;
        uint8_t output[96]; starts = ends = 0; fail_sync = 2;
        assert(read_export(&h, output) < 0 && starts == 2 && ends == 1); fail_sync = 0;
        h.d.layers[0].object_index[1] = 2; starts = ends = 0;
        assert(read_export(&h, output) < 0 && starts == 2 && ends == 2);
        h.d.layers[0].object_index[1] = 1; h.d.layers[0].pitch[1] = 7;
        assert(read_export(&h, output) < 0); h.d.layers[0].pitch[1] = 32; h.d.objects[1].size = 20;
        assert(read_export(&h, output) < 0); h.d.objects[1].size = 4096;
        int fd = h.d.objects[0].fd;
        for (unsigned p = 0; p < 2; p++) assert(!munmap(maps[p], 4096));
        release(&h); assert(fcntl(fd, F_GETFD) < 0 && errno == EBADF); count = 0; memset(held, 0, sizeof(held));
    }
    puts("NV12/P010 delayed export reader, exact rows, corruption, sync and bounds: PASS"); return 0;
}
