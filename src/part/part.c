/* MBR and GPT partition tables; see include/ofs/part.h. */
#include <string.h>
#include "ofs/part.h"

static ofs_u16 rd16(const ofs_u8 OFS_FAR *p) { return (ofs_u16)(p[0] | p[1] << 8); }
static ofs_u32 rd32(const ofs_u8 OFS_FAR *p) { return rd16(p) | (ofs_u32)rd16(p + 2) << 16; }
static ofs_u64 rd64(const ofs_u8 OFS_FAR *p) { return rd32(p) | (ofs_u64)rd32(p + 4) << 32; }

ofs_u32 ofs_crc32(ofs_u32 crc, const ofs_u8 OFS_FAR *p, unsigned len)
{
    unsigned k;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (k = 0; k < 8; ++k) crc = crc & 1 ? crc >> 1 ^ 0xEDB88320UL : crc >> 1;
    }
    return ~crc;
}

static int pow2(unsigned v, unsigned lo, unsigned hi)
{
    return v >= lo && v <= hi && !(v & (v - 1));
}

int ofs_part_is_vbr(const ofs_u8 OFS_FAR *b)
{
    static const char exfat[] = "EXFAT   ";
    ofs_u8 media = b[21];
    unsigned i;
    if (b[0] != 0xEB && b[0] != 0xE9) return 0;
    for (i = 0; i < 8 && b[3 + i] == (ofs_u8)exfat[i]; ++i) ;
    if (i == 8) return 1;
    return pow2(rd16(b + 11), 512, 4096) && pow2(b[13], 1, 128) && rd16(b + 14) &&
        b[16] >= 1 && b[16] <= 4 && (media == 0xF0 || media >= 0xF8) &&
        (rd16(b + 19) || rd32(b + 32));
}

struct scan {
    ofs_part_read_fn read;
    void *dev;
    ofs_u8 *buf;
    unsigned ssize;
    ofs_u64 sectors;
    ofs_part_fn fn;
    void *ctx;
    int n;
};

static int report(struct scan *s, struct ofs_part *p)
{
    int r;
    ++s->n;
    /* a callback's stop must not look like an error of ours */
    if (s->fn && (r = s->fn(s->ctx, p)) != 0) return r < 0 ? r : 1;
    return 0;
}

static int rd(struct scan *s, ofs_u64 lba)
{
    if (s->sectors && lba >= s->sectors) return OFS_PART_EBAD;
    return s->read(s->dev, lba, s->buf) ? OFS_PART_EIO : 0;
}

/* ---- GPT ---- */

struct gpt {
    ofs_u64 entries;
    ofs_u32 num, size, crc;
};

/* the header at lba: checked (signature, CRC, own LBA) */
static int gpt_header(struct scan *s, ofs_u64 lba, struct gpt *g, struct ofs_part_disk *d)
{
    static const ofs_u8 zero[4] = { 0, 0, 0, 0 };
    ofs_u8 *b = s->buf;
    ofs_u32 hsize, crc;
    int r;
    if ((r = rd(s, lba)) != 0) return r;
    if (memcmp((const char *)b, "EFI PART", 8)) return OFS_PART_EBAD;
    hsize = rd32(b + 12);
    if (hsize < 92 || hsize > s->ssize || rd64(b + 24) != lba) return OFS_PART_EBAD;
    crc = ofs_crc32(0, b, 16);
    crc = ofs_crc32(crc, zero, 4);
    crc = ofs_crc32(crc, b + 20, (unsigned)hsize - 20);
    if (crc != rd32(b + 16)) return OFS_PART_EBAD;
    g->entries = rd64(b + 72);
    g->num = rd32(b + 80);
    g->size = rd32(b + 84);
    g->crc = rd32(b + 88);
    if (g->size < 128 || g->size % 128 || g->num > 0x10000UL || g->entries < 2)
        return OFS_PART_EBAD;
    memcpy(d->guid, b + 56, 16);
    return 0;
}

