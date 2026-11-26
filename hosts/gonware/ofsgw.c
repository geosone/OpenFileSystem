/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The GonWare host: one driver of GonWare's volume layer (struct fs_driver,
   kernel/include/kernel/fs.h) over the common interface (include/ofs/vol.h).
   GonWare builds it once per module with the cores that module carries:
   FAT.GLM with OFS_NO_NTFS OFS_NO_ISO OFS_NO_UDF (FAT12/16/32 and exFAT),
   NTFS.GLM with OFS_NO_FAT OFS_NO_EXFAT OFS_NO_ISO OFS_NO_UDF
   OFS_READONLY, CDFS.GLM (UDF, ISO 9660) with OFS_NO_FAT OFS_NO_EXFAT
   OFS_NO_NTFS OFS_READONLY. OFS_GW_NAME names the driver, the
   module itself (GLM_MODULE) stays GonWare's and calls fs_register with
   ofs_gw_driver.

   The volume layer serialises the calls (fs_mutex), as vol.h wants it.
   A volume's ofs_vol and its buffer are allocated on its first mount
   under the module's heap owner (ofs_gw_init), kept with the volume slot
   and reused when it is mounted again: the layer has no unmount. */
#include <kernel/clock.h>
#include <kernel/fs.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include "ofs/vol.h"

#ifndef OFS_GW_NAME
#define OFS_GW_NAME "OFS"
#endif

_Static_assert(sizeof(struct ofs_node) <= sizeof(((struct fs_node *)0)->u), "fs_node too small for ofs_node");

#define SLOTS 24                  /* kernel/fs/fs.c MAX_VOLUMES */

struct slot {
	struct ofs_vol vol;
	ofs_u8 buf[OFS_VOL_BUF] __attribute__((aligned(64)));
};

static struct slot *slots[SLOTS];
static struct volume *owners[SLOTS];
static unsigned owner_id;

static struct slot *slot_for(struct volume *v)
{
	unsigned i, freeslot = SLOTS;

	for (i = 0; i < SLOTS; i++) {
		if (owners[i] == v)
			return slots[i];
		if (!owners[i] && freeslot == SLOTS)
			freeslot = i;
	}
	if (freeslot == SLOTS)
		return NULL;
	if (!slots[freeslot]) {
		unsigned prev = kmem_set_owner(owner_id);
		slots[freeslot] = kmalloc(sizeof(struct slot));
		kmem_set_owner(prev);
		if (!slots[freeslot])
			return NULL;
	}
	owners[freeslot] = v;
	return slots[freeslot];
}

#define VOL(v) ((struct ofs_vol *)(v)->u.priv)
#define NODE(n) ((struct ofs_node *)(void *)(n)->u.raw)

static int rd(void *dev, ofs_lba lba, void *buf) { return fs_disk_read(dev, lba, buf) ? -1 : 0; }
#ifndef OFS_READONLY
static int wr(void *dev, ofs_lba lba, const void *buf) { return fs_disk_write(dev, lba, buf) ? -1 : 0; }
#endif

static void now(void *dev, ofs_u16 *t, ofs_u16 *d)
{
	(void)dev;
	fs_dos_time(t, d);
}

/* ISO 9660 and UDF have no boot sector to probe: their mount looks at
   block 16 and the anchor at 256 */
static int probe(const uint8_t *s)
{
#if !defined(OFS_NO_ISO) || !defined(OFS_NO_UDF)
	(void)s;
	return 1;
#else
	return ofs_probe(s);
#endif
}

/* the volume layer's label: 11 bytes; FAT's as stored (OEM code page),
   the others' ASCII with '?' for the rest */
static void label(struct volume *v, struct ofs_vol *o)
{
	const unsigned char *p = (const unsigned char *)o->label;
	unsigned k = 0;

#ifndef OFS_NO_FAT
	if (o->type[0] == 'F') {
		unsigned n = 11;
		while (n && o->u.fat.label[n - 1] == ' ')
			n--;
		memcpy(v->label, o->u.fat.label, n);
		v->label[n] = 0;
		return;
	}
#endif
	while (*p && k < 11) {
		if (*p < 0x80) {
			v->label[k++] = (char)*p++;
			continue;
		}
		v->label[k++] = '?';
		for (++p; (*p & 0xC0) == 0x80; ++p)
			;
	}
	v->label[k] = 0;
}

