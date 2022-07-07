/*-
 * Copyright (c) 2022 Jason R. Thorpe.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * cocofs -- A tool to interact with TRS-80 CoCo floppy disk images.
 *
 * cocofs has the following commands:
 *
 * ==> ls	List the contents of a floppy disk.
 *
 * ==> copyout	Copy one or more files from the floppy disk into the
 *		current working directory.
 *
 * ==> copyin	Copy one or more files to the floppy disk.  The type
 *		and encoding will be guessed for each file, based on
 *		the file extension; overriding on a per-file basis
 *		is possible using the following format for the file
 *		names:
 *
 *			FOO.DAT[binary,data]
 *			HELLO.C[ascii,data]
 *
 *		The following type qualifiers are allowed:
 *
 *			basic
 *			data
 *			code
 *			text
 *
 *		The following encoding qualifiers are allowed:
 *
 *			binary
 *			ascii
 *
 *		One or both qualifiers may be specified, and in any order.
 *		The default if the default type/encoding cannot be guessed,
 *		or if qualifiers are specified, is binary data.
 *
 *		There is a slight danger that [ and ] are legitimate
 *		characters in the file name, but it's extremely unlikely
 *		because those keys don't exist on CoCo keyboard.
 *
 * ==> rm	Remove one or more files from the floppy disk.
 *
 * ==> format	Create a new floppy image.
 *
 * ==> dump	Dump information about the floppy disk.  This is
 *		essentially an enhanced version of the "ls" command
 *		that also shows information about the layout of the
 *		files on disk and shows additional information when
 *		disk format errors are encountered.
 */

#include <sys/stat.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Format information is gleaned from:
 *
 *	http://dragon32.info/info/tandydsk.html
 *
 * The CoCo DOS supports a single disk format:
 *
 *	1 head
 *	35 tracks (0 - 34)
 *	18 sectors per track (1 - 18)
 *	256 bytes per sector
 *
 * Like other TRSDOS formats, there are 2 granules per track, meaning
 * that a granule is group of 9 sectors.  With 35 tracks, there are a
 * total of 70 granules.  The directory is stored in track 17, which
 * leaves 68 granules for file data.
 *
 * The total size of the file system, including all metadata, is thus:
 *
 *	35*18*256 == 161280 == 157.5KiB
 *
 * ...and the size available for files is:
 *
 *	68*9*256 == 156672 == 153KiB
 */
#define	COCOFS_TRACKS		35
#define	COCOFS_SEC_PER_TRACK	18
#define	COCOFS_SEC_PER_GRANULE	9
#define	COCOFS_BYTES_PER_SEC	256
#define	COCOFS_BYTES_PER_TRACK	(COCOFS_SEC_PER_TRACK * COCOFS_BYTES_PER_SEC)
#define	COCOFS_BYTES_PER_GRANULE \
	(COCOFS_SEC_PER_GRANULE * COCOFS_BYTES_PER_SEC)

#define	COCOFS_TOTALSIZE	\
	(COCOFS_TRACKS * COCOFS_SEC_PER_TRACK * COCOFS_BYTES_PER_SEC)

#define	COCOFS_DIR_TRACK	17
#define	COCOFS_GRANULES_PER_TRACK \
	(COCOFS_SEC_PER_TRACK / COCOFS_SEC_PER_GRANULE)
#define	COCOFS_NGRANULES	\
	((COCOFS_TRACKS - 1) * COCOFS_GRANULES_PER_TRACK)

static unsigned int
cocofs_track_to_offset(unsigned int track)
{
	return track * (COCOFS_SEC_PER_TRACK * COCOFS_BYTES_PER_SEC);
}

static unsigned int
cocofs_sector_to_offset(unsigned int sector)
{
	/* sectors are numbered 1 - 18 */
	return (sector - 1) * COCOFS_BYTES_PER_SEC;
}

static unsigned int
cocofs_granule_to_track(unsigned int granule)
{
	unsigned int track;

	track = granule / COCOFS_GRANULES_PER_TRACK;
	if (track >= COCOFS_DIR_TRACK) {
		track++;
	}

	return track;
}

static unsigned int
cocofs_granule_to_offset(unsigned int granule)
{
	unsigned int track;
	unsigned int offset;

	track = cocofs_granule_to_track(granule);
	offset = cocofs_track_to_offset(track);
	if (granule & 1) {
		/* There are 2 granules per track. */
		offset += COCOFS_BYTES_PER_GRANULE;
	}

	return offset;
}

/*
 * CoCo DOS directory track:
 *
 * Sector 2 contains the Granule Map.
 * Sectors 3 - 11 store the directory entries
 *
 * The Granule Map works more or less like the FAT in MS-DOS.  Each byte
 * represents one granule:
 *
 * 0 - 67	Points to the next granule in the file chain
 * 0xc0 - 0xc9	This granule is the last granule in the chain.  The lower
 *		4 bits indicate how many sectors in the last granule are
 *		used.  NOTE: This implies that 0xc0 should never be
 *		seen in the wild.
 * 0xff		This granule is free.
 * ...		Any other value indicates either a corrupt Granule Map
 *		entry, or a means of allocating space to "hidden" files.
 *
 * The diretctory entry format is 32 bytes long and is as follows:
 *
 * 0 - 7	file name (padded with ' ')
 * 8 - 10	extension (padded with ' ')
 * 11		file type:
 *			0x00 Basic
 *			0x01 Data
 *			0x02 Machine code program
 *			0x03 Text editor
 *
 * 12		encoding:
 *			0x00 binary
 *			0xff ASCII
 * 13		first granule (0 - 67)
 * 14 - 15	number of bytes used in last sector of file (!! big-endian !!)
 * [remainder unused]
 */