/* the entry array: its CRC, then (report != 0) the partitions */
static int gpt_entries(struct scan *s, const struct gpt *g, int do_report)
{
    ofs_u32 bytes = g->num * g->size, off = 0, crc = 0, n, i;
    ofs_u64 lba = g->entries;
    struct ofs_part p;
    const ofs_u8 *e;
    int r;
    while (off < bytes) {
        if ((r = rd(s, lba)) != 0) return r;
        n = bytes - off < s->ssize ? bytes - off : s->ssize;
        if (!do_report) crc = ofs_crc32(crc, s->buf, (unsigned)n);
        else {
            /* entries start at multiples of 128 bytes, so the first 128
               bytes of one are always within one sector */
            for (i = (off + g->size - 1) / g->size * g->size; i < off + n; i += g->size) {
                e = s->buf + (unsigned)(i - off);
                if (!rd64(e) && !rd64(e + 8)) continue;   /* unused */
                memset(&p, 0, sizeof(p));
                p.scheme = OFS_PART_GPT;
                p.index = i / g->size + 1;
                memcpy(p.type_guid, e, 16);
                memcpy(p.guid, e + 16, 16);
                p.start = rd64(e + 32);
                p.count = rd64(e + 40) - p.start + 1;
                p.attr = rd64(e + 48);
                for (r = 0; r < 36; ++r) p.name[r] = rd16(e + 56 + 2 * r);
                if (rd64(e + 40) < p.start) return OFS_PART_EBAD;
                if ((r = report(s, &p)) != 0) return r;
            }
        }
        off += n; ++lba;
    }
    if (!do_report && crc != g->crc) return OFS_PART_EBAD;
    return 0;
}

static int gpt(struct scan *s, struct ofs_part_disk *d)
{
    struct gpt g;
    int r = gpt_header(s, 1, &g, d);
    if (!r) r = gpt_entries(s, &g, 0);
    if (r == OFS_PART_EBAD && s->sectors > 2) {
        r = gpt_header(s, s->sectors - 1, &g, d);
        if (!r) r = gpt_entries(s, &g, 0);
        if (!r) d->backup = 1;
    }
    if (r) return r;
    d->scheme = OFS_PART_GPT;
    return gpt_entries(s, &g, 1);
}

/* ---- MBR ---- */

static int extended(ofs_u8 type)
{
    return type == 0x05 || type == 0x0F || type == 0x85;
}

static void mbr_entry(const ofs_u8 *e, ofs_u64 base, ofs_u32 index, struct ofs_part *p)
{
    memset(p, 0, sizeof(*p));
    p->scheme = OFS_PART_MBR;
    p->index = index;
    p->active = e[0];
    p->type = e[4];
    p->start = base + rd32(e + 8);
    p->count = rd32(e + 12);
}

/* logical partitions: the chain of EBRs in the extended partition at ext */
static int logical(struct scan *s, ofs_u64 ext)
{
    ofs_u64 ebr = ext;
    ofs_u32 index = 5, hops;
    struct ofs_part p, next;
    int r;
    for (hops = 0; hops < 256; ++hops) {
        if ((r = rd(s, ebr)) != 0) return r;
        if (s->buf[510] != 0x55 || s->buf[511] != 0xAA) return OFS_PART_EBAD;
        mbr_entry(s->buf + 446, ebr, index, &p);
        mbr_entry(s->buf + 462, ext, 0, &next);
        if (p.type && p.count) {
            ++index;
            if ((r = report(s, &p)) != 0) return r;
        }
        if (!extended(next.type) || !next.count) return 0;
        if (next.start <= ebr) return OFS_PART_EBAD;      /* loops */
        ebr = next.start;
    }
    return OFS_PART_EBAD;
}

int ofs_part_scan(ofs_part_read_fn read, void *dev, unsigned sshift, ofs_u64 sectors,
                   ofs_u8 *buf, struct ofs_part_disk *d, ofs_part_fn fn, void *ctx)
{
    struct scan s;
    struct ofs_part p[4];
    ofs_u64 ext = 0;
    int i, r, protective = 0;
    memset(d, 0, sizeof(*d));
    if (sshift < 9 || sshift > 12) return OFS_PART_ESIZE;
    s.read = read; s.dev = dev; s.buf = buf; s.ssize = 1u << sshift;
    s.sectors = sectors; s.fn = fn; s.ctx = ctx; s.n = 0;
    if ((r = rd(&s, 0)) != 0) return r;
    if (buf[510] != 0x55 || buf[511] != 0xAA || ofs_part_is_vbr(buf)) return 0;
    for (i = 0; i < 4; ++i) {
        if (buf[446 + 16 * i] & 0x7F) return 0;            /* no MBR */
        mbr_entry(buf + 446 + 16 * i, 0, (ofs_u32)i + 1, &p[i]);
        if (p[i].type == 0xEE) protective = 1;
    }
    d->signature = rd32(buf + 440);
    if (protective) r = gpt(&s, d);
    else {
        d->scheme = OFS_PART_MBR;
        for (i = 0; i < 4 && !r; ++i) {
            if (!p[i].type || !p[i].count) continue;
            if (extended(p[i].type)) { if (!ext) ext = p[i].start; continue; }
            r = report(&s, &p[i]);
        }
        if (ext && !r) r = logical(&s, ext);
    }
    return r < 0 ? r : s.n;
}
