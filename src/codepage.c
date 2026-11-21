/* OEM code pages from the UNI_<cp>.TAB files of mars-unicode-tables
   (generated from the Unicode.org mappings), and the DOS upper case of
   their characters 80h-FFh. */
#include "ofs/codepage.h"
#include "ofs/exfat_format.h"

#define TAB_HEADER 256
#define TAB_UNDEFINED 0xFFFDu

static ofs_u16 rd16(const ofs_u8 *p) { return (ofs_u16)(p[0] | p[1] << 8); }

ofs_u16 ofs_unicode_upper(ofs_u16 u)
{
    /* the compressed table: 0xFFFF and a count stand for that many
       characters that stay as they are */
    ofs_u32 i, c = 0;
    for (i = 0; i + 1 < OFS_EXFMT_UPCASE_LEN && c <= u; i += 2) {
        ofs_u16 v = rd16(ofs_exfmt_upcase + i);
        if (v == 0xFFFF && i + 3 < OFS_EXFMT_UPCASE_LEN) {
            c += rd16(ofs_exfmt_upcase + i + 2);
            i += 2;
            continue;
        }
        if (c == u) return v;
        ++c;
    }
    return u;
}

static int find(const struct ofs_cp *cp, ofs_u16 u)
{
    unsigned i;
    if (u < 0x80) return u;
    for (i = 0; i < 128; ++i) if (cp->uni[i] == u) return (int)(0x80 + i);
    return -1;
}

int ofs_cp_load(struct ofs_cp *cp, ofs_u16 number, const ofs_u8 *tab, ofs_u32 size)
{
    const ofs_u8 *t = tab + TAB_HEADER + 4;
    unsigned i;
    if (size < TAB_HEADER + 4 + 512 || rd16(tab + TAB_HEADER) != 1 || rd16(tab + TAB_HEADER + 2))
        return -1;
    for (i = 0; i < 0x80; ++i)
        if (rd16(t + 2 * i) != i) return -1; /* no OEM code page: ASCII differs */
    cp->cp = number;
    for (i = 0; i < 128; ++i) cp->uni[i] = rd16(t + 2 * (0x80 + i));
    for (i = 0; i < 128; ++i) {
        ofs_u16 u = cp->uni[i], up;
        int c;
        cp->upper[i] = (ofs_u8)(0x80 + i);
        if (u == TAB_UNDEFINED) continue;
        up = ofs_unicode_upper(u);
        if ((c = find(cp, up)) >= 0) cp->upper[i] = (ofs_u8)c;
        else if (ofs_latin_base(up)) cp->upper[i] = ofs_latin_base(up);
    }
    return 0;
}