#define	GMAP_SECTOR		2

#define	GMAP_FREE		0xff
#define	GMAP_ALLOCATED		0xfe	/* pseudo; see cocofs_galloc() */
#define	GMAP_LAST		0xc0
#define	GMAP_IS_LAST(v)		(((v) & GMAP_LAST) == GMAP_LAST)
#define	GMAP_LAST_NSEC(v)	(((v) & 0x0f))

static bool
gmap_entry_is_valid(uint8_t v)
{
	return (v <= COCOFS_NGRANULES) ||
	       (v >= 0xc0 && v <= 0xc9) ||
	       v == GMAP_FREE;
}

struct cocofs_dirent {
	int8_t		d_name[8];
	int8_t		d_ext[3];
	uint8_t		d_type;
	uint8_t		d_encoding;
	uint8_t		d_first_granule;
	uint8_t		d_last_bytes[2];
	uint8_t		d_unused[16];
};

#define	COCOFS_DIRENT_TYPE_BASIC	0x00
#define	COCOFS_DIRENT_TYPE_DATA		0x01
#define	COCOFS_DIRENT_TYPE_CODE		0x02
#define	COCOFS_DIRENT_TYPE_TEXT		0x03
#define	COCOFS_DIRENT_TYPE_FREE		0xff

#define	COCOFS_DIRENT_ENC_BINARY	0x00
#define	COCOFS_DIRENT_ENC_ASCII		0xff

#define	COCOFS_DIR_TRACK_FIRST_SEC	3
#define	COCOFS_DIR_TRACK_LAST_SEC	11
#define	COCOFS_DIR_TRACK_NSEC		9
#define	COCOFS_DIR_TRACK_NENTRIES					\
	((COCOFS_DIR_TRACK_NSEC * COCOFS_BYTES_PER_SEC) / 		\
	 sizeof(struct cocofs_dirent))

/*
 * In-memory representation of a CoCo DOS file system.
 */
struct cocofs {
	int		fd;		/* file descriptor backing the image */
	uint8_t		*image_data;	/* full image data */
	uint8_t		*granule_map;	/* pointer to the Granule Map */
	struct cocofs_dirent *directory;/* pointer to the directory */
	unsigned int	free_granules;	/* # of free granules */
};

/*
 * stat(2)-like information about a CoCo DOS file.
 */
struct cocofs_stat {
	uint32_t	st_size;	/* total size */
	char		st_name[8+1];	/* name (null-terminated) */
	char		st_ext[3+1];	/* extension (null-terminated) */
	uint8_t		st_type;	/* file type */
	uint8_t		st_encoding;	/* file encoding */
};

static char
cocofs_mapchar(char c)
{
	if (c >= 'a' && c <= 'z') {
		return 'A' + (c - 'a');
	}
	return c;
}

static bool
cocofs_conv_name(const char *full, char name[8], char ext[3])
{
	int i;

	memset(name, ' ', 8);
	memset(ext, ' ', 3);

	for (i = 0; *full != '\0'; i++, full++) {
		if (*full == '.') {
			full++;
			break;
		}
		if (i == 8) {
			return false;
		}
		name[i] = cocofs_mapchar(*full);
	}

	for (i = 0; *full != '\0'; i++, full++) {
		if (i == 3) {
			return false;
		}
		ext[i] = cocofs_mapchar(*full);
	}

	return true;
}

static const char *
plural(long v)
{
	return v == 1 ? "" : "s";
}

struct str2val {
	const char *str;
	unsigned int val;
};

static const struct str2val *
str2val_lookup_val(const struct str2val *tab, uint8_t val)
{
	for (; tab->str != NULL; tab++) {
		if (tab->val == val) {
			return tab;
		}
	}
	return NULL;
}

static const struct str2val *
str2val_lookup_str(const struct str2val *tab, const char *str)
{
	for (; tab->str != NULL; tab++) {
		if (strcasecmp(tab->str, str) == 0) {
			return tab;
		}
	}
	return NULL;
}

static const struct str2val cocofs_default_types_and_encodings[] = {
	{ "ASM",	(COCOFS_DIRENT_TYPE_DATA << 8) |
				COCOFS_DIRENT_ENC_ASCII },

	{ "BAS",	(COCOFS_DIRENT_TYPE_BASIC << 8) |
				COCOFS_DIRENT_ENC_BINARY },

	{ "BIN",	(COCOFS_DIRENT_TYPE_CODE << 8) |
				COCOFS_DIRENT_ENC_BINARY },

	{ "DAT",	(COCOFS_DIRENT_TYPE_DATA << 8) |
				COCOFS_DIRENT_ENC_BINARY },

	{ "TXT",	(COCOFS_DIRENT_TYPE_TEXT << 8) |
				COCOFS_DIRENT_ENC_ASCII },

	{ "C",		(COCOFS_DIRENT_TYPE_DATA << 8) |
				COCOFS_DIRENT_ENC_ASCII },

	{ "H",		(COCOFS_DIRENT_TYPE_DATA << 8) |
				COCOFS_DIRENT_ENC_ASCII },

	{ NULL },
};

static const struct str2val cocofs_dir_types[] = {
	{ "Basic",	COCOFS_DIRENT_TYPE_BASIC },
	{ "Data",	COCOFS_DIRENT_TYPE_DATA },
	{ "Code",	COCOFS_DIRENT_TYPE_CODE },
	{ "Text",	COCOFS_DIRENT_TYPE_TEXT },
	{ NULL },
};