static int mount(struct volume *v)
{
	struct slot *s = slot_for(v);
	struct ofs_host host;
	int r;

	if (!s)
		return FS_EFULL;
	memset(&host, 0, sizeof(host));
	host.read = rd;
#ifndef OFS_READONLY
	host.write = v->read_only ? NULL : wr;
#endif
	host.now = now;
	host.tz = (ofs_s16)(clock_zone_offset() / 60); /* NTFS keeps UTC, DOS times are local */
	while ((1u << host.sshift) < v->disk->block_size)
		host.sshift++;
	r = ofs_mount(&s->vol, &host, v, v->start, fs_codepage(), s->buf);
	if (r)
		return r;
	v->u.priv = &s->vol;
	v->type = s->vol.type;
	v->dirty = s->vol.dirty; /* not cleanly unmounted: checked first */
	if (!(s->vol.caps & OFS_CAP_WRITE))
		v->read_only = 1;
	label(v, &s->vol);
	return FS_OK;
}

static int root(struct volume *v, struct fs_node *n)
{
	n->dir = 1;
	n->attr = FS_ATTR_DIR;
	n->size = 0;
	return ofs_root(VOL(v), NODE(n));
}

static void node(struct volume *v, const struct ofs_dirent *de, struct fs_node *n)
{
	memset(n, 0, sizeof(*n));
	n->vol = v;
	n->dir = de->node.dir;
	n->attr = de->node.attr;
	n->size = de->node.size;
	*NODE(n) = de->node;
}

/* scratch: calls come one at a time */
static struct ofs_dirent ent;
static uint16_t n16[256];
#ifndef OFS_READONLY
static uint16_t t16[256];
#endif

static int lookup(struct fs_node *dir, const char *name, struct fs_node *out)
{
	unsigned len = fs_utf8_to16(name, n16, 256);
	int r;

	if (len > 255)
		return FS_ENOENT;
	if ((r = ofs_lookup(VOL(dir->vol), NODE(dir), n16, len, &ent)))
		return r;
	node(dir->vol, &ent, out);
	return FS_OK;
}

static int readdir(struct fs_node *dir, uint32_t *pos, struct fs_dirent *de)
{
	int r = ofs_next(VOL(dir->vol), NODE(dir), pos, &ent);

	if (r)
		return r;
	fs_utf16_to8(ent.name, ent.name_len, de->name);
	de->attr = ent.attr;
	de->size = ent.node.size;
	de->time = ent.time;
	de->date = ent.date;
	de->ctime = ent.ctime;
	de->cdate = ent.cdate;
	de->ccs10 = ent.ctime_cs;
	de->atime = ent.atime;
	de->adate = ent.adate;
	return FS_OK;
}

static int read(struct fs_node *n, uint64_t off, void *buf, uint32_t len, uint32_t *done)
{
	ofs_u32 got = 0;
	int r = ofs_read(VOL(n->vol), NODE(n), off, buf, len, &got);

	*done = got;
	return r;
}

#ifndef OFS_READONLY
static int write(struct fs_node *n, uint64_t off, const void *buf, uint32_t len)
{
	int r = ofs_write(VOL(n->vol), NODE(n), off, buf, len);

	n->size = NODE(n)->size;
	return r;
}

static int create(struct fs_node *dir, const char *name, int is_dir, struct fs_node *out)
{
	unsigned len = fs_utf8_to16(name, n16, 256);
	int r;

	if (len > 255)
		return FS_ENAME;
	if ((r = ofs_create(VOL(dir->vol), NODE(dir), n16, len, is_dir, &ent)))
		return r;
	node(dir->vol, &ent, out);
	return FS_OK;
}

static int sync(struct volume *v) { return ofs_sync(VOL(v)); }
#endif

static int statfs(struct volume *v, uint64_t *free, uint64_t *total, uint32_t *unit)
{
	ofs_u32 fr, cb, cl;
	int r = ofs_statfs(VOL(v), &fr, &cb, &cl);

	if (r)
		return r;
	*free = fr;
	*total = cl;
	*unit = cb;
	return FS_OK;
}

#ifndef OFS_READONLY

