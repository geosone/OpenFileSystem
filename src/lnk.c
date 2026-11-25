/* SPDX-License-Identifier: GPL-2.0-or-later */
/* see include/ofs/lnk.h; the format after [MS-SHLLINK] and what Cygwin
   writes and accepts (path.cc: symlink_worker, check_shortcut) */
#include "ofs/lnk.h"

#define HDR      0x4C
#define F_IDLIST 0x01
#define F_DESC   0x04
#define F_REL    0x08
#define SW_NORMAL 1

/* CLSID_ShellLink {00021401-0000-0000-C000-000000000046} as stored */
static const ofs_u8 clsid[16] = {
    0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
};

static unsigned rd16(const ofs_u8 OFS_FAR *p) { return (unsigned)(p[0] | p[1] << 8); }
static ofs_u32 rd32(const ofs_u8 OFS_FAR *p) { return (ofs_u32)rd16(p) | (ofs_u32)rd16(p + 2) << 16; }
static void wr16(ofs_u8 OFS_FAR *p, unsigned v) { p[0] = (ofs_u8)v; p[1] = (ofs_u8)(v >> 8); }

int ofs_lnk_read(const ofs_u8 OFS_FAR *buf, ofs_u32 len, ofs_u16 *target, unsigned max, unsigned *tlen)
{
    ofs_u32 off, dlen, rlen, plen, i;
    unsigned k;
    if (len <= HDR || rd32(buf) != HDR) return 0;
    for (k = 0; k < 16; ++k)
        if (buf[4 + k] != clsid[k]) return 0;
    if ((rd32(buf + 0x14) & ~(ofs_u32)F_IDLIST) != (F_DESC | F_REL) || rd32(buf + 0x3C) != SW_NORMAL) return 0;
    off = HDR;
    if (rd32(buf + 0x14) & F_IDLIST) {
        if (off + 2 > len) return 0;
        off += 2 + rd16(buf + off);
    }
    if (off + 2 > len) return 0;
    dlen = rd16(buf + off);
    if (!dlen || off + 2 + dlen > len) return 0;
    if (buf[off + 2] == ':' && off + 3 < len && buf[off + 3] == '\\') return 0;   /* a Cygwin device */
    /* the full path after the relative one, if there is one */
    if (off + 2 + dlen + 2 <= len) {
        rlen = rd16(buf + off + 2 + dlen);
        i = off + 2 + dlen + 2 + rlen;
        if (i + 4 <= len && rd16(buf + i + 2) == 0xFEFF) {
            plen = rd16(buf + i) / 2;
            i += 4;
            if (i + plen * 2 > len) plen = (len - i) / 2;
            for (k = 0; k < plen && rd16(buf + i + 2 * k); ++k)
                ;
            if (!k || k > max) return 0;
            for (plen = k, k = 0; k < plen; ++k) target[k] = (ofs_u16)rd16(buf + i + 2 * k);
            *tlen = (unsigned)plen;
            return 1;
        }
    }
    /* else the description, in the ANSI code page (taken as Latin-1) */
    if (dlen > max) return 0;
    for (k = 0; k < dlen; ++k) target[k] = buf[off + 2 + k];
    *tlen = (unsigned)dlen;
    return 1;
}

/* an ANSI form: "?" for what is not ASCII, the separators as sep */
static void ansi(const ofs_u16 *t, unsigned n, ofs_u8 OFS_FAR *out, int sep)
{
    unsigned k;
    for (k = 0; k < n; ++k)
        out[k] = (ofs_u8)(t[k] == '/' || t[k] == '\\' ? (sep ? sep : t[k]) : t[k] < 0x80 ? t[k] : '?');
}

ofs_u32 ofs_lnk_make(const ofs_u16 *target, unsigned tlen, ofs_u8 OFS_FAR *buf, ofs_u32 size)
{
    static const char toolong[] = "[path too long]";
    unsigned dlen, k;
    ofs_u32 off, need, i;
    if (!tlen || tlen > OFS_LNK_TARGET_MAX) return 0;
    dlen = tlen > 260 ? sizeof(toolong) - 1 : tlen;
    need = HDR + 2 + dlen + 2 + tlen + 2 + 2 + 2 * (ofs_u32)tlen;
    if (size < need) return 0;
    for (i = 0; i < HDR; ++i) buf[i] = 0;
    buf[0] = HDR;
    for (k = 0; k < 16; ++k) buf[4 + k] = clsid[k];
    buf[0x14] = F_DESC | F_REL;
    buf[0x3C] = SW_NORMAL;
    off = HDR;
    wr16(buf + off, dlen);
    if (tlen > 260)
        for (k = 0; k < dlen; ++k) buf[off + 2 + k] = (ofs_u8)toolong[k];
    else
        ansi(target, tlen, buf + off + 2, 0);
    off += 2 + dlen;
    wr16(buf + off, tlen);
    ansi(target, tlen, buf + off + 2, '\\');
    off += 2 + tlen;
    wr16(buf + off, 2 * tlen);
    wr16(buf + off + 2, 0xFEFF);
    off += 4;
    for (k = 0; k < tlen; ++k, off += 2) wr16(buf + off, target[k]);
    return off;
}