static const struct str2val cocofs_dir_encodings[] = {
	{ "Binary",	COCOFS_DIRENT_ENC_BINARY },
	{ "ASCII",	COCOFS_DIRENT_ENC_ASCII },
	{ NULL },
};

static const char *
cocofs_dir_type(uint8_t t)
{
	static char buf[sizeof("<type 0xff>")];
	const struct str2val *tab;

	tab = str2val_lookup_val(cocofs_dir_types, t);
	if (tab != NULL) {
		return tab->str;
	}

	snprintf(buf, sizeof(buf), "<type 0x%02x>", t);
	return buf;
}

static const char *
cocofs_dir_encoding(uint8_t e)
{
	static char buf[sizeof("<encoding 0xff>")];
	const struct str2val *tab;

	tab = str2val_lookup_val(cocofs_dir_encodings, e);
	if (tab != NULL) {
		return tab->str;
	}

	snprintf(buf, sizeof(buf), "<encoding 0x%02x>", e);
	return buf;
}

static void
cocofs_default_type_and_encoding(const char *ext,
    uint8_t *typep, uint8_t *encp)
{
	const struct str2val *tab;

	tab = str2val_lookup_str(cocofs_default_types_and_encodings, ext);
	if (tab != NULL) {
		*typep = tab->val >> 8;
		*encp = tab->val & 0xff;
	} else {
		/* default to binary data. */
		*typep = COCOFS_DIRENT_TYPE_DATA;
		*encp = COCOFS_DIRENT_ENC_BINARY;
	}
}

/* N.B. modifies fname. */
static bool
cocofs_parse_fname(char *fname, char name[8], char ext[3],
    uint8_t *typep, uint8_t *encp)
{
	char *qual1 = NULL, *qual2 = NULL;
	char *cp1, *cp2;
	size_t fnamelen;
	bool have_type = false, have_enc = false;
	uint8_t type = COCOFS_DIRENT_TYPE_DATA;
	uint8_t enc = COCOFS_DIRENT_ENC_BINARY;

	fnamelen = strlen(fname);

	/*
	 * Check for qualifiers, separatre them from the file name
	 * and separators.
	 */
	cp2 = &fname[fnamelen - 1];
	if (*cp2 == ']') {
		for (cp1 = cp2; cp1 > fname; cp1--) {
			if (*cp1 == '[') {
				break;
			}
		}
		if (cp1 > fname) {
			/* We have qualifiers. */
			assert(*cp1 == '[');
			*cp1++ = '\0';
			*cp2 = '\0';
			
			qual1 = cp1;

			cp2 = strchr(cp1, ',');
			if (cp2 != NULL) {
				*cp2++ = '\0';
				qual2 = cp2;
			}
		}
	}

	/*
	 * fname is now separated from the qualifiers.  Parse the qualifiers,
	 * if we have them.
	 */
	for (cp1 = qual1; cp1 != NULL; cp1 = (cp1 == qual1) ? qual2 : NULL) {
		const struct str2val *tab;

		if ((tab = str2val_lookup_str(cocofs_dir_types,
					      cp1)) != NULL) {
			if (have_type) {
				fprintf(stderr,
				    "multiple types specified for %s\n",
				    fname);
				return false;
			}
			type = tab->val;
			have_type = true;
		} else if ((tab = str2val_lookup_str(cocofs_dir_encodings,
						    cp1)) != NULL) {
			if (have_enc) {
				fprintf(stderr,
				    "multiple encodings specified for %s\n",
				    fname);
				return false;
			}
			enc = tab->val;
			have_enc = true;
		} else {
			fprintf(stderr,
			    "unknown type/encoding qualifier for %s: %s\n",
			    fname, cp1);
			return false;
		}
	}

	/*
	 * Now convert the file name into the correct form.  Look only
	 * at the final path component.
	 */
	cp1 = strrchr(fname, '/');
	if (cp1 != NULL) {
		cp1++;
	} else {
		cp1 = fname;
	}
	if (! cocofs_conv_name(cp1, name, ext)) {
		fprintf(stderr,
		    "invalid file name: %s\n", fname);
		return false;
	}

	/*
	 * If qualifiers were not specified, then try to guess based
	 * on the file name extension.
	 */
	if (!have_type && !have_enc && (cp2 = strchr(cp1, '.')) != NULL) {
		cocofs_default_type_and_encoding(++cp2, &type, &enc);
	}

	*typep = type;
	*encp = enc;

	return true;
}

static unsigned int
cocofs_dir_lastbytes(const uint8_t *lastbytes)
{
	return (lastbytes[0] << 8) | lastbytes[1];
}

static void
cocofs_dir_set_lastbytes(unsigned int cnt, uint8_t *lastbytes)
{
	assert(cnt <= COCOFS_BYTES_PER_SEC);
	lastbytes[0] = (uint8_t)(cnt >> 8);
	lastbytes[1] = (uint8_t)cnt;
}

/*
 * We provide our own versions of pread() and pwrite() in order to
 * improve code portability.
 */

static ssize_t
cocofs_pread(int d, void *buf, size_t nbyte, off_t offset)
{
	if (lseek(d, offset, SEEK_SET) == -1) {
		return -1;
	}
	return read(d, buf, nbyte);
}