static int resize(struct fs_node *n, uint64_t size)
{
	int r = ofs_resize(VOL(n->vol), NODE(n), size);

	n->size = NODE(n)->size;
	return r;
}

static int remove(struct fs_node *dir, const char *name)
{
	unsigned len = fs_utf8_to16(name, n16, 256);

	if (len > 255)
		return FS_ENOENT;
	return ofs_remove(VOL(dir->vol), NODE(dir), n16, len);
}

static int rename(struct fs_node *dir, const char *name, struct fs_node *to, const char *to_name)
{
	unsigned len = fs_utf8_to16(name, n16, 256), tlen = fs_utf8_to16(to_name, t16, 256);

	if (len > 255)
		return FS_ENOENT;
	if (tlen > 255)
		return FS_ENAME;
	return ofs_rename(VOL(dir->vol), NODE(dir), n16, len, NODE(to), t16, tlen);
}

static int setattr(struct fs_node *dir, const char *name, uint8_t attr)
{
	unsigned len = fs_utf8_to16(name, n16, 256);

	if (len > 255)
		return FS_ENOENT;
	return ofs_setattr(VOL(dir->vol), NODE(dir), n16, len, attr);
}

/* FS_TIME_* are OFS_TIME_*'s numbers */
_Static_assert(FS_TIME_CREATED == OFS_TIME_CREATED && FS_TIME_MODIFIED == OFS_TIME_MODIFIED &&
		       FS_TIME_ACCESSED == OFS_TIME_ACCESSED,
	       "FS_TIME_ and OFS_TIME_ differ");
static int settime(struct fs_node *dir, const char *name, unsigned which, uint16_t time, uint16_t date,
		   uint8_t cs10)
{
	unsigned len = fs_utf8_to16(name, n16, 256);

	if (len > 255)
		return FS_ENOENT;
	return ofs_settime(VOL(dir->vol), NODE(dir), n16, len, which, time, date, cs10);
}
#endif

/* FS_XATTR_SET .. _REMOVE are OFS_EA_*'s numbers */
_Static_assert(FS_XATTR_SET == OFS_EA_SET && FS_XATTR_CREATE == OFS_EA_CREATE &&
		       FS_XATTR_REPLACE == OFS_EA_REPLACE && FS_XATTR_REMOVE == OFS_EA_REMOVE,
	       "FS_XATTR_ and OFS_EA_ differ");
static int xattr(struct fs_node *dir, const char *name, int op, const char *ea, void *buf, uint32_t size,
		 uint32_t *len)
{
	unsigned l = fs_utf8_to16(name, n16, 256);
	uint16_t n = 0;
	int r;

	if (l > 255)
		return FS_ENOENT;
	if (size > 0xFFFF) {
		if (op != FS_XATTR_GET && op != FS_XATTR_LIST)
			return FS_EFULL;          /* a file's EAs hold at most 64 KiB */
		size = 0xFFFF;
	}
	if (op == FS_XATTR_GET)
		r = ofs_getea(VOL(dir->vol), NODE(dir), n16, l, ea, buf, (uint16_t)size, &n);
	else if (op == FS_XATTR_LIST)
		r = ofs_listea(VOL(dir->vol), NODE(dir), n16, l, buf, (uint16_t)size, &n);
#ifndef OFS_READONLY
	else
		r = ofs_setea(VOL(dir->vol), NODE(dir), n16, l, ea, buf, (uint16_t)size, op);
#else
	else
		r = FS_EROFS;
#endif
	*len = n;
	return r == OFS_ENOATTR ? FS_ENOATTR : r == OFS_ERANGE ? FS_ERANGE : r;
}

const struct fs_driver ofs_gw_driver = {
	OFS_GW_NAME, probe, mount, root, lookup, readdir, read,
#ifndef OFS_READONLY
	write, create, sync, resize, remove, rename,
#else
	NULL, NULL, NULL, NULL, NULL, NULL,
#endif
	statfs,
#ifndef OFS_READONLY
	setattr, settime,
#else
	NULL, NULL,
#endif
	xattr,
};

/* from the module's init: its heap owner holds the volumes' buffers */
void ofs_gw_init(void)
{
	owner_id = kmem_owner();
	fs_register(&ofs_gw_driver);
}
