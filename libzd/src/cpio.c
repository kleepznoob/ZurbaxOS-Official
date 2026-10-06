/* newc cpio archives, the format the kernel unpacks into its initial root. */
#include "zd.h"

static bool out(CpioWriter *w, const void *data, usize n)
{
	if (!write_all(w->fd, data, n))
		return false;
	w->written += n;
	return true;
}

static bool pad4(CpioWriter *w)
{
	static const u8 zero[4];
	usize n = (4 - w->written % 4) % 4;
	return !n || out(w, zero, n);
}

static void hex8(char *p, u32 v)
{
	static const char digits[] = "0123456789ABCDEF";
	for (int i = 7; i >= 0; i--, v >>= 4)
		p[i] = digits[v & 15];
}

static bool entry(CpioWriter *w, const char *path, u32 mode, u32 nlink, const void *data, usize len, u32 rmajor, u32 rminor)
{
	/* Archive paths are relative: "system/bin/zinit", never "/system/bin/zinit". */
	while (*path == '/')
		path++;
	usize name_len = strlen(path) + 1;
	char h[110];
	memcpy(h, "070701", 6);
	u32 fields[13] = { w->ino++, mode, 0, 0, nlink, 0, (u32)len, 0, 0, rmajor, rminor, (u32)name_len, 0 };
	for (int i = 0; i < 13; i++)
		hex8(h + 6 + i * 8, fields[i]);
	return out(w, h, sizeof h) && out(w, path, name_len) && pad4(w) && (!len || out(w, data, len)) && pad4(w);
}

void cpio_begin(CpioWriter *w, int fd)
{
	w->fd = fd;
	w->ino = 1;
	w->written = 0;
}

bool cpio_dir(CpioWriter *w, const char *path, u32 mode)
{
	return entry(w, path, S_IFDIR | (mode & 07777), 2, NULL, 0, 0, 0);
}

bool cpio_file(CpioWriter *w, const char *path, u32 mode, const void *data, usize len)
{
	return entry(w, path, S_IFREG | (mode & 07777), 1, data, len, 0, 0);
}

bool cpio_symlink(CpioWriter *w, const char *path, const char *target)
{
	return entry(w, path, S_IFLNK | 0777, 1, target, strlen(target), 0, 0);
}

bool cpio_device(CpioWriter *w, const char *path, u32 mode, u32 major, u32 minor)
{
	return entry(w, path, mode, 1, NULL, 0, major, minor);
}

bool cpio_end(CpioWriter *w)
{
	if (!entry(w, "TRAILER!!!", 0, 1, NULL, 0, 0, 0))
		return false;
	/* Round up to 512 bytes like other archivers do. */
	static const u8 zero[512];
	usize n = (512 - w->written % 512) % 512;
	return !n || out(w, zero, n);
}