static ssize_t
cocofs_pwrite(int d, const void *buf, size_t nbyte, off_t offset)
{
	if (lseek(d, offset, SEEK_SET) == -1) {
		return -1;
	}
	return write(d, buf, nbyte);
}

static struct cocofs *
cocofs_alloc(int fd)
{
	struct cocofs *fs = calloc(1, sizeof(*fs));
	assert(fs != NULL);

	/* Allocate a fresh, zero'd image. */
	fs->image_data = calloc(1, COCOFS_TOTALSIZE);
	assert(fs->image_data != NULL);

	/* Cache pointers to Granule Map and directory. */
	uint8_t *directory_track =
	    fs->image_data + cocofs_track_to_offset(COCOFS_DIR_TRACK);
	fs->granule_map =
	    directory_track + cocofs_sector_to_offset(GMAP_SECTOR);
	fs->directory = (struct cocofs_dirent *)
	    (directory_track +
	     cocofs_sector_to_offset(COCOFS_DIR_TRACK_FIRST_SEC));

	fs->fd = fd;

	return fs;
}

static void
cocofs_free(struct cocofs *fs)
{
	free(fs->image_data);
	free(fs);
}

static struct cocofs *
cocofs_format(int fd)
{
	struct cocofs *fs = cocofs_alloc(fd);

	/*
	 * Looking at several CoCo disk images, it appears that simply
	 * initializing the entire disk to 0xff's is sufficient. That
	 * marks all of the granule map entries as "FREE", and it appears
	 * to be what a free directory entry looks like, as well.
	 *
	 * As far as I can tell, CoCo disks to not have the separate
	 * granule allocation table in sector 1 of the directory track.
	 */
	memset(fs->image_data, 0xff, COCOFS_TOTALSIZE);
	fs->free_granules = COCOFS_NGRANULES;

	return fs;
}

static struct cocofs *
cocofs_load(int fd)
{
	struct cocofs *fs = cocofs_alloc(fd);
	ssize_t rv;
	int i;

	/* Read in the image. */
	rv = cocofs_pread(fd, fs->image_data, COCOFS_TOTALSIZE, 0);
	if (rv == -1) {
		fprintf(stderr, "ERROR: unable to read image: %s\n",
		    strerror(errno));
		cocofs_free(fs);
		return NULL;
	}
	if (rv < COCOFS_TOTALSIZE) {
		fprintf(stderr, "WARNING: read only %ld byte%s of image data\n",
		    (long)rv, plural(rv));
	}

	for (i = 0; i < COCOFS_NGRANULES; i++) {
		if (fs->granule_map[i] == GMAP_FREE) {
			fs->free_granules++;
		}
	}

	return fs;
}

static bool
cocofs_save(const struct cocofs *fs)
{
	ssize_t rv;

	rv = cocofs_pwrite(fs->fd, fs->image_data, COCOFS_TOTALSIZE, 0);
	if (rv != COCOFS_TOTALSIZE) {
		fprintf(stderr, "ERROR: unable to write image data: %s\n",
		    strerror(errno));
		return false;
	}
	return true;
}

static void
cocofs_close(struct cocofs *fs)
{
	close(fs->fd);
	cocofs_free(fs);
}

static struct cocofs_dirent *
cocofs_lookup_raw(struct cocofs *fs, const char *name, const char *ext)
{
	struct cocofs_dirent *dir;
	unsigned int i;

	for (i = 0; i < COCOFS_DIR_TRACK_NENTRIES; i++) {
		dir = &fs->directory[i];
		if (dir->d_type == COCOFS_DIRENT_TYPE_FREE) {
			continue;
		}
		if (memcmp(dir->d_name, name, sizeof(dir->d_name)) != 0) {
			continue;
		}
		if (memcmp(dir->d_ext, ext, sizeof(dir->d_ext)) != 0) {
			continue;
		}
		break;
	}

	if (i < COCOFS_DIR_TRACK_NENTRIES) {
		return dir;
	}

	return NULL;
}

static struct cocofs_dirent *
cocofs_lookup(struct cocofs *fs, const char *lookup)
{
	char name[8], ext[3];

	if (! cocofs_conv_name(lookup, name, ext)) {
		return NULL;
	}

	return cocofs_lookup_raw(fs, name, ext);
}

static void
cocofs_stat(const struct cocofs *fs, const struct cocofs_dirent *dir,
    struct cocofs_stat *st)
{
	uint32_t size = 0;
	uint32_t last_nsec = 0;
	unsigned int loopcnt;
	uint8_t g, gn;

	for (loopcnt = 0, g = dir->d_first_granule;
	     loopcnt <= COCOFS_NGRANULES;
	     loopcnt++, g = gn) {
		gn = fs->granule_map[g];
		if (! gmap_entry_is_valid(gn) ||
		    gn == GMAP_FREE) {
			break;
		}

		if (GMAP_IS_LAST(gn)) {
			last_nsec = GMAP_LAST_NSEC(gn);
			break;
		}

		size += (COCOFS_SEC_PER_GRANULE * COCOFS_BYTES_PER_SEC);
	}

	if (last_nsec) {
		unsigned int lastsec_bytes;

		size += last_nsec * COCOFS_BYTES_PER_SEC;

		lastsec_bytes = cocofs_dir_lastbytes(dir->d_last_bytes);
		if (lastsec_bytes > COCOFS_BYTES_PER_SEC) {
			lastsec_bytes = COCOFS_BYTES_PER_SEC;
		}

		size -= (COCOFS_BYTES_PER_SEC - lastsec_bytes);
	}

	st->st_size = size;

