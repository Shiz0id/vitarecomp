/* reloc.c — walking SCE relocation entries. See reloc.h. */

#include "reloc.h"

#include <string.h>

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t bits(uint32_t w, int lo, int n) {
    return (w >> lo) & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u));
}

/* Segment index -> link-time base. 0xF means "no symbol segment": S is 0. */
static int seg_base(const vm_image *img, uint32_t idx, uint32_t *out) {
    if (idx == 0xF) { *out = 0; return 1; }
    if (idx >= img->phdr_count) return 0;
    *out = img->phdr_vaddr[idx];
    return 1;
}

/* The word stored at a link-time address, from whichever LOAD segment holds
 * it in the file. Formats 6-9 carry no addend: the target is what the linker
 * already wrote there. */
static int stored_word(const vm_image *img, uint32_t va, uint32_t *out) {
    if (va >= img->seg_vaddr && va - img->seg_vaddr + 4 <= img->seg_len) {
        *out = le32(img->data + img->seg_file + (va - img->seg_vaddr));
        return 1;
    }
    for (uint32_t i = 0; i < img->data_seg_count; i++) {
        const vm_seg *s = &img->data_segs[i];
        if (va >= s->vaddr && va - s->vaddr + 4 <= s->len) {
            *out = le32(img->data + s->file + (va - s->vaddr));
            return 1;
        }
    }
    return 0;
}

static int walk_one(const vm_image *img, const uint8_t *p, const uint8_t *end,
                    vr_fn fn, void *ctx) {
    /* The state later entries are deltas against. */
    uint32_t g_addr = 0, g_offset = 0, g_patchseg = 0;
    uint32_t g_saddr = 0, g_addend = 0, g_type = 0, g_type2 = 0;
    int n = 0;

    while (p + 4 <= end) {
        uint32_t w0 = le32(p);
        uint32_t fmt = w0 & 0xF;
        uint32_t size = fmt == 0 ? 12 : (fmt <= 3 ? 8 : 4);
        if (fmt > 9 || p + size > end) return -1;
        uint32_t w1 = size >= 8 ? le32(p + 4) : 0;
        uint32_t w2 = size >= 12 ? le32(p + 8) : 0;

        switch (fmt) {
            case 0: case 1: {
                uint32_t sseg = bits(w0, 4, 4), code = bits(w0, 8, 8);
                uint32_t pseg = bits(w0, 16, 4), off, add, code2 = 0, dist2 = 0;
                if (fmt == 0) {
                    code2 = bits(w0, 20, 8); dist2 = bits(w0, 28, 4);
                    add = w1; off = w2;
                } else {
                    off = bits(w0, 20, 12) | (bits(w1, 0, 10) << 12);
                    add = bits(w1, 10, 22);
                }
                uint32_t s, pb;
                if (!seg_base(img, sseg, &s) || !seg_base(img, pseg, &pb)) return -1;
                fn(ctx, pb + off, code, s + add);
                if (code2) fn(ctx, pb + off + dist2 * 2, code2, s + add);
                g_addr = pb; g_offset = off; g_patchseg = pseg;
                g_saddr = s; g_addend = add; g_type = code; g_type2 = code2;
                break;
            }
            case 2: {
                uint32_t s;
                if (!seg_base(img, bits(w0, 4, 4), &s)) return -1;
                g_offset += bits(w0, 16, 16);
                g_saddr = s; g_addend = w1; g_type = bits(w0, 8, 8); g_type2 = 0;
                fn(ctx, g_addr + g_offset, g_type, s + g_addend);
                break;
            }
            case 3: {                              /* a MOVW/MOVT pair */
                uint32_t s;
                if (!seg_base(img, bits(w0, 4, 4), &s)) return -1;
                int thumb = (int)bits(w0, 8, 1);
                uint32_t dist2 = bits(w0, 27, 5);
                g_type  = thumb ? VR_THM_MOVW : VR_MOVW;
                g_type2 = thumb ? VR_THM_MOVT : VR_MOVT;
                g_offset += bits(w0, 9, 18);
                g_saddr = s; g_addend = bits(w1, 0, 22);
                fn(ctx, g_addr + g_offset, g_type, s + g_addend);
                fn(ctx, g_addr + g_offset + dist2, g_type2, s + g_addend);
                break;
            }
            case 4: {
                uint32_t dist2 = bits(w0, 27, 5);
                g_offset += bits(w0, 4, 23);
                fn(ctx, g_addr + g_offset, g_type, g_saddr + g_addend);
                fn(ctx, g_addr + g_offset + dist2, g_type2, g_saddr + g_addend);
                break;
            }
            case 5: {
                uint32_t t = g_saddr + g_addend;
                g_offset += bits(w0, 4, 9);
                fn(ctx, g_addr + g_offset, g_type, t);
                fn(ctx, g_addr + g_offset + bits(w0, 13, 5), g_type2, t);
                g_offset += bits(w0, 18, 9);
                fn(ctx, g_addr + g_offset, g_type, t);
                fn(ctx, g_addr + g_offset + bits(w0, 27, 5), g_type2, t);
                break;
            }
            case 6: case 7: case 8: case 9: {
                /* ABS32 against whatever segment the stored value lies in,
                 * with the offset deltas packed into the entry. */
                uint32_t pb;
                if (!seg_base(img, g_patchseg, &pb)) return -1;
                uint32_t width = fmt == 6 ? 28 : fmt == 7 ? 7 : fmt == 8 ? 4 : 2;
                uint32_t deltas = bits(w0, 4, 28);
                do {
                    uint32_t d = bits(deltas, 0, (int)width);
                    g_offset += fmt == 6 ? d : d * 4;
                    uint32_t v;
                    if (stored_word(img, pb + g_offset, &v))
                        fn(ctx, g_addr + g_offset, VR_ABS32, v);
                    if (fmt == 6) break;
                } while (deltas >>= width);
                g_type = VR_ABS32; g_type2 = 0;
                break;
            }
        }
        n++;
        p += size;
    }
    return n;
}

int vr_walk(const vm_image *img, vr_fn fn, void *ctx) {
    int total = 0;
    for (uint32_t i = 0; i < img->rela_count; i++) {
        const vm_seg *r = &img->relas[i];
        const uint8_t *p = img->data + r->file;
        int n = walk_one(img, p, p + r->len, fn, ctx);
        if (n < 0) return -1;
        total += n;
    }
    return total;
}
