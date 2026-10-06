/*
 * libzd — disks: GPT partition tables, FAT32 and ext4 file systems, and the
 * newc cpio archives that become the system's initramfs. Used by the
 * installer and by the image builder; everything writes through plain
 * file descriptors, so it works on a device or an image file alike.
 */
#pragma once

#include "zr.h"

#define SECTOR 512u

/* ---- GPT ------------------------------------------------------------------------- */

typedef struct {
	u8 type[16];
	u8 guid[16];
	u64 first_lba, last_lba;  /* inclusive */
	u64 attrs;
	char name[37];             /* ASCII; stored as UTF-16 */
	u32 slot;                  /* entry number in the table from 1, or 0 for the first free one */
} GptPart;

extern const u8 GPT_TYPE_ESP[16];
extern const u8 GPT_TYPE_LINUX[16];
extern const u8 GPT_TYPE_MS_DATA[16];

void guid_random(u8 out[16]);
void guid_format(const u8 g[16], char out[37]);
/* Writes a protective MBR, the primary and the backup GPT. */
bool gpt_write(int fd, u64 disk_sectors, const GptPart *parts, int n, const u8 disk_guid[16]);
/* Reads the primary GPT (each partition keeps its slot, so rewriting the
 * table keeps partition numbers). Returns the number of partitions, or -1. */
int gpt_read(int fd, u64 disk_sectors, GptPart *parts, int max, u8 disk_guid[16]);
/* The first usable and last usable sector of a GPT disk. */
u64 gpt_first_usable(void);
u64 gpt_last_usable(u64 disk_sectors);
/* The largest free, 1 MiB-aligned stretch of a GPT disk: [*first, *last]. Returns its size in sectors. */
u64 gpt_largest_gap(u64 disk_sectors, const GptPart *parts, int n, u64 *first, u64 *last);

/* ---- FAT32 -------------------------------------------------------------------------- */

typedef struct FatWriter FatWriter;

/* Formats [start, start+sectors) as FAT32 and returns a writer for adding files. */
FatWriter *fat_format(int fd, u64 start_lba, u64 sectors, const char *label);
/* Adds a file or folder at an absolute 8.3-friendly path like "/EFI/BOOT/BOOTX64.EFI". */
bool fat_mkdir(FatWriter *w, const char *path);
bool fat_add_file(FatWriter *w, const char *path, const void *data, usize len);
bool fat_add_file_from(FatWriter *w, const char *path, const char *source); /* copies a host file */
/* Writes the tables and directories; frees the writer. */
bool fat_finish(FatWriter *w);

/* ---- ext4 ------------------------------------------------------------------------------- */

/* Formats [start, start+bytes) as ext4 (no journal; inode tables zeroed lazily
 * by the kernel). The root folder and lost+found exist afterwards. */
bool ext4_format(int fd, u64 start_byte, u64 bytes, const char *label, const u8 uuid[16]);

/* ---- cpio (newc) --------------------------------------------------------------------------- */

typedef struct {
	int fd;
	u32 ino;
	u64 written;
} CpioWriter;

void cpio_begin(CpioWriter *w, int fd);
bool cpio_dir(CpioWriter *w, const char *path, u32 mode);
bool cpio_file(CpioWriter *w, const char *path, u32 mode, const void *data, usize len);
bool cpio_symlink(CpioWriter *w, const char *path, const char *target);
bool cpio_device(CpioWriter *w, const char *path, u32 mode, u32 major, u32 minor);
bool cpio_end(CpioWriter *w);