	int i;
	memcpy(st->st_name, dir->d_name, sizeof(st->st_name));
	st->st_name[8] = '\0';
	for (i = 7; i >= 0 && st->st_name[i] == ' '; i--) {
		st->st_name[i] = '\0';
	}

	memcpy(st->st_ext, dir->d_ext, sizeof(st->st_ext));
	st->st_ext[3] = '\0';
	for (i = 2; i >= 0 && st->st_ext[i] == ' '; i--) {
		st->st_ext[i] = '\0';
	}

	st->st_type = dir->d_type;
	st->st_encoding = dir->d_encoding;
}

static void
cocofs_print_stat(const struct cocofs_stat *st)
{
	printf("  %-8s   %-3s  %6u byte%-1s (%s, %s)\n",
	    st->st_name, st->st_ext, st->st_size, plural(st->st_size),
	    cocofs_dir_type(st->st_type),
	    cocofs_dir_encoding(st->st_encoding));
}

static void
cocofs_enumerate_directory(struct cocofs *fs, bool do_dump)
{
	struct cocofs_dirent *dir;
	struct cocofs_stat st;
	int nfiles = 0, gi;
	unsigned int di;
	unsigned int free_granules = COCOFS_NGRANULES;
	unsigned int loopcnt;
	unsigned int lastbytes;
	uint8_t g, gn;

	uint8_t gmap_shadow[COCOFS_NGRANULES];
	memset(gmap_shadow, 0xff, sizeof(gmap_shadow));

	printf("\n");
	for (di = 0; di < COCOFS_DIR_TRACK_NENTRIES; di++) {
		dir = &fs->directory[di];
		if (dir->d_type > COCOFS_DIRENT_TYPE_TEXT) {
			if (do_dump) {
				printf("%2d: entry type 0x%02x, skipping.\n",
				    di, dir->d_type);
			}
			continue;
		}

		nfiles++;
		cocofs_stat(fs, dir, &st);
		cocofs_print_stat(&st);

		if (! do_dump) {
			continue;
		}

		/* Chase the granule list for this file. */
		for (gi = 0, g = dir->d_first_granule, loopcnt = 0;
		     loopcnt <= COCOFS_NGRANULES;
		     gi++, g = gn) {
			if (g >= COCOFS_NGRANULES) {
				printf("\tINVALID GRANULE #%d: %d\n",
				    gi, g);
				break;
			}
			if (gmap_shadow[g] != 0xff) {
				printf("\tGRANULE %d ALREADY ALLOCATED "
				       "TO FILE %d\n", g, di);
			} else {
				assert(free_granules != 0);
				free_granules--;
				gmap_shadow[g] = di;
			}
			gn = fs->granule_map[g];
			if (! gmap_entry_is_valid(gn)) {
				printf("\tINVALID GRANULE MAP ENTRY "
				       "%2d: %d -> 0x%02x\n", gi, g, gn);
				break;
			}
			if (GMAP_IS_LAST(gn)) {
				printf("\tGranule %2d: %d (last, nsec=%d)\n",
				    gi, g, GMAP_LAST_NSEC(gn));
				break;
			} else {
				printf("\tGranule %2d: %d\n",
				    gi, g);
			}
			g = gn;
		}
		if (loopcnt > COCOFS_NGRANULES) {
			printf("\tGRANULE LIST CYCLE DETECTED\n");
		}
		lastbytes = cocofs_dir_lastbytes(dir->d_last_bytes);
		printf("\tBytes in last sector: %u (0x%02x 0x%02x)\n",
		    lastbytes,
		    dir->d_last_bytes[0], dir->d_last_bytes[1]);

	}

	if (nfiles) {
		printf("\n");
	}

	if (! do_dump) {
		free_granules = fs->free_granules;
	}
	printf("%d file%s, %u granule%s (%u bytes) free\n",
	    nfiles, plural(nfiles),
	    free_granules, plural(free_granules),
	    free_granules * COCOFS_SEC_PER_GRANULE * COCOFS_BYTES_PER_SEC);
	if (do_dump && free_granules != fs->free_granules) {
		printf("WARNING: FREE GRANULES LOADED %u != COMPUTED %u\n",
		    fs->free_granules, free_granules);
	}
}

static bool
cocofs_rm(struct cocofs *fs, struct cocofs_dirent *dir)
{
	unsigned int gi;
	uint8_t g, gn;

	for (gi = 0, g = dir->d_first_granule;; gi++, g = gn) {
		/* No need to detect cycles here, because we'll break them. */

		if (g >= COCOFS_NGRANULES) {
			fprintf(stderr, "INVALID GRANULE #%d: %d\n",
			    gi, g);
			return false;
		}

		gn = fs->granule_map[g];
		if (! gmap_entry_is_valid(gn) ||
		    gn == GMAP_FREE) {
			printf("INVALID GRANULE MAP ENTRY "
			       "%2d: %d -> 0x%02x\n", gi, g, gn);
			return false;
		}

		fs->granule_map[g] = GMAP_FREE;
		fs->free_granules++;
		if (GMAP_IS_LAST(gn)) {
			break;
		}
	}

	memset(dir, 0xff, sizeof(*dir));

	return true;
}

static bool
cocofs_copyout(const struct cocofs *fs, const struct cocofs_dirent *dir,
    const char *outfname)
{
	unsigned int loopcnt;
	unsigned int last_nsec = 0;
	unsigned int last_nbytes;
	unsigned int offset;
	unsigned int gi;
	ssize_t rv;
	uint8_t g, gn;
	int outfd;

	outfd = open(outfname, O_WRONLY | O_CREAT, 0644);
	if (outfd == -1) {
		fprintf(stderr, "unable to open output file %s: %s\n",
		    outfname, strerror(errno));
		return false;
	}

	for (gi = 0, g = dir->d_first_granule, loopcnt = 0;; gi++, g = gn) {
		if (loopcnt > COCOFS_NGRANULES) {
			fprintf(stderr, "GRANULE MAP CYCLE DETECTED\n");
			goto bad;
		}

		if (g >= COCOFS_NGRANULES) {
			fprintf(stderr, "INVALID GRANULE #%d: %d\n",
			    gi, g);
			goto bad;
		}

		gn = fs->granule_map[g];
		if (! gmap_entry_is_valid(gn) ||
		    gn == GMAP_FREE) {
			printf("INVALID GRANULE MAP ENTRY "
			       "%2d: %d -> 0x%02x\n", gi, g, gn);
			goto bad;
		}
		if (GMAP_IS_LAST(gn)) {
			last_nsec = GMAP_LAST_NSEC(gn);
			break;
		} else {
			/* Write out a full granule. */
			offset = cocofs_granule_to_offset(g);
			rv = write(outfd, fs->image_data + offset,
			    COCOFS_BYTES_PER_GRANULE);
			if (rv != COCOFS_BYTES_PER_GRANULE) {
				fprintf(stderr, "error writing %s: %s\n",
				    outfname, strerror(errno));
				goto bad;
			}
		}
		g = gn;
	}

	if (last_nsec < 1 || last_nsec > COCOFS_SEC_PER_GRANULE) {
		fprintf(stderr, "UNEXPECTED LAST_NSEC %u\n", last_nsec);
		goto bad;
	}
	last_nbytes = cocofs_dir_lastbytes(dir->d_last_bytes);
	if (last_nbytes > COCOFS_BYTES_PER_SEC) {
		fprintf(stderr, "UNEXPECTED LAST_BYTES %u, CLAMPING TO %d\n",
		    last_nbytes, COCOFS_BYTES_PER_SEC);
		last_nbytes = COCOFS_BYTES_PER_SEC;
	}
	last_nbytes = (last_nsec * COCOFS_BYTES_PER_SEC) -
	    (COCOFS_BYTES_PER_SEC - last_nbytes);
	
	/* Write out the trailing bytes in the last granule. */
	offset = cocofs_granule_to_offset(g);
	rv = write(outfd, fs->image_data + offset, last_nbytes);
	if (rv != last_nbytes) {
		fprintf(stderr, "error writing %s: %s\n",
		    outfname, strerror(errno));
		goto bad;
	}

	close(outfd);
	return true;

 bad:
	close(outfd);
	return false;
}

static unsigned int
cocofs_galloc(struct cocofs *fs, unsigned int last)
{
	unsigned int g, next, loopcnt;

	for (loopcnt = 0, g = last; loopcnt <= COCOFS_NGRANULES;
	     loopcnt++, g = next) {
		if (fs->granule_map[g] == GMAP_FREE) {
			fs->granule_map[g] = GMAP_ALLOCATED;
			fs->free_granules--;
			return g;
		}
		next = g + 1;
		if (next == COCOFS_NGRANULES) {
			next = 0;
		}
	}
	assert(loopcnt <= COCOFS_NGRANULES);
	abort();
}

static bool
cocofs_copyin(struct cocofs *fs, const char *infile, const char name[8],
    const char ext[3], uint8_t type, uint8_t enc)
{
	struct cocofs_dirent *dir = NULL;
	struct stat sb;
	int infd;
	unsigned int granules_needed;
	unsigned int orig_free_granules;
	unsigned int g, gi;
	unsigned int i;
	uint8_t orig_gmap[COCOFS_NGRANULES];
	uint8_t glist[COCOFS_NGRANULES];

	/*
	 * Make a backup copy of the granule map so we can roll back,
	 * if necesary.
	 */
	memcpy(orig_gmap, fs->granule_map, sizeof(orig_gmap));
	orig_free_granules = fs->free_granules;

	/*
	 * Initialize the list we use to hold allocate granules.
	 */
	memset(glist, 0xff, sizeof(glist));

	infd = open(infile, O_RDONLY);
	if (infd == -1) {
		fprintf(stderr,
		    "unable to open %s: %s\n", infile, strerror(errno));
		return false;
	}

	if (fstat(infd, &sb) == -1) {
		fprintf(stderr,
		    "unable to stat %s: %s\n", infile, strerror(errno));
		goto bad;
	}

	if ((unsigned long long)sb.st_size >
	    fs->free_granules * COCOFS_BYTES_PER_GRANULE) {
		fprintf(stderr,
		    "%s: %s\n", infile, strerror(ENOSPC));
		goto bad;
	}

	granules_needed = sb.st_size / COCOFS_BYTES_PER_GRANULE;
	if (sb.st_size % COCOFS_BYTES_PER_GRANULE) {
		granules_needed++;
	}
	assert(granules_needed <= fs->free_granules);

	/*
	 * Find a free directory entry.
	 */
	for (i = 0; i < COCOFS_DIR_TRACK_NENTRIES; i++) {
		dir = &fs->directory[i];
		if (dir->d_type == COCOFS_DIRENT_TYPE_FREE) {
			break;
		}
	}
	if (i == COCOFS_DIR_TRACK_NENTRIES) {
		fprintf(stderr,
		    "No directory entries available for %s\n", infile);
		dir = NULL;
		goto bad;
	}

	/*
	 * Our allocation strategy is really simple.  We start in the
	 * middle of the disk (where the directory is; all file reads
	 * need to start there), and allocate one block at a time.  We
	 * pass in the starting point each time to try and allocate as
	 * contiguously as possible.  We are guaranteed that this will
	 * succeed, as we have already checked that there are enough
	 * free granules to satisfy the request.
	 */
	unsigned int last;
	for (last = COCOFS_NGRANULES / 2, gi = 0; gi < granules_needed;
	     last = g, gi++) {
		g = cocofs_galloc(fs, last);
		glist[gi] = g;
	}

	/*
	 * We now have our list of granules for the new file, and
	 * each granule has been marked as allocated in the Granule
	 * Map.
	 */
	unsigned int resid, cursz;
	ssize_t rv;
	uint8_t *buf;
	for (gi = 0, resid = (unsigned int)sb.st_size;
	     resid != 0; gi++, resid -= cursz) {
		cursz = resid;
		if (cursz > COCOFS_BYTES_PER_GRANULE) {
			cursz = COCOFS_BYTES_PER_GRANULE;
		}

		g = glist[gi];
		assert(fs->granule_map[g] == GMAP_ALLOCATED);
		buf = fs->image_data + cocofs_granule_to_offset(g);
		rv = cocofs_pread(infd, buf, cursz,
				  gi * COCOFS_BYTES_PER_GRANULE);
		if (rv != cursz) {
			fprintf(stderr, "failed to read %s\n", infile);
			goto bad;
		}

		if (resid <= COCOFS_BYTES_PER_GRANULE) {
			/*
			 * This is the last granule of the file.  Set
			 * the trailing-bytes values.
			 */
			uint8_t nsec = resid / COCOFS_BYTES_PER_SEC;
			if (resid % COCOFS_BYTES_PER_SEC) {
				nsec++;
			}
			assert(gi + 1 == granules_needed);
			assert(nsec <= COCOFS_SEC_PER_GRANULE);
			fs->granule_map[g] = GMAP_LAST | nsec;

			unsigned int lastbytes = resid % COCOFS_BYTES_PER_SEC;
			if (lastbytes == 0) {
				lastbytes = COCOFS_BYTES_PER_SEC;
			}

			/* Zero out the remainder of the granule. */
			unsigned int trailing =
			    COCOFS_BYTES_PER_GRANULE - resid;
			if (trailing != 0) {
				memset(&buf[resid], 0, trailing);
			}

			/* Set the directory entry. */
			memcpy(dir->d_name, name, sizeof(dir->d_name));
			memcpy(dir->d_ext, ext, sizeof(dir->d_ext));
			dir->d_type = type;
			dir->d_encoding = enc;
			dir->d_first_granule = glist[0];
			cocofs_dir_set_lastbytes(lastbytes, dir->d_last_bytes);
		} else {
			/*
			 * Point the current Granule Map entry at the
			 * next granule.
			 */
			assert(gi + 1 < granules_needed);
			fs->granule_map[g] = glist[gi + 1];
		}
	}

	/* All done. */
	close(infd);
	return true;

 bad:
	/* Back out changes to the directory and granule map. */
	if (dir != NULL) {
		memset(dir, 0xff, sizeof(*dir));
	}
	memcpy(fs->granule_map, orig_gmap, sizeof(orig_gmap));
	fs->free_granules = orig_free_granules;
	close(infd);
	return false;
}

static const char *myname = "cocofs";

static void
set_myname(const char *argv0)
{
	char *cp;

	/* Unix path. */
	cp = strrchr(argv0, '/');
	if (cp == NULL) {
		/* Windows path. */
		cp = strrchr(argv0, '\\');
	}
	if (cp == NULL) {
		/* stick with the default. */
		return;
	}
	myname = ++cp;	/* advance past the path delimeter */
}

static int
usage(void)
{
	fprintf(stderr, "usage: %s <image> dump\n", myname);
	fprintf(stderr, "       %s <image> format\n", myname);
	fprintf(stderr, "       %s <image> ls [file1 [file2 [...]]]\n", myname);
	fprintf(stderr, "       %s <image> rm file1 [file2 [...]]\n", myname);
	fprintf(stderr, "       %s <image> copyin file1 [file2 [...]]\n",
	    myname);
	fprintf(stderr, "       %s <image> copyout file1 [file2 [...]]\n",
	    myname);

	return EXIT_FAILURE;
}

static int
cmd_dump(struct cocofs *fs, int argc, char *argv[])
{
	if (argc != 0) {
		return usage();
	}
	(void)argv;

	cocofs_enumerate_directory(fs, true);

	return EXIT_SUCCESS;
}

static int
cmd_format(struct cocofs *fs, int argc, char *argv[])
{
	if (argc != 0) {
		return usage();
	}
	(void)argv;

	/* Caller formatted a new image for us. */

	return cocofs_save(fs) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int
cmd_ls(struct cocofs *fs, int argc, char *argv[])
{
	if (argc == 0) {
		cocofs_enumerate_directory(fs, false);
		return EXIT_SUCCESS;
	}

	struct cocofs_dirent *dir;
	struct cocofs_stat st;
	int retval = EXIT_SUCCESS;
	int i;
	for (i = 0; i < argc; i++) {
		dir = cocofs_lookup(fs, argv[i]);
		if (dir == NULL) {
			fprintf(stderr, "%s: %s\n",
			    argv[i], strerror(ENOENT));
			retval = EXIT_FAILURE;
			continue;
		}
		cocofs_stat(fs, dir, &st);
		cocofs_print_stat(&st);
	}
	return retval;
}

static int
cmd_rm(struct cocofs *fs, int argc, char *argv[])
{
	if (argc == 0) {
		return usage();
	}

	struct cocofs_dirent *dir;
	int retval = EXIT_SUCCESS;
	int i;
	for (i = 0; i < argc; i++) {
		dir = cocofs_lookup(fs, argv[i]);
		if (dir == NULL) {
			fprintf(stderr, "%s: %s\n",
			    argv[i], strerror(ENOENT));
			retval = EXIT_FAILURE;
			continue;
		}
		if (! cocofs_rm(fs, dir)) {
			retval = EXIT_FAILURE;
			break;
		}
		if (! cocofs_save(fs)) {
			retval = EXIT_FAILURE;
			break;
		}
	}
	return retval;
}

static int
cmd_copyout(struct cocofs *fs, int argc, char *argv[])
{
	if (argc == 0) {
		return usage();
	}

	struct cocofs_dirent *dir;
	struct cocofs_stat st;
	char outfname[8+1+3+1];	/* 88888888.333\0 */
	int retval = EXIT_SUCCESS;
	int i;
	for (i = 0; i < argc; i++) {
		dir = cocofs_lookup(fs, argv[i]);
		if (dir == NULL) {
			fprintf(stderr, "%s: %s\n",
			    argv[i], strerror(ENOENT));
			retval = EXIT_FAILURE;
			continue;
		}
		cocofs_stat(fs, dir, &st);
		/*
		 * N.B. cocofs_stat() guarantees that st.st_name will
		 * never exceed 8 bytes and that st.st_ext will never
		 * exceed 3 bytes.  This means that the 13 byte buffer
		 * allocated above will be sufficient for all possible
		 * inputs, and thus strcpy() and sprintf() will be safe
		 * to use.  (This improves the portability of the code.)
		 */
		if (st.st_ext[0] == '\0') {
			/* No extension. */
			strcpy(outfname, st.st_name);
		} else {
			sprintf(outfname, "%s.%s", st.st_name, st.st_ext);
		}
		if (! cocofs_copyout(fs, dir, outfname)) {
			retval = EXIT_FAILURE;
		}
	}

	return retval;
}

static int
cmd_copyin(struct cocofs *fs, int argc, char *argv[])
{
	if (argc == 0) {
		return usage();
	}

	struct cocofs_dirent *dir;
	char name[8], ext[3];
	uint8_t type, enc;
	int retval = EXIT_SUCCESS;
	int i;
	for (i = 0; i < argc; i++) {
		if (! cocofs_parse_fname(argv[i], name, ext, &type, &enc)) {
			/* Error message already displayed. */
			retval = EXIT_FAILURE;
			continue;
		}

		/* Make sure this file does not already exist. */
		dir = cocofs_lookup_raw(fs, name, ext);
		if (dir != NULL) {
			fprintf(stderr, "%s: %s\n", argv[1], strerror(EEXIST));
			retval = EXIT_FAILURE;
			continue;
		}
		if (! cocofs_copyin(fs, argv[i], name, ext, type, enc)) {
			retval = EXIT_FAILURE;
			break;
		}
		if (! cocofs_save(fs)) {
			retval = EXIT_FAILURE;
			break;
		}
	}

	return retval;
}

const struct {
	const char *verb;
	int oflags;
	int (*func)(struct cocofs *, int, char *[]);
} cmdtab[] = {
	{
		"dump",
		O_RDONLY,
		cmd_dump,
	},
	{
		"ls",
		O_RDONLY,
		cmd_ls,
	},
	{
		"rm",
		O_RDWR,
		cmd_rm,
	},
	{
		"format",
		O_WRONLY | O_CREAT | O_TRUNC,
		cmd_format,
	},
	{
		"copyout",
		O_RDONLY,
		cmd_copyout,
	},
	{
		"copyin",
		O_RDWR,
		cmd_copyin,
	},

	{
		NULL,
	}
};

int
main(int argc, char *argv[])
{
	struct cocofs *fs;
	int fd, cmd;

	/* Skip over argv[0]. */
	assert(argc > 0);
	set_myname(argv[0]);
	argc--;
	argv++;

	/* Must have at least 2 arguments. */
	if (argc < 2) {
		exit(usage());
	}

	/* Find the command (argv[1]). */
	for (cmd = 0; cmdtab[cmd].verb != NULL; cmd++) {
		if (strcmp(cmdtab[cmd].verb, argv[1]) == 0) {
			break;
		}
	}
	if (cmdtab[cmd].verb == NULL) {
		exit(usage());
	}

	/* Open the image (name in argv[0]). */
	fd = open(argv[0], cmdtab[cmd].oflags, 0644);
	if (fd == -1) {
		fprintf(stderr, "ERROR: failed to open '%s': %s\n",
		    argv[0], strerror(errno));
		exit(EXIT_FAILURE);
	}

	/* O_CREAT implies "create new". */
	fs = (cmdtab[cmd].oflags & O_CREAT) ? cocofs_format(fd)
					    : cocofs_load(fd);
	if (fs == NULL) {
		exit(EXIT_FAILURE);
	}

	/* Advance past the mandatory arguments. */
	argc -= 2;
	argv += 2;

	/* Run the command. */
	int eval = (*cmdtab[cmd].func)(fs, argc, argv);

	cocofs_close(fs);

	exit(eval);
}
