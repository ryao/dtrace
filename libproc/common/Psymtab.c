/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)Psymtab.c	1.45	07/07/12 SMI"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <memory.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <limits.h>
#include <libgen.h>
#include <zone.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/systeminfo.h>
#include <sys/sysmacros.h>

#include "libproc.h"
#include "Pcontrol.h"
#include "Putil.h"
#include "Psymtab_machelf.h"

rd_agent_t *rd_new(struct ps_prochandle *php, int pid);

static file_info_t *build_map_symtab(struct ps_prochandle *, map_info_t *);
static map_info_t *exec_map(struct ps_prochandle *);
static map_info_t *object_to_map(struct ps_prochandle *, Lmid_t, const char *);
static map_info_t *object_name_to_map(struct ps_prochandle *,
	Lmid_t, const char *);
static GElf_Sym *sym_by_name(sym_tbl_t *, const char *, GElf_Sym *, uint_t *);
static int read_ehdr32(struct ps_prochandle *, Elf32_Ehdr *, uint_t *,
    uintptr_t);
#ifdef _LP64
static int read_ehdr64(struct ps_prochandle *, Elf64_Ehdr *, uint_t *,
    uintptr_t);
#endif

#define	DATA_TYPES	\
	((1 << STT_OBJECT) | (1 << STT_FUNC) | \
	(1 << STT_COMMON) | (1 << STT_TLS))
#define	IS_DATA_TYPE(tp)	(((1 << (tp)) & DATA_TYPES) != 0)

#define	MA_RWX	(MA_READ | MA_WRITE | MA_EXEC)

typedef enum {
	PRO_NATURAL,
	PRO_BYADDR,
	PRO_BYNAME
} pr_order_t;

static int
addr_cmp(const void *aa, const void *bb)
{
	uintptr_t a = *((uintptr_t *)aa);
	uintptr_t b = *((uintptr_t *)bb);

	if (a > b)
		return (1);
	if (a < b)
		return (-1);
	return (0);
}

/*
 * This function creates a list of addresses for a load object's sections.
 * The list is in ascending address order and alternates start address
 * then end address for each section we're interested in. The function
 * returns a pointer to the list, which must be freed by the caller.
 */
static uintptr_t *
get_saddrs(struct ps_prochandle *P, uintptr_t ehdr_start, uint_t *n)
{
	uintptr_t a, addr, *addrs, last = 0;
	uint_t i, naddrs = 0, unordered = 0;

	if (P->status.pr_dmodel == PR_MODEL_ILP32) {
		Elf32_Ehdr ehdr;
		Elf32_Phdr phdr;
		uint_t phnum;

		if (read_ehdr32(P, &ehdr, &phnum, ehdr_start) != 0)
			return (NULL);

		addrs = malloc(sizeof (uintptr_t) * phnum * 2);
		a = ehdr_start + ehdr.e_phoff;
		for (i = 0; i < phnum; i++, a += ehdr.e_phentsize) {
			if (Pread(P, &phdr, sizeof (phdr), a) !=
			    sizeof (phdr)) {
				free(addrs);
				return (NULL);
			}
			if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0)
				continue;

			addr = phdr.p_vaddr;
			if (ehdr.e_type == ET_DYN)
				addr += ehdr_start;
			if (last > addr)
				unordered = 1;
			addrs[naddrs++] = addr;
			addrs[naddrs++] = last = addr + phdr.p_memsz - 1;
		}
#ifdef _LP64
#if defined(linux)
	} else if (P->status.pr_dmodel == PR_MODEL_LP64) {
#else
	} else {
#endif
		Elf64_Ehdr ehdr;
		Elf64_Phdr phdr;
		uint_t phnum;

		if (read_ehdr64(P, &ehdr, &phnum, ehdr_start) != 0)
			return (NULL);

		addrs = malloc(sizeof (uintptr_t) * phnum * 2);
		a = ehdr_start + ehdr.e_phoff;
		for (i = 0; i < phnum; i++, a += ehdr.e_phentsize) {
			if (Pread(P, &phdr, sizeof (phdr), a) !=
			    sizeof (phdr)) {
				free(addrs);
				return (NULL);
			}
			if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0)
				continue;

			addr = phdr.p_vaddr;
			if (ehdr.e_type == ET_DYN)
				addr += ehdr_start;
			if (last > addr)
				unordered = 1;
			addrs[naddrs++] = addr;
			addrs[naddrs++] = last = addr + phdr.p_memsz - 1;
		}
#endif
	} else {
		printf("Internal error: %s:%s pr_dmodel is incorrect\n",
			__FILE__, __func__);
		abort();
	}

	if (unordered)
		qsort(addrs, naddrs, sizeof (uintptr_t), addr_cmp);

	*n = naddrs;
	return (addrs);
}

/*
 * Allocation function for a new file_info_t
 */
file_info_t *
file_info_new(struct ps_prochandle *P, map_info_t *mptr)
{
	file_info_t *fptr;
	map_info_t *mp;
	uintptr_t mstart, mend, sstart, send;
	uint_t i;

	if ((fptr = calloc(1, sizeof (file_info_t))) == NULL)
		return (NULL);

	list_link(fptr, &P->file_head);
	(void) strcpy(fptr->file_pname, mptr->map_pmap.pr_mapname);
	mptr->map_file = fptr;
	fptr->file_ref = 1;
	fptr->file_fd = -1;
	P->num_files++;

	/*
	 * To figure out which map_info_t instances correspond to the mappings
	 * for this load object we try to obtain the start and end address
	 * for each section of our in-memory ELF image. If successful, we
	 * walk down the list of addresses and the list of map_info_t
	 * instances in lock step to correctly find the mappings that
	 * correspond to this load object.
	 */
	if ((fptr->file_saddrs = get_saddrs(P, mptr->map_pmap.pr_vaddr,
	    &fptr->file_nsaddrs)) == NULL)
		return (fptr);

	mp = P->mappings;
	i = 0;
	while (mp < P->mappings + P->map_count && i < fptr->file_nsaddrs) {

		/* Calculate the start and end of the mapping and section */
		mstart = mp->map_pmap.pr_vaddr;
		mend = mp->map_pmap.pr_vaddr + mp->map_pmap.pr_size;
		sstart = fptr->file_saddrs[i];
		send = fptr->file_saddrs[i + 1];

		if (mend <= sstart) {
			/* This mapping is below the current section */
			mp++;
		} else if (mstart >= send) {
			/* This mapping is above the current section */
			i += 2;
		} else {
			/* This mapping overlaps the current section */
			if (mp->map_file == NULL) {
				p_dprintf("file_info_new: associating "
				    "segment at %p\n",
				    (void *)mp->map_pmap.pr_vaddr);
				mp->map_file = fptr;
				fptr->file_ref++;
			} else {
				p_dprintf("file_info_new: segment at %p "
				    "already associated with %s\n",
				    (void *)mp->map_pmap.pr_vaddr,
				    (mp == mptr ? "this file" :
				    mp->map_file->file_pname));
			}
			mp++;
		}
	}

	return (fptr);
}

/*
 * Deallocation function for a file_info_t
 */
static void
file_info_free(struct ps_prochandle *P, file_info_t *fptr)
{
	if (--fptr->file_ref == 0) {
		list_unlink(fptr);
		if (fptr->file_symtab.sym_elf) {
			(void) elf_end(fptr->file_symtab.sym_elf);
			free(fptr->file_symtab.sym_elfmem);
		}
		if (fptr->file_symtab.sym_byname)
			free(fptr->file_symtab.sym_byname);
		if (fptr->file_symtab.sym_byaddr)
			free(fptr->file_symtab.sym_byaddr);

		if (fptr->file_dynsym.sym_elf) {
			(void) elf_end(fptr->file_dynsym.sym_elf);
			free(fptr->file_dynsym.sym_elfmem);
		}
		if (fptr->file_dynsym.sym_byname)
			free(fptr->file_dynsym.sym_byname);
		if (fptr->file_dynsym.sym_byaddr)
			free(fptr->file_dynsym.sym_byaddr);

		if (fptr->file_lo)
			free(fptr->file_lo);
		if (fptr->file_lname) {
			free(fptr->file_lname);
			fptr->file_lname = NULL;
		}
		if (fptr->file_elf)
			(void) elf_end(fptr->file_elf);
		if (fptr->file_elfmem != NULL)
			free(fptr->file_elfmem);
		if (fptr->file_fd >= 0)
			(void) close(fptr->file_fd);
		if (fptr->file_ctfp) {
			ctf_close(fptr->file_ctfp);
			free(fptr->file_ctf_buf);
		}
		if (fptr->file_saddrs)
			free(fptr->file_saddrs);
		free(fptr);
		P->num_files--;
	}
}

/*
 * Deallocation function for a map_info_t
 */
static void
map_info_free(struct ps_prochandle *P, map_info_t *mptr)
{
	file_info_t *fptr;

	if ((fptr = mptr->map_file) != NULL) {
		if (fptr->file_map == mptr)
			fptr->file_map = NULL;
		file_info_free(P, fptr);
	}
	if (P->execname && mptr == P->map_exec) {
		free(P->execname);
		P->execname = NULL;
	}
	if (P->auxv && (mptr == P->map_exec || mptr == P->map_ldso)) {
		free(P->auxv);
		P->auxv = NULL;
		P->nauxv = 0;
	}
	if (mptr == P->map_exec)
		P->map_exec = NULL;
	if (mptr == P->map_ldso)
		P->map_ldso = NULL;
}

/*
 * Call-back function for librtld_db to iterate through all of its shared
 * libraries.  We use this to get the load object names for the mappings.
 */
static int
map_iter(const rd_loadobj_t *lop, void *cd)
{
	char buf[PATH_MAX];
	struct ps_prochandle *P = cd;
	map_info_t *mptr;
	file_info_t *fptr;

	p_dprintf("encountered rd object at %p\n", (void *)lop->rl_base);

	if ((mptr = Paddr2mptr(P, lop->rl_base)) == NULL) {
		p_dprintf("map_iter: base address doesn't match any mapping\n");
		return (1); /* Base address does not match any mapping */
	}

	if ((fptr = mptr->map_file) == NULL &&
	    (fptr = file_info_new(P, mptr)) == NULL) {
		p_dprintf("map_iter: failed to allocate a new file_info_t\n");
		return (1); /* Failed to allocate a new file_info_t */
	}

	if ((fptr->file_lo == NULL) &&
	    (fptr->file_lo = malloc(sizeof (rd_loadobj_t))) == NULL) {
		p_dprintf("map_iter: failed to allocate rd_loadobj_t\n");
		file_info_free(P, fptr);
		return (1); /* Failed to allocate rd_loadobj_t */
	}

	fptr->file_map = mptr;
	*fptr->file_lo = *lop;

	fptr->file_lo->rl_plt_base = fptr->file_plt_base;
	fptr->file_lo->rl_plt_size = fptr->file_plt_size;

	if (fptr->file_lname) {
		free(fptr->file_lname);
		fptr->file_lname = NULL;
	}

#if defined(linux)
	fptr->file_lname = strdup((char *) lop->rl_nameaddr);
/*printf("filename=%s\n", fptr->file_lname);*/
#else
	if (Pread_string(P, buf, sizeof (buf), lop->rl_nameaddr) > 0) {
		if ((fptr->file_lname = strdup(buf)) != NULL)
			fptr->file_lbase = basename(fptr->file_lname);
	} else {
		p_dprintf("map_iter: failed to read string at %p\n",
		    (void *)lop->rl_nameaddr);
	}
#endif

	p_dprintf("loaded rd object %s lmid %lx\n",
	    fptr->file_lname ? fptr->file_lname : "<NULL>", lop->rl_lmident);
	return (1);
}

static void
map_set(struct ps_prochandle *P, map_info_t *mptr, const char *lname)
{
	file_info_t *fptr;

	if ((fptr = mptr->map_file) == NULL &&
	    (fptr = file_info_new(P, mptr)) == NULL)
		return; /* Failed to allocate a new file_info_t */

	fptr->file_map = mptr;

	if ((fptr->file_lo == NULL) &&
	    (fptr->file_lo = malloc(sizeof (rd_loadobj_t))) == NULL) {
		file_info_free(P, fptr);
		return; /* Failed to allocate rd_loadobj_t */
	}

	(void) memset(fptr->file_lo, 0, sizeof (rd_loadobj_t));
	fptr->file_lo->rl_base = mptr->map_pmap.pr_vaddr;
	fptr->file_lo->rl_bend =
	    mptr->map_pmap.pr_vaddr + mptr->map_pmap.pr_size;

	fptr->file_lo->rl_plt_base = fptr->file_plt_base;
	fptr->file_lo->rl_plt_size = fptr->file_plt_size;

	if (fptr->file_lname == NULL &&
	    (fptr->file_lname = strdup(lname)) != NULL)
		fptr->file_lbase = basename(fptr->file_lname);
}

static void
load_static_maps(struct ps_prochandle *P)
{
	map_info_t *mptr;

	/*
	 * Construct the map for the a.out.
	 */
	if ((mptr = object_name_to_map(P, PR_LMID_EVERY, PR_OBJ_EXEC)) != NULL) {
		char buf[128];
		char buf2[BUFSIZ];
		int	n;
		snprintf(buf, sizeof buf, "/proc/%d/exe", P->pid);
		if ((n = readlink(buf, buf2, sizeof buf2 - 1)) != 0) {
			buf2[n] = '\0';
			map_set(P, mptr, buf2);
		} else
			map_set(P, mptr, "a.out");
	}

	/*
	 * If the dynamic linker exists for this process,
	 * construct the map for it.
	 */
	if (Pgetauxval(P, AT_BASE) != -1L &&
	    (mptr = object_name_to_map(P, PR_LMID_EVERY, PR_OBJ_LDSO)) != NULL)
		map_set(P, mptr, "ld.so.1");
}

/*
 * Go through all the address space mappings, validating or updating
 * the information already gathered, or gathering new information.
 *
 * This function is only called when we suspect that the mappings have changed
 * because this is the first time we're calling it or because of rtld activity.
 */
void
Pupdate_maps(struct ps_prochandle *P)
{
	char mapfile[PATH_MAX];
	int mapfd;
	struct stat statb;
	prmap_t *Pmap = NULL;
	prmap_t *pmap;
	ssize_t nmap;
	int i;
	uint_t oldmapcount;
	map_info_t *newmap, *newp;
	map_info_t *mptr;

	if (P->info_valid || P->state == PS_UNDEAD)
		return;

	Preadauxvec(P);

# if linux
	if (_libproc_debug) {
		printf("in Pupdate_maps (check for 64b maps!)\n");
		printf("in Pupdate_maps (check: pr_mapname is big enough PRMAPSZ=%d)\n", PRMAPSZ);
	}

	FILE *fp;
	char	buf[BUFSIZ];
	(void) snprintf(mapfile, sizeof (mapfile), "%s/%d/maps",
	    procfs_path, (int)P->pid);
	if ((fp = fopen(mapfile, "r")) == NULL) {
		Preset_maps(P);	/* utter failure; destroy tables */
		return;
	}
	Pmap = calloc(sizeof *Pmap, 1);
	for (i = 0; fgets(buf, sizeof buf, fp); i++) {
		unsigned long lo, hi, offset, inode;
		char	perms[128];
		char	majmin[128];
		char	filename[BUFSIZ];
		Pmap = realloc(Pmap, (i + 1) * sizeof *Pmap);
		sscanf(buf, "%lx-%lx %s %lx %s %ld %s",
			&lo, &hi, perms, &offset, majmin, &inode, filename);
		memset(&Pmap[i], 0, sizeof Pmap[i]);
		Pmap[i].pr_vaddr = lo;
		Pmap[i].pr_size = hi - lo;
		Pmap[i].pr_offset = offset;
		Pmap[i].pr_pagesize = 4096;
		if (perms[0] == 'r')
			Pmap[i].pr_mflags |= MA_READ;
		if (perms[1] == 'w')
			Pmap[i].pr_mflags |= MA_WRITE;
		if (perms[2] == 'x')
			Pmap[i].pr_mflags |= MA_EXEC;
		strncpy(Pmap[i].pr_mapname, filename, sizeof(Pmap[i].pr_mapname));
		nmap = i + 1;
	}
	(void) fclose(fp);
# else
	(void) snprintf(mapfile, sizeof (mapfile), "%s/%d/map",
	    procfs_path, (int)P->pid);
	if ((mapfd = open(mapfile, O_RDONLY)) < 0 ||
	    fstat(mapfd, &statb) != 0 ||
	    statb.st_size < sizeof (prmap_t) ||
	    (Pmap = malloc(statb.st_size)) == NULL ||
	    (nmap = pread(mapfd, Pmap, statb.st_size, 0L)) <= 0 ||
	    (nmap /= sizeof (prmap_t)) == 0) {
		if (Pmap != NULL)
			free(Pmap);
		if (mapfd >= 0)
			(void) close(mapfd);
		Preset_maps(P);	/* utter failure; destroy tables */
		return;
	}
	(void) close(mapfd);
# endif
	if ((newmap = calloc(1, nmap * sizeof (map_info_t))) == NULL)
		return;

	/*
	 * We try to merge any file information we may have for existing
	 * mappings, to avoid having to rebuild the file info.
	 */
	mptr = P->mappings;
	pmap = Pmap;
	newp = newmap;
	oldmapcount = P->map_count;
	for (i = 0; i < nmap; i++, pmap++, newp++) {

		if (oldmapcount == 0) {
			/*
			 * We've exhausted all the old mappings.  Every new
			 * mapping should be added.
			 */
			newp->map_pmap = *pmap;

		} else if (pmap->pr_vaddr == mptr->map_pmap.pr_vaddr &&
		    pmap->pr_size == mptr->map_pmap.pr_size &&
		    pmap->pr_offset == mptr->map_pmap.pr_offset &&
		    (pmap->pr_mflags & ~(MA_BREAK | MA_STACK)) ==
		    (mptr->map_pmap.pr_mflags & ~(MA_BREAK | MA_STACK)) &&
		    pmap->pr_pagesize == mptr->map_pmap.pr_pagesize &&
		    pmap->pr_shmid == mptr->map_pmap.pr_shmid &&
		    strcmp(pmap->pr_mapname, mptr->map_pmap.pr_mapname) == 0) {

			/*
			 * This mapping matches exactly.  Copy over the old
			 * mapping, taking care to get the latest flags.
			 * Make sure the associated file_info_t is updated
			 * appropriately.
			 */
			*newp = *mptr;
			if (P->map_exec == mptr)
				P->map_exec = newp;
			if (P->map_ldso == mptr)
				P->map_ldso = newp;
			newp->map_pmap.pr_mflags = pmap->pr_mflags;
			if (mptr->map_file != NULL &&
			    mptr->map_file->file_map == mptr)
				mptr->map_file->file_map = newp;
			oldmapcount--;
			mptr++;

		} else if (pmap->pr_vaddr + pmap->pr_size >
		    mptr->map_pmap.pr_vaddr) {

			/*
			 * The old mapping doesn't exist any more, remove it
			 * from the list.
			 */
			map_info_free(P, mptr);
			oldmapcount--;
			i--;
			newp--;
			pmap--;
			mptr++;

		} else {

			/*
			 * This is a new mapping, add it directly.
			 */
			newp->map_pmap = *pmap;
		}
	}

	/*
	 * Free any old maps
	 */
	while (oldmapcount) {
		map_info_free(P, mptr);
		oldmapcount--;
		mptr++;
	}

	free(Pmap);
	if (P->mappings != NULL)
		free(P->mappings);
	P->mappings = newmap;
	P->map_count = P->map_alloc = nmap;
	P->info_valid = 1;

	/*
	 * Consult librtld_db to get the load object
	 * names for all of the shared libraries.
	 */
	if (P->rap != NULL)
		(void) rd_loadobj_iter(P->rap, map_iter, P);
}

/*
 * Update all of the mappings and rtld_db as if by Pupdate_maps(), and then
 * forcibly cache all of the symbol tables associated with all object files.
 */
void
Pupdate_syms(struct ps_prochandle *P)
{
	file_info_t *fptr = list_next(&P->file_head);
	int i;

	Pupdate_maps(P);

	for (i = 0; i < P->num_files; i++, fptr = list_next(fptr)) {
		Pbuild_file_symtab(P, fptr);
		(void) Pbuild_file_ctf(P, fptr);
	}
}

/*
 * Return the librtld_db agent handle for the victim process.
 * The handle will become invalid at the next successful exec() and the
 * client (caller of proc_rd_agent()) must not use it beyond that point.
 * If the process is already dead, we've already tried our best to
 * create the agent during core file initialization.
 */
rd_agent_t *
Prd_agent(struct ps_prochandle *P)
{
	if (P->rap == NULL && P->state != PS_DEAD && P->state != PS_IDLE) {
		Pupdate_maps(P);
		if (P->num_files == 0)
			load_static_maps(P);
		rd_log(_libproc_debug);
		if ((P->rap = rd_new(P, P->pid)) != NULL)
			(void) rd_loadobj_iter(P->rap, map_iter, P);
	}
	return (P->rap);
}

/*
 * Return the prmap_t structure containing 'addr', but only if it
 * is in the dynamic linker's link map and is the text section.
 */
const prmap_t *
Paddr_to_text_map(struct ps_prochandle *P, uintptr_t addr)
{
	map_info_t *mptr;

	if (!P->info_valid)
		Pupdate_maps(P);

	if ((mptr = Paddr2mptr(P, addr)) != NULL) {
		file_info_t *fptr = build_map_symtab(P, mptr);
		const prmap_t *pmp = &mptr->map_pmap;

		/*
		 * Assume that if rl_data_base is NULL, it means that no
		 * data section was found for this load object, and that
		 * a section must be text. Otherwise, a section will be
		 * text unless it ends above the start of the data
		 * section.
		 */
		if (fptr != NULL && fptr->file_lo != NULL &&
		    (fptr->file_lo->rl_data_base == NULL ||
		    pmp->pr_vaddr + pmp->pr_size <
		    fptr->file_lo->rl_data_base))
			return (pmp);
	}

	return (NULL);
}

/*
 * Return the prmap_t structure containing 'addr' (no restrictions on
 * the type of mapping).
 */
const prmap_t *
Paddr_to_map(struct ps_prochandle *P, uintptr_t addr)
{
	map_info_t *mptr;

	if (!P->info_valid)
		Pupdate_maps(P);

	if ((mptr = Paddr2mptr(P, addr)) != NULL)
		return (&mptr->map_pmap);

	return (NULL);
}

/*
 * Convert a full or partial load object name to the prmap_t for its
 * corresponding primary text mapping.
 */
const prmap_t *
Plmid_to_map(struct ps_prochandle *P, Lmid_t lmid, const char *name)
{
	map_info_t *mptr;

	if (name == PR_OBJ_EVERY)
		return (NULL); /* A reasonable mistake */

	if ((mptr = object_name_to_map(P, lmid, name)) != NULL)
		return (&mptr->map_pmap);

	return (NULL);
}

const prmap_t *
Pname_to_map(struct ps_prochandle *P, const char *name)
{
	return (Plmid_to_map(P, PR_LMID_EVERY, name));
}

const rd_loadobj_t *
Paddr_to_loadobj(struct ps_prochandle *P, uintptr_t addr)
{
	map_info_t *mptr;

	if (!P->info_valid)
		Pupdate_maps(P);

	if ((mptr = Paddr2mptr(P, addr)) == NULL)
		return (NULL);

	/*
	 * By building the symbol table, we implicitly bring the PLT
	 * information up to date in the load object.
	 */
	(void) build_map_symtab(P, mptr);
printf("mapping %p to loadobj\n", (void *) addr);
	return (mptr->map_file->file_lo);
}

const rd_loadobj_t *
Plmid_to_loadobj(struct ps_prochandle *P, Lmid_t lmid, const char *name)
{
	map_info_t *mptr;

	if (name == PR_OBJ_EVERY)
		return (NULL);

	if ((mptr = object_name_to_map(P, lmid, name)) == NULL)
		return (NULL);

	/*
	 * By building the symbol table, we implicitly bring the PLT
	 * information up to date in the load object.
	 */
	(void) build_map_symtab(P, mptr);

	return (mptr->map_file->file_lo);
}

const rd_loadobj_t *
Pname_to_loadobj(struct ps_prochandle *P, const char *name)
{
	return (Plmid_to_loadobj(P, PR_LMID_EVERY, name));
}

ctf_file_t *
Pbuild_file_ctf(struct ps_prochandle *P, file_info_t *fptr)
{
	ctf_sect_t ctdata, symtab, strtab;
	sym_tbl_t *symp;
	int err;

	if (fptr->file_ctfp != NULL)
		return (fptr->file_ctfp);

	Pbuild_file_symtab(P, fptr);

	if (fptr->file_ctf_size == 0)
		return (NULL);

	symp = fptr->file_ctf_dyn ? &fptr->file_dynsym : &fptr->file_symtab;
	if (symp->sym_data_pri == NULL)
		return (NULL);

	/*
	 * The buffer may alread be allocated if this is a core file that
	 * contained CTF data for this file.
	 */
	if (fptr->file_ctf_buf == NULL) {
		fptr->file_ctf_buf = malloc(fptr->file_ctf_size);
		if (fptr->file_ctf_buf == NULL) {
			p_dprintf("failed to allocate ctf buffer\n");
			return (NULL);
		}

		if (pread(fptr->file_fd, fptr->file_ctf_buf,
		    fptr->file_ctf_size, fptr->file_ctf_off) !=
		    fptr->file_ctf_size) {
			free(fptr->file_ctf_buf);
			fptr->file_ctf_buf = NULL;
			p_dprintf("failed to read ctf data\n");
			return (NULL);
		}
	}

	ctdata.cts_name = ".SUNW_ctf";
	ctdata.cts_type = SHT_PROGBITS;
	ctdata.cts_flags = 0;
	ctdata.cts_data = fptr->file_ctf_buf;
	ctdata.cts_size = fptr->file_ctf_size;
	ctdata.cts_entsize = 1;
	ctdata.cts_offset = 0;

	symtab.cts_name = fptr->file_ctf_dyn ? ".dynsym" : ".symtab";
	symtab.cts_type = symp->sym_hdr_pri.sh_type;
	symtab.cts_flags = symp->sym_hdr_pri.sh_flags;
	symtab.cts_data = symp->sym_data_pri->d_buf;
	symtab.cts_size = symp->sym_hdr_pri.sh_size;
	symtab.cts_entsize = symp->sym_hdr_pri.sh_entsize;
	symtab.cts_offset = symp->sym_hdr_pri.sh_offset;

	strtab.cts_name = fptr->file_ctf_dyn ? ".dynstr" : ".strtab";
	strtab.cts_type = symp->sym_strhdr.sh_type;
	strtab.cts_flags = symp->sym_strhdr.sh_flags;
	strtab.cts_data = symp->sym_strs;
	strtab.cts_size = symp->sym_strhdr.sh_size;
	strtab.cts_entsize = symp->sym_strhdr.sh_entsize;
	strtab.cts_offset = symp->sym_strhdr.sh_offset;

	fptr->file_ctfp = ctf_bufopen(&ctdata, &symtab, &strtab, &err);
	if (fptr->file_ctfp == NULL) {
		free(fptr->file_ctf_buf);
		fptr->file_ctf_buf = NULL;
		return (NULL);
	}

	p_dprintf("loaded %lu bytes of CTF data for %s\n",
	    (ulong_t)fptr->file_ctf_size, fptr->file_pname);

	return (fptr->file_ctfp);
}

ctf_file_t *
Paddr_to_ctf(struct ps_prochandle *P, uintptr_t addr)
{
	map_info_t *mptr;
	file_info_t *fptr;

	if (!P->info_valid)
		Pupdate_maps(P);

	if ((mptr = Paddr2mptr(P, addr)) == NULL ||
	    (fptr = mptr->map_file) == NULL)
		return (NULL);

	return (Pbuild_file_ctf(P, fptr));
}

ctf_file_t *
Plmid_to_ctf(struct ps_prochandle *P, Lmid_t lmid, const char *name)
{
	map_info_t *mptr;
	file_info_t *fptr;

	if (name == PR_OBJ_EVERY)
		return (NULL);

	if ((mptr = object_name_to_map(P, lmid, name)) == NULL ||
	    (fptr = mptr->map_file) == NULL)
		return (NULL);

	return (Pbuild_file_ctf(P, fptr));
}

ctf_file_t *
Pname_to_ctf(struct ps_prochandle *P, const char *name)
{
	return (Plmid_to_ctf(P, PR_LMID_EVERY, name));
}

/*
 * If we're not a core file, re-read the /proc/<pid>/auxv file and store
 * its contents in P->auxv.  In the case of a core file, we either
 * initialized P->auxv in Pcore() from the NT_AUXV, or we don't have an
 * auxv because the note was missing.
 */
void
Preadauxvec(struct ps_prochandle *P)
{
	char auxfile[64];
	struct stat statb;
	ssize_t naux;
	int fd;

	if (P->state == PS_DEAD)
		return; /* Already read during Pgrab_core() */
	if (P->state == PS_IDLE)
		return; /* No aux vec for Pgrab_file() */

	if (P->auxv != NULL) {
		free(P->auxv);
		P->auxv = NULL;
		P->nauxv = 0;
	}

	(void) snprintf(auxfile, sizeof (auxfile), "%s/%d/auxv",
	    procfs_path, (int)P->pid);
	if ((fd = open(auxfile, O_RDONLY)) < 0)
		return;

#if linux
	/***********************************************/
	/*   On   Linux,   /proc  entries  cannot  be  */
	/*   stat()ed to find their sizes.	       */
	/***********************************************/
	{
	char	buf[128 * 2 * sizeof(long)];
	int	base_i, i;

	P->auxv = malloc(sizeof buf);
	naux = read(fd, buf, 128 * sizeof(auxv_t));
	if (naux <= 0) {
		free(P->auxv);
		P->auxv = NULL;
		close(fd);
		return;
	}

	/***********************************************/
	/*   On  a 64b kernel, running a 32b process,  */
	/*   the auxv entries will be the wrong size,  */
	/*   so  adjust  them  so  that everything is  */
	/*   64b.				       */
	/***********************************************/
#if defined(__amd64)
	if (*(int *) (buf + 4) != 0) {
		/***********************************************/
		/*   32b auxv, correct it.		       */
		/***********************************************/
		struct auxv32 {
			int a_type;
			unsigned int a_ptr;
			} *a32 = (struct auxv32 *) buf;
		auxv_t *a64 = P->auxv;
		int n = naux / sizeof(struct auxv32);
		int	i;
		for (i = 0; i < n; i++, a64++, a32++) {
			a64->a_type = a32->a_type;
			a64->a_un.a_val = a32->a_ptr;
		}
		naux = i;
	} else 
#endif
		{
		/***********************************************/
		/*   64b auxv - we can have it as it is.       */
		/***********************************************/
		memcpy(P->auxv, buf, naux);
		naux /= sizeof(auxv_t);
	}
	/***********************************************/
	/*   Correction   for   those  systems  where  */
	/*   AT_BASE is not set e.g. Centos5.	       */
	/***********************************************/
	base_i = -1;
	for (i = 0; i < naux; i++) {
		if (P->auxv[i].a_type == AT_BASE) {
			base_i = i;
			break;
		}
	}
	if (base_i >= 0 && P->auxv[i].a_un.a_ptr == 0) {
		FILE *fp;
		snprintf(buf, sizeof buf, "/proc/%d/maps", P->pid);
		if ((fp = fopen(buf, "r")) != NULL) {
			while (fgets(buf, sizeof buf, fp) != NULL) {
				unsigned long lo = 0;
				if (strstr(buf, "/ld-") != 0) {
					sscanf(buf, "%lx", &lo);
					P->auxv[i].a_un.a_ptr = (void *) lo;
					break;
					}
			}
			fclose(fp);
			
		}
	}

	}

	if (_libproc_debug) {
		int i; 
		for (i = 0; i < naux; i++) {printf("aux[%d]: type=%d %p\n", i, P->auxv[i].a_type, P->auxv[i].a_un.a_ptr);}
	}
	P->auxv[naux].a_type = AT_NULL;
	P->auxv[naux].a_un.a_val = 0L;
	P->nauxv = (int)naux;


#else
	if (fstat(fd, &statb) == 0 &&
	    statb.st_size >= sizeof (auxv_t) &&
	    (P->auxv = malloc(statb.st_size + sizeof (auxv_t))) != NULL) {
		if ((naux = read(fd, P->auxv, statb.st_size)) < 0 ||
		    (naux /= sizeof (auxv_t)) < 1) {
			free(P->auxv);
			P->auxv = NULL;
		} else {
			P->auxv[naux].a_type = AT_NULL;
			P->auxv[naux].a_un.a_val = 0L;
			P->nauxv = (int)naux;
		}
	}
#endif

	(void) close(fd);
}

/*
 * Return a requested element from the process's aux vector.
 * Return -1 on failure (this is adequate for our purposes).
 */
long
Pgetauxval(struct ps_prochandle *P, int type)
{
	auxv_t *auxv;

	if (P->auxv == NULL)
		Preadauxvec(P);

	if (P->auxv == NULL)
		return (-1);

	for (auxv = P->auxv; auxv->a_type != AT_NULL; auxv++) {
		if (auxv->a_type == type)
			return (auxv->a_un.a_val);
	}

	return (-1);
}

/*
 * Return a pointer to our internal copy of the process's aux vector.
 * The caller should not hold on to this pointer across any libproc calls.
 */
const auxv_t *
Pgetauxvec(struct ps_prochandle *P)
{
	static const auxv_t empty = { AT_NULL, 0L };

	if (P->auxv == NULL)
		Preadauxvec(P);

	if (P->auxv == NULL)
		return (&empty);

	return (P->auxv);
}

/*
 * Return 1 if the given mapping corresponds to the given file_info_t's
 * load object; return 0 otherwise.
 */
static int
is_mapping_in_file(struct ps_prochandle *P, map_info_t *mptr, file_info_t *fptr)
{
	prmap_t *pmap = &mptr->map_pmap;
	rd_loadobj_t *lop = fptr->file_lo;
	uint_t i;
	uintptr_t mstart, mend, sstart, send;

	/*
	 * We can get for free the start address of the text and data
	 * sections of the load object. Start by seeing if the mapping
	 * encloses either of these.
	 */
	if ((pmap->pr_vaddr <= lop->rl_base &&
	    lop->rl_base < pmap->pr_vaddr + pmap->pr_size) ||
	    (pmap->pr_vaddr <= lop->rl_data_base &&
	    lop->rl_data_base < pmap->pr_vaddr + pmap->pr_size))
		return (1);

	/*
	 * It's still possible that this mapping correponds to the load
	 * object. Consider the example of a mapping whose start and end
	 * addresses correspond to those of the load object's text section.
	 * If the mapping splits, e.g. as a result of a segment demotion,
	 * then although both mappings are still backed by the same section,
	 * only one will be seen to enclose that section's start address.
	 * Thus, to be rigorous, we ask not whether this mapping encloses
	 * the start of a section, but whether there exists a section that
	 * overlaps this mapping.
	 *
	 * If we don't already have the section addresses, and we successfully
	 * get them, then we cache them in case we come here again.
	 */
	if (fptr->file_saddrs == NULL &&
	    (fptr->file_saddrs = get_saddrs(P,
	    fptr->file_map->map_pmap.pr_vaddr, &fptr->file_nsaddrs)) == NULL)
		return (0);

	mstart = mptr->map_pmap.pr_vaddr;
	mend = mptr->map_pmap.pr_vaddr + mptr->map_pmap.pr_size;
	for (i = 0; i < fptr->file_nsaddrs; i += 2) {
		/* Does this section overlap the mapping? */
		sstart = fptr->file_saddrs[i];
		send = fptr->file_saddrs[i + 1];
		if (!(mend <= sstart || mstart >= send))
			return (1);
	}

	return (0);
}

/*
 * Find or build the symbol table for the given mapping.
 */
static file_info_t *
build_map_symtab(struct ps_prochandle *P, map_info_t *mptr)
{
	prmap_t *pmap = &mptr->map_pmap;
	file_info_t *fptr;
	uint_t i;

	if ((fptr = mptr->map_file) != NULL) {
		Pbuild_file_symtab(P, fptr);
		return (fptr);
	}

	if (pmap->pr_mapname[0] == '\0')
		return (NULL);

	/*
	 * Attempt to find a matching file.
	 * (A file can be mapped at several different addresses.)
	 */
	for (i = 0, fptr = list_next(&P->file_head); i < P->num_files;
	    i++, fptr = list_next(fptr)) {
		if (fptr->file_lo &&
		    strcmp(fptr->file_pname, pmap->pr_mapname) == 0 &&
		    is_mapping_in_file(P, mptr, fptr)) {
			mptr->map_file = fptr;
			fptr->file_ref++;
			Pbuild_file_symtab(P, fptr);
			return (fptr);
		}
	}

	/*
	 * If we need to create a new file_info structure, iterate
	 * through the load objects in order to attempt to connect
	 * this new file with its primary text mapping.  We again
	 * need to handle ld.so as a special case because we need
	 * to be able to bootstrap librtld_db.
	 */
	if ((fptr = file_info_new(P, mptr)) == NULL)
		return (NULL);

	if (P->map_ldso != mptr) {
		if (P->rap != NULL)
			(void) rd_loadobj_iter(P->rap, map_iter, P);
		else
			(void) Prd_agent(P);
	} else {
		fptr->file_map = mptr;
	}

	/*
	 * If librtld_db wasn't able to help us connect the file to a primary
	 * text mapping, set file_map to the current mapping because we require
	 * fptr->file_map to be set in Pbuild_file_symtab.  librtld_db may be
	 * unaware of what's going on in the rare case that a legitimate ELF
	 * file has been mmap(2)ed into the process address space *without*
	 * the use of dlopen(3x).
	 */
	if (fptr->file_map == NULL)
		fptr->file_map = mptr;

	Pbuild_file_symtab(P, fptr);

	return (fptr);
}

static int
read_ehdr32(struct ps_prochandle *P, Elf32_Ehdr *ehdr, uint_t *phnum,
    uintptr_t addr)
{
	if (Pread(P, ehdr, sizeof (*ehdr), addr) != sizeof (*ehdr))
		return (-1);

	if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
	    ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
	    ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
	    ehdr->e_ident[EI_MAG3] != ELFMAG3 ||
	    ehdr->e_ident[EI_CLASS] != ELFCLASS32 ||
#ifdef _BIG_ENDIAN
	    ehdr->e_ident[EI_DATA] != ELFDATA2MSB ||
#else
	    ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
#endif
	    ehdr->e_ident[EI_VERSION] != EV_CURRENT)
		return (-1);

	if ((*phnum = ehdr->e_phnum) == PN_XNUM) {
		Elf32_Shdr shdr0;

		if (ehdr->e_shoff == 0 || ehdr->e_shentsize < sizeof (shdr0) ||
		    Pread(P, &shdr0, sizeof (shdr0), addr + ehdr->e_shoff) !=
		    sizeof (shdr0))
			return (-1);

		if (shdr0.sh_info != 0)
			*phnum = shdr0.sh_info;
	}

	return (0);
}

static int
read_dynamic_phdr32(struct ps_prochandle *P, const Elf32_Ehdr *ehdr,
    uint_t phnum, Elf32_Phdr *phdr, uintptr_t addr)
{
	uint_t i;

	for (i = 0; i < phnum; i++) {
		uintptr_t a = addr + ehdr->e_phoff + i * ehdr->e_phentsize;
		if (Pread(P, phdr, sizeof (*phdr), a) != sizeof (*phdr))
			return (-1);

		if (phdr->p_type == PT_DYNAMIC)
			return (0);
	}

	return (-1);
}

#ifdef _LP64
static int
read_ehdr64(struct ps_prochandle *P, Elf64_Ehdr *ehdr, uint_t *phnum,
    uintptr_t addr)
{
	if (Pread(P, ehdr, sizeof (Elf64_Ehdr), addr) != sizeof (Elf64_Ehdr))
		return (-1);

	if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
	    ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
	    ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
	    ehdr->e_ident[EI_MAG3] != ELFMAG3 ||
	    ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
#ifdef _BIG_ENDIAN
	    ehdr->e_ident[EI_DATA] != ELFDATA2MSB ||
#else
	    ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
#endif
	    ehdr->e_ident[EI_VERSION] != EV_CURRENT)
		return (-1);

	if ((*phnum = ehdr->e_phnum) == PN_XNUM) {
		Elf64_Shdr shdr0;

		if (ehdr->e_shoff == 0 || ehdr->e_shentsize < sizeof (shdr0) ||
		    Pread(P, &shdr0, sizeof (shdr0), addr + ehdr->e_shoff) !=
		    sizeof (shdr0))
			return (-1);

		if (shdr0.sh_info != 0)
			*phnum = shdr0.sh_info;
	}

	return (0);
}

static int
read_dynamic_phdr64(struct ps_prochandle *P, const Elf64_Ehdr *ehdr,
    uint_t phnum, Elf64_Phdr *phdr, uintptr_t addr)
{
	uint_t i;

	for (i = 0; i < phnum; i++) {
		uintptr_t a = addr + ehdr->e_phoff + i * ehdr->e_phentsize;
		if (Pread(P, phdr, sizeof (*phdr), a) != sizeof (*phdr))
			return (-1);

		if (phdr->p_type == PT_DYNAMIC)
			return (0);
	}

	return (-1);
}
#endif	/* _LP64 */

/*
 * The text segment for each load object contains the elf header and
 * program headers. We can use this information to determine if the
 * file that corresponds to the load object is the same file that
 * was loaded into the process's address space. There can be a discrepency
 * if a file is recompiled after the process is started or if the target
 * represents a core file from a differently configured system -- two
 * common examples. The DT_CHECKSUM entry in the dynamic section
 * provides an easy method of comparison. It is important to note that
 * the dynamic section usually lives in the data segment, but the meta
 * data we use to find the dynamic section lives in the text segment so
 * if either of those segments is absent we can't proceed.
 *
 * We're looking through the elf file for several items: the symbol tables
 * (both dynsym and symtab), the procedure linkage table (PLT) base,
 * size, and relocation base, and the CTF information. Most of this can
 * be recovered from the loaded image of the file itself, the exceptions
 * being the symtab and CTF data.
 *
 * First we try to open the file that we think corresponds to the load
 * object, if the DT_CHECKSUM values match, we're all set, and can simply
 * recover all the information we need from the file. If the values of
 * DT_CHECKSUM don't match, or if we can't access the file for whatever
 * reasaon, we fake up a elf file to use in its stead. If we can't read
 * the elf data in the process's address space, we fall back to using
 * the file even though it may give inaccurate information.
 *
 * The elf file that we fake up has to consist of sections for the
 * dynsym, the PLT and the dynamic section. Note that in the case of a
 * core file, we'll get the CTF data in the file_info_t later on from
 * a section embedded the core file (if it's present).
 *
 * file_differs() conservatively looks for mismatched files, identifying
 * a match when there is any ambiguity (since that's the legacy behavior).
 */
static int
file_differs(struct ps_prochandle *P, Elf *elf, file_info_t *fptr)
{
	Elf_Scn *scn;
	GElf_Shdr shdr;
	GElf_Dyn dyn;
	Elf_Data *data;
	uint_t i, ndyn;
	GElf_Xword cksum;
	uintptr_t addr;

	if (fptr->file_map == NULL)
		return (0);

	if ((Pcontent(P) & (CC_CONTENT_TEXT | CC_CONTENT_DATA)) !=
	    (CC_CONTENT_TEXT | CC_CONTENT_DATA))
		return (0);

	/*
	 * First, we find the checksum value in the elf file.
	 */
	scn = NULL;
	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		if (gelf_getshdr(scn, &shdr) != NULL &&
		    shdr.sh_type == SHT_DYNAMIC)
			goto found_shdr;
	}
	return (0);

found_shdr:
	if ((data = elf_getdata(scn, NULL)) == NULL)
		return (0);

	if (P->status.pr_dmodel == PR_MODEL_ILP32)
		ndyn = shdr.sh_size / sizeof (Elf32_Dyn);
#ifdef _LP64
	else if (P->status.pr_dmodel == PR_MODEL_LP64)
		ndyn = shdr.sh_size / sizeof (Elf64_Dyn);
#endif
	else
		return (0);

	for (i = 0; i < ndyn; i++) {
		if (gelf_getdyn(data, i, &dyn) != NULL &&
		    dyn.d_tag == DT_CHECKSUM)
			goto found_cksum;
	}

	/*
	 * The in-memory ELF has no DT_CHECKSUM section, but we will report it
	 * as matching the file anyhow.
	 */
	return (0);

found_cksum:
	cksum = dyn.d_un.d_val;
	p_dprintf("elf cksum value is %llx\n", (u_longlong_t)cksum);

	/*
	 * Get the base of the text mapping that corresponds to this file.
	 */
	addr = fptr->file_map->map_pmap.pr_vaddr;

	if (P->status.pr_dmodel == PR_MODEL_ILP32) {
		Elf32_Ehdr ehdr;
		Elf32_Phdr phdr;
		Elf32_Dyn dync, *dynp;
		uint_t phnum, i;

		if (read_ehdr32(P, &ehdr, &phnum, addr) != 0 ||
		    read_dynamic_phdr32(P, &ehdr, phnum, &phdr, addr) != 0)
			return (0);

		if (ehdr.e_type == ET_DYN)
			phdr.p_vaddr += addr;
		if ((dynp = malloc(phdr.p_filesz)) == NULL)
			return (0);
		dync.d_tag = DT_NULL;
		if (Pread(P, dynp, phdr.p_filesz, phdr.p_vaddr) !=
		    phdr.p_filesz) {
			free(dynp);
			return (0);
		}

		for (i = 0; i < phdr.p_filesz / sizeof (Elf32_Dyn); i++) {
			if (dynp[i].d_tag == DT_CHECKSUM)
				dync = dynp[i];
		}

		free(dynp);

		if (dync.d_tag != DT_CHECKSUM)
			return (0);

		p_dprintf("image cksum value is %llx\n",
		    (u_longlong_t)dync.d_un.d_val);
		return (dync.d_un.d_val != cksum);
#ifdef _LP64
	} else if (P->status.pr_dmodel == PR_MODEL_LP64) {
		Elf64_Ehdr ehdr;
		Elf64_Phdr phdr;
		Elf64_Dyn dync, *dynp;
		uint_t phnum, i;

		if (read_ehdr64(P, &ehdr, &phnum, addr) != 0 ||
		    read_dynamic_phdr64(P, &ehdr, phnum, &phdr, addr) != 0)
			return (0);

		if (ehdr.e_type == ET_DYN)
			phdr.p_vaddr += addr;
		if ((dynp = malloc(phdr.p_filesz)) == NULL)
			return (0);
		dync.d_tag = DT_NULL;
		if (Pread(P, dynp, phdr.p_filesz, phdr.p_vaddr) !=
		    phdr.p_filesz) {
			free(dynp);
			return (0);
		}

		for (i = 0; i < phdr.p_filesz / sizeof (Elf64_Dyn); i++) {
			if (dynp[i].d_tag == DT_CHECKSUM)
				dync = dynp[i];
		}

		free(dynp);

		if (dync.d_tag != DT_CHECKSUM)
			return (0);

		p_dprintf("image cksum value is %llx\n",
		    (u_longlong_t)dync.d_un.d_val);
		return (dync.d_un.d_val != cksum);
#endif	/* _LP64 */
	}

	return (0);
}

/*
 * Read data from the specified process and construct an in memory
 * image of an ELF file that represents it well enough to let
 * us probe it for information.
 */
static Elf *
fake_elf(struct ps_prochandle *P, file_info_t *fptr)
{
	Elf *elf;
	uintptr_t addr;
	uint_t phnum;

	if (fptr->file_map == NULL)
		return (NULL);

	if ((Pcontent(P) & (CC_CONTENT_TEXT | CC_CONTENT_DATA)) !=
	    (CC_CONTENT_TEXT | CC_CONTENT_DATA))
		return (NULL);

	addr = fptr->file_map->map_pmap.pr_vaddr;

	if (P->status.pr_dmodel == PR_MODEL_ILP32) {
		Elf32_Ehdr ehdr;
		Elf32_Phdr phdr;

		if ((read_ehdr32(P, &ehdr, &phnum, addr) != 0) ||
		    read_dynamic_phdr32(P, &ehdr, phnum, &phdr, addr) != 0)
			return (NULL);

		elf = fake_elf32(P, fptr, addr, &ehdr, phnum, &phdr);
#ifdef _LP64
	} else {
		Elf64_Ehdr ehdr;
		Elf64_Phdr phdr;

		if (read_ehdr64(P, &ehdr, &phnum, addr) != 0 ||
		    read_dynamic_phdr64(P, &ehdr, phnum, &phdr, addr) != 0)
			return (NULL);

		elf = fake_elf64(P, fptr, addr, &ehdr, phnum, &phdr);
#endif
	}

	return (elf);
}

/*
 * We wouldn't need these if qsort(3C) took an argument for the callback...
 */
static mutex_t sort_mtx = DEFAULTMUTEX;
static char *sort_strs;
static GElf_Sym *sort_syms;

int
byaddr_cmp_common(GElf_Sym *a, char *aname, GElf_Sym *b, char *bname)
{
	if (a->st_value < b->st_value)
		return (-1);
	if (a->st_value > b->st_value)
		return (1);

	/*
	 * Prefer the function to the non-function.
	 */
	if (GELF_ST_TYPE(a->st_info) != GELF_ST_TYPE(b->st_info)) {
		if (GELF_ST_TYPE(a->st_info) == STT_FUNC)
			return (-1);
		if (GELF_ST_TYPE(b->st_info) == STT_FUNC)
			return (1);
	}

	/*
	 * Prefer the weak or strong global symbol to the local symbol.
	 */
	if (GELF_ST_BIND(a->st_info) != GELF_ST_BIND(b->st_info)) {
		if (GELF_ST_BIND(b->st_info) == STB_LOCAL)
			return (-1);
		if (GELF_ST_BIND(a->st_info) == STB_LOCAL)
			return (1);
	}

	/*
	 * Prefer the symbol that doesn't begin with a '$' since compilers and
	 * other symbol generators often use it as a prefix.
	 */
	if (*bname == '$')
		return (-1);
	if (*aname == '$')
		return (1);

	/*
	 * Prefer the name with fewer leading underscores in the name.
	 */
	while (*aname == '_' && *bname == '_') {
		aname++;
		bname++;
	}

	if (*bname == '_')
		return (-1);
	if (*aname == '_')
		return (1);

	/*
	 * Prefer the symbol with the smaller size.
	 */
	if (a->st_size < b->st_size)
		return (-1);
	if (a->st_size > b->st_size)
		return (1);

	/*
	 * All other factors being equal, fall back to lexicographic order.
	 */
	return (strcmp(aname, bname));
}

static int
byaddr_cmp(const void *aa, const void *bb)
{
	GElf_Sym *a = &sort_syms[*(uint_t *)aa];
	GElf_Sym *b = &sort_syms[*(uint_t *)bb];
	char *aname = sort_strs + a->st_name;
	char *bname = sort_strs + b->st_name;

	return (byaddr_cmp_common(a, aname, b, bname));
}

static int
byname_cmp(const void *aa, const void *bb)
{
	GElf_Sym *a = &sort_syms[*(uint_t *)aa];
	GElf_Sym *b = &sort_syms[*(uint_t *)bb];
	char *aname = sort_strs + a->st_name;
	char *bname = sort_strs + b->st_name;

	return (strcmp(aname, bname));
}

/*
 * Given a symbol index, look up the corresponding symbol from the
 * given symbol table.
 *
 * This function allows the caller to treat the symbol table as a single
 * logical entity even though there may be 2 actual ELF symbol tables
 * involved. See the comments in Pcontrol.h for details.
 */
static GElf_Sym *
symtab_getsym(sym_tbl_t *symtab, int ndx, GElf_Sym *dst)
{
	/* If index is in range of primary symtab, look it up there */
	if (ndx >= symtab->sym_symn_aux) {
		return (gelf_getsym(symtab->sym_data_pri,
		    ndx - symtab->sym_symn_aux, dst));
	}

	/* Not in primary: Look it up in the auxiliary symtab */
	return (gelf_getsym(symtab->sym_data_aux, ndx, dst));
}

void
optimize_symtab(sym_tbl_t *symtab)
{
	GElf_Sym *symp, *syms;
	uint_t i, *indexa, *indexb;
	size_t symn, strsz, count;

	if (symtab == NULL || symtab->sym_data_pri == NULL ||
	    symtab->sym_byaddr != NULL)
		return;

	symn = symtab->sym_symn;
	strsz = symtab->sym_strsz;

	symp = syms = malloc(sizeof (GElf_Sym) * symn);
	if (symp == NULL) {
		p_dprintf("optimize_symtab: failed to malloc symbol array");
		return;
	}

	/*
	 * First record all the symbols into a table and count up the ones
	 * that we're interested in. We mark symbols as invalid by setting
	 * the st_name to an illegal value.
	 */
	for (i = 0, count = 0; i < symn; i++, symp++) {
		if (symtab_getsym(symtab, i, symp) != NULL &&
		    symp->st_name < strsz &&
		    IS_DATA_TYPE(GELF_ST_TYPE(symp->st_info)))
			count++;
		else
			symp->st_name = strsz;
	}

	/*
	 * Allocate sufficient space for both tables and populate them
	 * with the same symbols we just counted.
	 */
	symtab->sym_count = count;
	indexa = symtab->sym_byaddr = calloc(sizeof (uint_t), count);
	indexb = symtab->sym_byname = calloc(sizeof (uint_t), count);
	if (indexa == NULL || indexb == NULL) {
		p_dprintf(
		    "optimize_symtab: failed to malloc symbol index arrays");
		symtab->sym_count = 0;
		if (indexa != NULL) {	/* First alloc succeeded. Free it */
			free(indexa);
			symtab->sym_byaddr = NULL;
		}
		free(syms);
		return;
	}
	for (i = 0, symp = syms; i < symn; i++, symp++) {
		if (symp->st_name < strsz)
			*indexa++ = *indexb++ = i;
	}

	/*
	 * Sort the two tables according to the appropriate criteria,
	 * unless the user has overridden this behaviour.
	 *
	 * An example where we might not sort the tables is the relatively
	 * unusual case of a process with very large symbol tables in which
	 * we perform few lookups. In such a case the total time would be
	 * dominated by the sort. It is difficult to determine a priori
	 * how many lookups an arbitrary client will perform, and
	 * hence whether the symbol tables should be sorted. We therefore
	 * sort the tables by default, but provide the user with a
	 * "chicken switch" in the form of the LIBPROC_NO_QSORT
	 * environment variable.
	 */
	if (!_libproc_no_qsort) {
		(void) mutex_lock(&sort_mtx);
		sort_strs = symtab->sym_strs;
		sort_syms = syms;

		qsort(symtab->sym_byaddr, count, sizeof (uint_t), byaddr_cmp);
		qsort(symtab->sym_byname, count, sizeof (uint_t), byname_cmp);

		sort_strs = NULL;
		sort_syms = NULL;
		(void) mutex_unlock(&sort_mtx);
	}

	free(syms);
}

/*
 * Build the symbol table for the given mapped file.
 */
void
Pbuild_file_symtab(struct ps_prochandle *P, file_info_t *fptr)
{
	char objectfile[PATH_MAX];
	uint_t i;

	GElf_Ehdr ehdr;
	GElf_Sym s;

	Elf_Data *shdata;
	Elf_Scn *scn;
	Elf *elf;
	size_t nshdrs, shstrndx;

	struct {
		GElf_Shdr c_shdr;
		Elf_Data *c_data;
		const char *c_name;
	} *cp, *cache = NULL, *dyn = NULL, *plt = NULL, *ctf = NULL;

	if (fptr->file_init)
		return;	/* We've already processed this file */

	/*
	 * Mark the file_info struct as having the symbol table initialized
	 * even if we fail below.  We tried once; we don't try again.
	 */
	fptr->file_init = 1;

	if (elf_version(EV_CURRENT) == EV_NONE) {
		p_dprintf("libproc ELF version is more recent than libelf\n");
		return;
	}

	if (P->state == PS_DEAD || P->state == PS_IDLE) {
		/*
		 * If we're a not live, we can't open files from the /proc
		 * object directory; we have only the mapping and file names
		 * to guide us.  We prefer the file_lname, but need to handle
		 * the case of it being NULL in order to bootstrap: we first
		 * come here during rd_new() when the only information we have
		 * is interpreter name associated with the AT_BASE mapping.
		 */
		(void) snprintf(objectfile, sizeof (objectfile), "%s",
		    fptr->file_lname ? fptr->file_lname : fptr->file_pname);
	} else {
# if linux
		(void) strncpy(objectfile, fptr->file_pname, sizeof (objectfile) - 1);
# else
		(void) snprintf(objectfile, sizeof (objectfile),
		    "%s/%d/object/%s",
		    procfs_path, (int)P->pid, fptr->file_pname);
# endif
	}

/**********************************************************************/
/*   libelf  disagrees  with  solaris  on  what  return is from some  */
/*   functions, so parameterise this.				      */
/**********************************************************************/
#define ELF_ERR -1
	/*
	 * Open the object file, create the elf file, and then get the elf
	 * header and .shstrtab data buffer so we can process sections by
	 * name. If anything goes wrong try to fake up an elf file from
	 * the in-core elf image.
	 */
	if (_libproc_debug)
		printf("Build symtab %s\n", objectfile);
	if ((fptr->file_fd = open(objectfile, O_RDONLY)) < 0) {
		p_dprintf("Pbuild_file_symtab: failed to open %s: %s\n",
		    objectfile, strerror(errno));

		if ((elf = fake_elf(P, fptr)) == NULL ||
		    elf_kind(elf) != ELF_K_ELF ||
		    gelf_getehdr(elf, &ehdr) == NULL ||
		    elf_getshnum(elf, &nshdrs) == ELF_ERR ||
		    elf_getshstrndx(elf, &shstrndx) == ELF_ERR ||
		    (scn = elf_getscn(elf, shstrndx)) == NULL ||
		    (shdata = elf_getdata(scn, NULL)) == NULL) {
			p_dprintf("failed to fake up ELF file\n");
			return;
		}

	} else if ((elf = elf_begin(fptr->file_fd, ELF_C_READ, NULL)) == NULL ||
	    elf_kind(elf) != ELF_K_ELF ||
	    gelf_getehdr(elf, &ehdr) == NULL ||
	    elf_getshnum(elf, &nshdrs) == ELF_ERR ||
	    elf_getshstrndx(elf, &shstrndx) == ELF_ERR ||
	    (scn = elf_getscn(elf, shstrndx)) == NULL ||
	    (shdata = elf_getdata(scn, NULL)) == NULL) {
		int err = elf_errno();
		p_dprintf("failed to process ELF file %s: %s\n",
		    objectfile, (err == 0) ? "<null>" : elf_errmsg(err));
#if 0
printf("bad elf_kind %x\n", elf_kind(elf));
printf("bad gelf_getehdr: %p\n", gelf_getehdr(elf, &ehdr));
printf("bad elf_getshnum = %d nshdrs=%d\n", elf_getshnum(elf, &nshdrs), nshdrs);
printf("bad elf_getshstrndx:%d shstrndx=%d\n", elf_getshstrndx(elf, &shstrndx), shstrndx);
//		if (    (scn = elf_getscn(elf, shstrndx)) == NULL ||
//		    (shdata = elf_getdata(scn, NULL)) == NULL) printf("bad last\n");
#endif
		if ((elf = fake_elf(P, fptr)) == NULL ||
		    elf_kind(elf) != ELF_K_ELF ||
		    gelf_getehdr(elf, &ehdr) == NULL ||
		    elf_getshnum(elf, &nshdrs) == ELF_ERR ||
		    elf_getshstrndx(elf, &shstrndx) == ELF_ERR ||
		    (scn = elf_getscn(elf, shstrndx)) == NULL ||
		    (shdata = elf_getdata(scn, NULL)) == NULL) {
			p_dprintf("failed to fake up ELF file\n");
printf("%s(%d): fix me!\n", __func__, __LINE__);
			goto bad;
		}

	} else if (file_differs(P, elf, fptr)) {
		Elf *newelf;

		/*
		 * Before we get too excited about this elf file, we'll check
		 * its checksum value against the value we have in memory. If
		 * they don't agree, we try to fake up a new elf file and
		 * proceed with that instead.
		 */

		p_dprintf("ELF file %s (%lx) doesn't match in-core image\n",
		    fptr->file_pname,
		    (ulong_t)fptr->file_map->map_pmap.pr_vaddr);

		if ((newelf = fake_elf(P, fptr)) == NULL ||
		    elf_kind(newelf) != ELF_K_ELF ||
		    gelf_getehdr(newelf, &ehdr) == NULL ||
		    elf_getshnum(newelf, &nshdrs) == ELF_ERR ||
		    elf_getshstrndx(newelf, &shstrndx) == ELF_ERR ||
		    (scn = elf_getscn(newelf, shstrndx)) == NULL ||
		    (shdata = elf_getdata(scn, NULL)) == NULL) {
			p_dprintf("failed to fake up ELF file\n");
		} else {
			(void) elf_end(elf);
			elf = newelf;

			p_dprintf("switched to faked up ELF file\n");
		}
	}

	if ((cache = malloc(nshdrs * sizeof (*cache))) == NULL) {
		p_dprintf("failed to malloc section cache for %s\n", objectfile);
		goto bad;
	}

	p_dprintf("processing ELF file %s\n", objectfile);
	fptr->file_class = ehdr.e_ident[EI_CLASS];
	fptr->file_etype = ehdr.e_type;
	fptr->file_elf = elf;
	fptr->file_shstrs = shdata->d_buf;
	fptr->file_shstrsz = shdata->d_size;

	/*
	 * Iterate through each section, caching its section header, data
	 * pointer, and name.  We use this for handling sh_link values below.
	 */
	for (cp = cache + 1, scn = NULL; scn = elf_nextscn(elf, scn); cp++) {
		if (gelf_getshdr(scn, &cp->c_shdr) == NULL) {
			p_dprintf("Pbuild_file_symtab: Failed to get section "
			    "header\n");
			goto bad; /* Failed to get section header */
		}

		if ((cp->c_data = elf_getdata(scn, NULL)) == NULL) {
			p_dprintf("Pbuild_file_symtab: Failed to get section "
			    "data\n");
			goto bad; /* Failed to get section data */
		}

		if (cp->c_shdr.sh_name >= shdata->d_size) {
			p_dprintf("Pbuild_file_symtab: corrupt section name");
			goto bad; /* Corrupt section name */
		}

		cp->c_name = (const char *)shdata->d_buf + cp->c_shdr.sh_name;
//printf("found section %s\n", cp->c_name);
	}

	/*
	 * Now iterate through the section cache in order to locate info
	 * for the .symtab, .dynsym, .SUNW_ldynsym, .dynamic, .plt,
	 * and .SUNW_ctf sections:
	 */
	for (i = 1, cp = cache + 1; i < nshdrs; i++, cp++) {
		GElf_Shdr *shp = &cp->c_shdr;

		if (shp->sh_type == SHT_SYMTAB || shp->sh_type == SHT_DYNSYM) {
			sym_tbl_t *symp = shp->sh_type == SHT_SYMTAB ?
			    &fptr->file_symtab : &fptr->file_dynsym;
			/*
			 * It's possible that the we already got the symbol
			 * table from the core file itself. Either the file
			 * differs in which case our faked up elf file will
			 * only contain the dynsym (not the symtab) or the
			 * file matches in which case we'll just be replacing
			 * the symbol table we pulled out of the core file
			 * with an equivalent one. In either case, this
			 * check isn't essential, but it's a good idea.
			 */
			if (symp->sym_data_pri == NULL) {
				p_dprintf("Symbol table found for %s\n",
				    objectfile);
				symp->sym_data_pri = cp->c_data;
				symp->sym_symn +=
				    shp->sh_size / shp->sh_entsize;
				symp->sym_strs =
				    cache[shp->sh_link].c_data->d_buf;
				symp->sym_strsz =
				    cache[shp->sh_link].c_data->d_size;
				symp->sym_hdr_pri = cp->c_shdr;
				symp->sym_strhdr = cache[shp->sh_link].c_shdr;
			} else {
				p_dprintf("Symbol table already there for %s\n",
				    objectfile);
			}
		} else if (shp->sh_type == SHT_SUNW_LDYNSYM) {
			/* .SUNW_ldynsym section is auxiliary to .dynsym */
			if (fptr->file_dynsym.sym_data_aux == NULL) {
				p_dprintf(".SUNW_ldynsym symbol table"
				    " found for %s\n", objectfile);
				fptr->file_dynsym.sym_data_aux = cp->c_data;
				fptr->file_dynsym.sym_symn_aux =
				    shp->sh_size / shp->sh_entsize;
				fptr->file_dynsym.sym_symn +=
				    fptr->file_dynsym.sym_symn_aux;
				fptr->file_dynsym.sym_hdr_aux = cp->c_shdr;
			} else {
				p_dprintf(".SUNW_ldynsym symbol table already"
				    " there for %s\n", objectfile);
			}
		} else if (shp->sh_type == SHT_DYNAMIC) {
			dyn = cp;
		} else if (strcmp(cp->c_name, ".plt") == 0) {
			plt = cp;
		} else if (strcmp(cp->c_name, ".SUNW_ctf") == 0) {
			/*
			 * Skip over bogus CTF sections so they don't come back
			 * to haunt us later.
			 */
			if (shp->sh_link == 0 ||
			    shp->sh_link >= nshdrs ||
			    (cache[shp->sh_link].c_shdr.sh_type != SHT_DYNSYM &&
			    cache[shp->sh_link].c_shdr.sh_type != SHT_SYMTAB)) {
				p_dprintf("Bad sh_link %d for "
				    "CTF\n", shp->sh_link);
				continue;
			}
			ctf = cp;
		}
	}

	/*
	 * At this point, we've found all the symbol tables we're ever going
	 * to find: the ones in the loop above and possibly the symtab that
	 * was included in the core file. Before we perform any lookups, we
	 * create sorted versions to optimize for lookups.
	 */
	optimize_symtab(&fptr->file_symtab);
	optimize_symtab(&fptr->file_dynsym);

	/*
	 * Fill in the base address of the text mapping for shared libraries.
	 * This allows us to translate symbols before librtld_db is ready.
	 */
	if (fptr->file_etype == ET_DYN) {
		fptr->file_dyn_base = fptr->file_map->map_pmap.pr_vaddr -
		    fptr->file_map->map_pmap.pr_offset;
		p_dprintf("setting file_dyn_base for %s to %lx\n",
		    objectfile, (long)fptr->file_dyn_base);
	}

	/*
	 * Record the CTF section information in the file info structure.
	 */
	if (ctf != NULL) {
		fptr->file_ctf_off = ctf->c_shdr.sh_offset;
		fptr->file_ctf_size = ctf->c_shdr.sh_size;
		if (ctf->c_shdr.sh_link != 0 &&
		    cache[ctf->c_shdr.sh_link].c_shdr.sh_type == SHT_DYNSYM)
			fptr->file_ctf_dyn = 1;
	}

	if (fptr->file_lo == NULL)
		goto done; /* Nothing else to do if no load object info */

	/*
	 * If the object is a shared library and we have a different rl_base
	 * value, reset file_dyn_base according to librtld_db's information.
	 */
	if (fptr->file_etype == ET_DYN &&
	    fptr->file_lo->rl_base != fptr->file_dyn_base) {
		p_dprintf("resetting file_dyn_base for %s to %lx\n",
		    objectfile, (long)fptr->file_lo->rl_base);
		fptr->file_dyn_base = fptr->file_lo->rl_base;
	}

	if (_libproc_debug)
		printf("plt at %p\n", plt);

	/*
	 * Fill in the PLT information for this file if a PLT symbol is found.
	 */
	if (sym_by_name(&fptr->file_dynsym, "_PROCEDURE_LINKAGE_TABLE_", &s,
	    NULL) != NULL) {
		fptr->file_plt_base = s.st_value + fptr->file_dyn_base;
		fptr->file_plt_size = (plt != NULL) ? plt->c_shdr.sh_size : 0;

		/*
		 * Bring the load object up to date; it is the only way the
		 * user has to access the PLT data. The PLT information in the
		 * rd_loadobj_t is not set in the call to map_iter() (the
		 * callback for rd_loadobj_iter) where we set file_lo.
		 */
		fptr->file_lo->rl_plt_base = fptr->file_plt_base;
		fptr->file_lo->rl_plt_size = fptr->file_plt_size;

		p_dprintf("PLT found at %p, size = %lu\n",
		    (void *)fptr->file_plt_base, (ulong_t)fptr->file_plt_size);
	}
#if defined(linux)
	/***********************************************/
	/*   Linux    executables    dont    have   a  */
	/*   _PROCEDURE_LINKAGE_TABLE_  but  we  have  */
	/*   the plt section anyway, so lets use it.   */
	/***********************************************/
	if (1 || plt) {
		fptr->file_lo->rl_plt_base = plt->c_shdr.sh_offset;
		fptr->file_lo->rl_plt_size = plt->c_shdr.sh_size;

		p_dprintf("PLT found at %p, size = %lu\n",
		    (void *)fptr->file_plt_base, (ulong_t)fptr->file_plt_size);
	}
#endif
	/*
	 * Fill in the PLT information.
	 */
	if (dyn != NULL) {
		uintptr_t dynaddr = dyn->c_shdr.sh_addr + fptr->file_dyn_base;
		size_t ndyn = dyn->c_shdr.sh_size / dyn->c_shdr.sh_entsize;
		GElf_Dyn d;

		for (i = 0; i < ndyn; i++) {
			if (gelf_getdyn(dyn->c_data, i, &d) != NULL &&
			    d.d_tag == DT_JMPREL) {
				p_dprintf("DT_JMPREL is %p\n",
				    (void *)(uintptr_t)d.d_un.d_ptr);
				fptr->file_jmp_rel =
				    d.d_un.d_ptr + fptr->file_dyn_base;
				break;
			}
		}

		p_dprintf("_DYNAMIC found at %p, %lu entries, DT_JMPREL = %p\n",
		    (void *)dynaddr, (ulong_t)ndyn, (void *)fptr->file_jmp_rel);
	}

done:
	free(cache);
	return;

bad:
	if (cache != NULL)
		free(cache);

	(void) elf_end(elf);
	fptr->file_elf = NULL;
	if (fptr->file_elfmem != NULL) {
		free(fptr->file_elfmem);
		fptr->file_elfmem = NULL;
	}
	(void) close(fptr->file_fd);
	fptr->file_fd = -1;
}

/*
 * Given a process virtual address, return the map_info_t containing it.
 * If none found, return NULL.
 */
map_info_t *
Paddr2mptr(struct ps_prochandle *P, uintptr_t addr)
{
	int lo = 0;
	int hi = P->map_count - 1;
	int mid;
	map_info_t *mp;

	while (lo <= hi) {

		mid = (lo + hi) / 2;
		mp = &P->mappings[mid];

		/* check that addr is in [vaddr, vaddr + size) */
		if ((addr - mp->map_pmap.pr_vaddr) < mp->map_pmap.pr_size)
			return (mp);

		if (addr < mp->map_pmap.pr_vaddr)
			hi = mid - 1;
		else
			lo = mid + 1;
	}

	return (NULL);
}

/*
 * Return the map_info_t for the executable file.
 * If not found, return NULL.
 */
static map_info_t *
exec_map(struct ps_prochandle *P)
{
	uint_t i;
	map_info_t *mptr;
	map_info_t *mold = NULL;
	file_info_t *fptr;
	uintptr_t base;

	for (i = 0, mptr = P->mappings; i < P->map_count; i++, mptr++) {
		if (mptr->map_pmap.pr_mapname[0] == '\0')
			continue;
		if (strcmp(mptr->map_pmap.pr_mapname, "a.out") == 0) {
			if ((fptr = mptr->map_file) != NULL &&
			    fptr->file_lo != NULL) {
				base = fptr->file_lo->rl_base;
				if (base >= mptr->map_pmap.pr_vaddr &&
				    base < mptr->map_pmap.pr_vaddr +
				    mptr->map_pmap.pr_size)	/* text space */
					return (mptr);
				mold = mptr;	/* must be the data */
				continue;
			}
			/* This is a poor way to test for text space */
			if (!(mptr->map_pmap.pr_mflags & MA_EXEC) ||
			    (mptr->map_pmap.pr_mflags & MA_WRITE)) {
				mold = mptr;
				continue;
			}
			return (mptr);
		}
	}

	return (mold);
}

/*
 * Given a shared object name, return the map_info_t for it.  If no matching
 * object is found, return NULL.  Normally, the link maps contain the full
 * object pathname, e.g. /usr/lib/libc.so.1.  We allow the object name to
 * take one of the following forms:
 *
 * 1. An exact match (i.e. a full pathname): "/usr/lib/libc.so.1"
 * 2. An exact basename match: "libc.so.1"
 * 3. An initial basename match up to a '.' suffix: "libc.so" or "libc"
 * 4. The literal string "a.out" is an alias for the executable mapping
 *
 * The third case is a convenience for callers and may not be necessary.
 *
 * As the exact same object name may be loaded on different link maps (see
 * dlmopen(3DL)), we also allow the caller to resolve the object name by
 * specifying a particular link map id.  If lmid is PR_LMID_EVERY, the
 * first matching name will be returned, regardless of the link map id.
 */
static map_info_t *
object_to_map(struct ps_prochandle *P, Lmid_t lmid, const char *objname)
{
	map_info_t *mp;
	file_info_t *fp;
	size_t objlen;
	uint_t i;

	/*
	 * If we have no rtld_db, then always treat a request as one for all
	 * link maps.
	 */
	if (P->rap == NULL)
		lmid = PR_LMID_EVERY;

	/*
	 * First pass: look for exact matches of the entire pathname or
	 * basename (cases 1 and 2 above):
	 */
	for (i = 0, mp = P->mappings; i < P->map_count; i++, mp++) {

		if (mp->map_pmap.pr_mapname[0] == '\0' ||
		    (fp = mp->map_file) == NULL || fp->file_lname == NULL)
			continue;

		if (lmid != PR_LMID_EVERY &&
		    (fp->file_lo == NULL || lmid != fp->file_lo->rl_lmident))
			continue;

		/*
		 * If we match, return the primary text mapping; otherwise
		 * just return the mapping we matched.
		 */
		if (strcmp(fp->file_lname, objname) == 0 ||
		    strcmp(fp->file_lbase, objname) == 0)
			return (fp->file_map ? fp->file_map : mp);
	}

	objlen = strlen(objname);

	/*
	 * Second pass: look for partial matches (case 3 above):
	 */
	for (i = 0, mp = P->mappings; i < P->map_count; i++, mp++) {

		if (mp->map_pmap.pr_mapname[0] == '\0' ||
		    (fp = mp->map_file) == NULL || fp->file_lname == NULL)
			continue;

		if (lmid != PR_LMID_EVERY &&
		    (fp->file_lo == NULL || lmid != fp->file_lo->rl_lmident))
			continue;

		/*
		 * If we match, return the primary text mapping; otherwise
		 * just return the mapping we matched.
		 */
		if (strncmp(fp->file_lbase, objname, objlen) == 0 &&
		    fp->file_lbase[objlen] == '.')
			return (fp->file_map ? fp->file_map : mp);
	}

	/*
	 * One last check: we allow "a.out" to always alias the executable,
	 * assuming this name was not in use for something else.
	 */
	if ((lmid == PR_LMID_EVERY || lmid == LM_ID_BASE) &&
	    (strcmp(objname, "a.out") == 0))
		return (P->map_exec);

	return (NULL);
}

static map_info_t *
object_name_to_map(struct ps_prochandle *P, Lmid_t lmid, const char *name)
{
	map_info_t *mptr;

	if (!P->info_valid)
		Pupdate_maps(P);

	if (P->map_exec == NULL && ((mptr = Paddr2mptr(P,
	    Pgetauxval(P, AT_ENTRY))) != NULL || (mptr = exec_map(P)) != NULL))
		P->map_exec = mptr;

	if (P->map_ldso == NULL && (mptr = Paddr2mptr(P,
	    Pgetauxval(P, AT_BASE))) != NULL)
		P->map_ldso = mptr;

	if (name == PR_OBJ_EXEC)
		mptr = P->map_exec;
	else if (name == PR_OBJ_LDSO)
		mptr = P->map_ldso;
	else if (Prd_agent(P) != NULL || P->state == PS_IDLE)
		mptr = object_to_map(P, lmid, name);
	else
		mptr = NULL;

	return (mptr);
}

/*
 * When two symbols are found by address, decide which one is to be preferred.
 */
static GElf_Sym *
sym_prefer(GElf_Sym *sym1, char *name1, GElf_Sym *sym2, char *name2)
{
	/*
	 * Prefer the non-NULL symbol.
	 */
	if (sym1 == NULL)
		return (sym2);
	if (sym2 == NULL)
		return (sym1);

	/*
	 * Defer to the sort ordering...
	 */
	return (byaddr_cmp_common(sym1, name1, sym2, name2) <= 0 ? sym1 : sym2);
}

/*
 * Use a binary search to do the work of sym_by_addr().
 */
static GElf_Sym *
sym_by_addr_binary(sym_tbl_t *symtab, GElf_Addr addr, GElf_Sym *symp,
    uint_t *idp)
{
	GElf_Sym sym, osym;
	uint_t i, oid, *byaddr = symtab->sym_byaddr;
	int min, max, mid, omid, found = 0;

	if (symtab->sym_data_pri == NULL || symtab->sym_count == 0)
		return (NULL);

	min = 0;
	max = symtab->sym_count - 1;
	osym.st_value = 0;

	/*
	 * We can't return when we've found a match, we have to continue
	 * searching for the closest matching symbol.
	 */
	while (min <= max) {
		mid = (max + min) / 2;

		i = byaddr[mid];
		(void) symtab_getsym(symtab, i, &sym);

		if (addr >= sym.st_value &&
		    addr < sym.st_value + sym.st_size &&
		    (!found || sym.st_value > osym.st_value)) {
			osym = sym;
			omid = mid;
			oid = i;
			found = 1;
		}

		if (addr < sym.st_value)
			max = mid - 1;
		else
			min = mid + 1;
	}

	if (!found)
		return (NULL);

	/*
	 * There may be many symbols with identical values so we walk
	 * backward in the byaddr table to find the best match.
	 */
	do {
		sym = osym;
		i = oid;

		if (omid == 0)
			break;

		oid = byaddr[--omid];
		(void) symtab_getsym(symtab, oid, &osym);
	} while (addr >= osym.st_value &&
	    addr < sym.st_value + osym.st_size &&
	    osym.st_value == sym.st_value);

	*symp = sym;
	if (idp != NULL)
		*idp = i;
	return (symp);
}

/*
 * Use a linear search to do the work of sym_by_addr().
 */
static GElf_Sym *
sym_by_addr_linear(sym_tbl_t *symtab, GElf_Addr addr, GElf_Sym *symbolp,
    uint_t *idp)
{
	size_t symn = symtab->sym_symn;
	char *strs = symtab->sym_strs;
	GElf_Sym sym, *symp = NULL;
	GElf_Sym osym, *osymp = NULL;
	int i, id;

	if (symtab->sym_data_pri == NULL || symn == 0 || strs == NULL)
		return (NULL);

	for (i = 0; i < symn; i++) {
		if ((symp = symtab_getsym(symtab, i, &sym)) != NULL) {
			if (addr >= sym.st_value &&
			    addr < sym.st_value + sym.st_size) {
				if (osymp)
					symp = sym_prefer(
					    symp, strs + symp->st_name,
					    osymp, strs + osymp->st_name);
				if (symp != osymp) {
					osym = sym;
					osymp = &osym;
					id = i;
				}
			}
		}
	}
	if (osymp) {
		*symbolp = osym;
		if (idp)
			*idp = id;
		return (symbolp);
	}
	return (NULL);
}

/*
 * Look up a symbol by address in the specified symbol table.
 * Adjustment to 'addr' must already have been made for the
 * offset of the symbol if this is a dynamic library symbol table.
 *
 * Use a linear or a binary search depending on whether or not we
 * chose to sort the table in optimize_symtab().
 */
static GElf_Sym *
sym_by_addr(sym_tbl_t *symtab, GElf_Addr addr, GElf_Sym *symp, uint_t *idp)
{
	if (_libproc_no_qsort) {
		return (sym_by_addr_linear(symtab, addr, symp, idp));
	} else {
		return (sym_by_addr_binary(symtab, addr, symp, idp));
	}
}

/*
 * Use a binary search to do the work of sym_by_name().
 */
static GElf_Sym *
sym_by_name_binary(sym_tbl_t *symtab, const char *name, GElf_Sym *symp,
    uint_t *idp)
{
	char *strs = symtab->sym_strs;
	uint_t i, *byname = symtab->sym_byname;
	int min, mid, max, cmp;

	if (symtab->sym_data_pri == NULL || strs == NULL ||
	    symtab->sym_count == 0)
		return (NULL);

	min = 0;
	max = symtab->sym_count - 1;

	if (_libproc_debug)
		printf("sym: count=%d (looking for '%s')\n", (int) symtab->sym_count, name);

	while (min <= max) {
		mid = (max + min) / 2;

		i = byname[mid];
		(void) symtab_getsym(symtab, i, symp);
//printf("  %s\n", strs+symp->st_name);

		if ((cmp = strcmp(name, strs + symp->st_name)) == 0) {
			if (idp != NULL)
				*idp = i;
			if (_libproc_debug)
				printf("   => found %s\n", name);
			return (symp);
		}

		if (cmp < 0)
			max = mid - 1;
		else
			min = mid + 1;
	}

	if (_libproc_debug)
		printf("sym_by_name_binary: NOT found %s\n", name);
	return (NULL);
}

/*
 * Use a linear search to do the work of sym_by_name().
 */
static GElf_Sym *
sym_by_name_linear(sym_tbl_t *symtab, const char *name, GElf_Sym *symp,
    uint_t *idp)
{
	size_t symn = symtab->sym_symn;
	char *strs = symtab->sym_strs;
	int i;

	if (symtab->sym_data_pri == NULL || symn == 0 || strs == NULL)
		return (NULL);

	for (i = 0; i < symn; i++) {
		if (symtab_getsym(symtab, i, symp) &&
		    strcmp(name, strs + symp->st_name) == 0) {
			if (idp)
				*idp = i;
			return (symp);
		}
	}

	return (NULL);
}

/*
 * Look up a symbol by name in the specified symbol table.
 *
 * Use a linear or a binary search depending on whether or not we
 * chose to sort the table in optimize_symtab().
 */
static GElf_Sym *
sym_by_name(sym_tbl_t *symtab, const char *name, GElf_Sym *symp, uint_t *idp)
{
	if (_libproc_no_qsort) {
		return (sym_by_name_linear(symtab, name, symp, idp));
	} else {
		return (sym_by_name_binary(symtab, name, symp, idp));
	}
}

/*
 * Search the process symbol tables looking for a symbol whose
 * value to value+size contain the address specified by addr.
 * Return values are:
 *	sym_name_buffer containing the symbol name
 *	GElf_Sym symbol table entry
 *	prsyminfo_t ancillary symbol information
 * Returns 0 on success, -1 on failure.
 */
int
Pxlookup_by_addr(
	struct ps_prochandle *P,
	uintptr_t addr,			/* process address being sought */
	char *sym_name_buffer,		/* buffer for the symbol name */
	size_t bufsize,			/* size of sym_name_buffer */
	GElf_Sym *symbolp,		/* returned symbol table entry */
	prsyminfo_t *sip)		/* returned symbol info */
{
	GElf_Sym	*symp;
	char		*name;
	GElf_Sym	sym1, *sym1p = NULL;
	GElf_Sym	sym2, *sym2p = NULL;
	char		*name1 = NULL;
	char		*name2 = NULL;
	uint_t		i1;
	uint_t		i2;
	map_info_t	*mptr;
	file_info_t	*fptr;

	(void) Prd_agent(P);

	if ((mptr = Paddr2mptr(P, addr)) == NULL ||	/* no such address */
	    (fptr = build_map_symtab(P, mptr)) == NULL || /* no mapped file */
	    fptr->file_elf == NULL)			/* not an ELF file */
		return (-1);

	/*
	 * Adjust the address by the load object base address in
	 * case the address turns out to be in a shared library.
	 */
	addr -= fptr->file_dyn_base;

	/*
	 * Search both symbol tables, symtab first, then dynsym.
	 */
	if ((sym1p = sym_by_addr(&fptr->file_symtab, addr, &sym1, &i1)) != NULL)
		name1 = fptr->file_symtab.sym_strs + sym1.st_name;
	if ((sym2p = sym_by_addr(&fptr->file_dynsym, addr, &sym2, &i2)) != NULL)
		name2 = fptr->file_dynsym.sym_strs + sym2.st_name;

	if ((symp = sym_prefer(sym1p, name1, sym2p, name2)) == NULL)
		return (-1);

	name = (symp == sym1p) ? name1 : name2;
	if (bufsize > 0) {
		(void) strncpy(sym_name_buffer, name, bufsize);
		sym_name_buffer[bufsize - 1] = '\0';
	}

	*symbolp = *symp;
	if (sip != NULL) {
		sip->prs_name = bufsize == 0 ? NULL : sym_name_buffer;
		sip->prs_object = fptr->file_lbase;
		sip->prs_id = (symp == sym1p) ? i1 : i2;
		sip->prs_table = (symp == sym1p) ? PR_SYMTAB : PR_DYNSYM;
		sip->prs_lmid = (fptr->file_lo == NULL) ? LM_ID_BASE :
		    fptr->file_lo->rl_lmident;
	}

	if (GELF_ST_TYPE(symbolp->st_info) != STT_TLS)
		symbolp->st_value += fptr->file_dyn_base;

	return (0);
}

int
Plookup_by_addr(struct ps_prochandle *P, uintptr_t addr, char *buf, size_t size,
    GElf_Sym *symp)
{
	return (Pxlookup_by_addr(P, addr, buf, size, symp, NULL));
}

/*
 * Search the process symbol tables looking for a symbol whose name matches the
 * specified name and whose object and link map optionally match the specified
 * parameters.  On success, the function returns 0 and fills in the GElf_Sym
 * symbol table entry.  On failure, -1 is returned.
 */
int
Pxlookup_by_name(
	struct ps_prochandle *P,
	Lmid_t lmid,			/* link map to match, or -1 for any */
	const char *oname,		/* load object name */
	const char *sname,		/* symbol name */
	GElf_Sym *symp,			/* returned symbol table entry */
	prsyminfo_t *sip)		/* returned symbol info */
{
	map_info_t *mptr;
	file_info_t *fptr;
	int cnt;

	GElf_Sym sym;
	prsyminfo_t si;
	int rv = -1;
	uint_t id;

	if (oname == PR_OBJ_EVERY) {
		/* create all the file_info_t's for all the mappings */
		(void) Prd_agent(P);
		cnt = P->num_files;
		fptr = list_next(&P->file_head);
	} else {
		cnt = 1;
		if ((mptr = object_name_to_map(P, lmid, oname)) == NULL ||
		    (fptr = build_map_symtab(P, mptr)) == NULL)
			return (-1);
	}

	/*
	 * Iterate through the loaded object files and look for the symbol
	 * name in the .symtab and .dynsym of each.  If we encounter a match
	 * with SHN_UNDEF, keep looking in hopes of finding a better match.
	 * This means that a name such as "puts" will match the puts function
	 * in libc instead of matching the puts PLT entry in the a.out file.
	 */
	for (; cnt > 0; cnt--, fptr = list_next(fptr)) {
		Pbuild_file_symtab(P, fptr);

		if (fptr->file_elf == NULL)
			continue;

		if (lmid != PR_LMID_EVERY && fptr->file_lo != NULL &&
		    lmid != fptr->file_lo->rl_lmident)
			continue;

		if (_libproc_debug)
			printf("Pxlookup_by_name '%s' %s %p %p\n", sname, oname, fptr->file_symtab.sym_data_pri, fptr->file_dynsym.sym_data_pri);
		if (fptr->file_symtab.sym_data_pri != NULL &&
		    sym_by_name(&fptr->file_symtab, sname, symp, &id)) {
			if (sip != NULL) {
				sip->prs_id = id;
				sip->prs_table = PR_SYMTAB;
				sip->prs_object = oname;
				sip->prs_name = sname;
				sip->prs_lmid = fptr->file_lo == NULL ?
				    LM_ID_BASE : fptr->file_lo->rl_lmident;
			}
		} else if (fptr->file_dynsym.sym_data_pri != NULL &&
		    sym_by_name(&fptr->file_dynsym, sname, symp, &id)) {
			if (sip != NULL) {
				sip->prs_id = id;
				sip->prs_table = PR_DYNSYM;
				sip->prs_object = oname;
				sip->prs_name = sname;
				sip->prs_lmid = fptr->file_lo == NULL ?
				    LM_ID_BASE : fptr->file_lo->rl_lmident;
			}
		} else {
			continue;
		}

		if (GELF_ST_TYPE(symp->st_info) != STT_TLS)
			symp->st_value += fptr->file_dyn_base;

		if (symp->st_shndx != SHN_UNDEF)
			return (0);

		if (rv != 0) {
			if (sip != NULL)
				si = *sip;
			sym = *symp;
			rv = 0;
		}
	}

	if (rv == 0) {
		if (sip != NULL)
			*sip = si;
		*symp = sym;
	}

	return (rv);
}

/*
 * Search the process symbol tables looking for a symbol whose name matches the
 * specified name, but without any restriction on the link map id.
 */
int
Plookup_by_name(struct ps_prochandle *P, const char *object,
	const char *symbol, GElf_Sym *symp)
{
	return (Pxlookup_by_name(P, PR_LMID_EVERY, object, symbol, symp, NULL));
}

/*
 * Iterate over the process's address space mappings.
 */
int
Pmapping_iter(struct ps_prochandle *P, proc_map_f *func, void *cd)
{
	map_info_t *mptr;
	file_info_t *fptr;
	char *object_name;
	int rc = 0;
	int i;

	/* create all the file_info_t's for all the mappings */
	(void) Prd_agent(P);

	for (i = 0, mptr = P->mappings; i < P->map_count; i++, mptr++) {
		if ((fptr = mptr->map_file) == NULL)
			object_name = NULL;
		else
			object_name = fptr->file_lname;
		if ((rc = func(cd, &mptr->map_pmap, object_name)) != 0)
			return (rc);
	}
	return (0);
}

/*
 * Iterate over the process's mapped objects.
 */
int
Pobject_iter(struct ps_prochandle *P, proc_map_f *func, void *cd)
{
	map_info_t *mptr;
	file_info_t *fptr;
	uint_t cnt;
	int rc = 0;

	(void) Prd_agent(P); /* create file_info_t's for all the mappings */
	Pupdate_maps(P);

	for (cnt = P->num_files, fptr = list_next(&P->file_head);
	    cnt; cnt--, fptr = list_next(fptr)) {

		const char *lname = fptr->file_lname ? fptr->file_lname : "";

		if ((mptr = fptr->file_map) == NULL)
			continue;

		if ((rc = func(cd, &mptr->map_pmap, lname)) != 0)
			return (rc);
	}
	return (0);
}

/*
 * Given a virtual address, return the name of the underlying
 * mapped object (file), as provided by the dynamic linker.
 * Return NULL on failure (no underlying shared library).
 */
char *
Pobjname(struct ps_prochandle *P, uintptr_t addr,
	char *buffer, size_t bufsize)
{
	map_info_t *mptr;
	file_info_t *fptr = NULL;

	/* create all the file_info_t's for all the mappings */
	(void) Prd_agent(P);

	if ((mptr = Paddr2mptr(P, addr)) != NULL &&
	    (fptr = mptr->map_file) != NULL &&
	    fptr->file_lname != NULL) {
		(void) strncpy(buffer, fptr->file_lname, bufsize);
		if (strlen(fptr->file_lname) >= bufsize)
			buffer[bufsize-1] = '\0';
		return (buffer);
	}
#if defined(linux)
	// temp hack - but seems to work
	if (fptr)
		fptr->file_lname = strdup(fptr->file_pname);
#endif
	return (NULL);
}

/*
 * Given a virtual address, return the link map id of the underlying mapped
 * object (file), as provided by the dynamic linker.  Return -1 on failure.
 */
int
Plmid(struct ps_prochandle *P, uintptr_t addr, Lmid_t *lmidp)
{
	map_info_t *mptr;
	file_info_t *fptr;

	/* create all the file_info_t's for all the mappings */
	(void) Prd_agent(P);

	if ((mptr = Paddr2mptr(P, addr)) != NULL &&
	    (fptr = mptr->map_file) != NULL && fptr->file_lo != NULL) {
		*lmidp = fptr->file_lo->rl_lmident;
		return (0);
	}

	return (-1);
}

/*
 * Given an object name and optional lmid, iterate over the object's symbols.
 * If which == PR_SYMTAB, search the normal symbol table.
 * If which == PR_DYNSYM, search the dynamic symbol table.
 */
static int
Psymbol_iter_com(struct ps_prochandle *P, Lmid_t lmid, const char *object_name,
    int which, int mask, pr_order_t order, proc_xsym_f *func, void *cd)
{
	GElf_Sym sym;
	GElf_Shdr shdr;
	map_info_t *mptr;
	file_info_t *fptr;
	sym_tbl_t *symtab;
	size_t symn;
	const char *strs;
	size_t strsz;
	prsyminfo_t si;
	int rv;
	uint_t *map, i, count, ndx;

	if ((mptr = object_name_to_map(P, lmid, object_name)) == NULL)
		return (-1);

	if ((fptr = build_map_symtab(P, mptr)) == NULL || /* no mapped file */
	    fptr->file_elf == NULL)			/* not an ELF file */
		return (-1);

	/*
	 * Search the specified symbol table.
	 */
	switch (which) {
	case PR_SYMTAB:
		symtab = &fptr->file_symtab;
		si.prs_table = PR_SYMTAB;
		break;
	case PR_DYNSYM:
		symtab = &fptr->file_dynsym;
		si.prs_table = PR_DYNSYM;
		break;
	default:
		return (-1);
	}

	si.prs_object = object_name;
	si.prs_lmid = fptr->file_lo == NULL ?
	    LM_ID_BASE : fptr->file_lo->rl_lmident;

	symn = symtab->sym_symn;
	strs = symtab->sym_strs;
	strsz = symtab->sym_strsz;

	switch (order) {
	case PRO_NATURAL:
		map = NULL;
		count = symn;
		break;
	case PRO_BYNAME:
		map = symtab->sym_byname;
		count = symtab->sym_count;
		break;
	case PRO_BYADDR:
		map = symtab->sym_byaddr;
		count = symtab->sym_count;
		break;
	default:
		return (-1);
	}

	if (symtab->sym_data_pri == NULL || strs == NULL || count == 0)
		return (-1);

	rv = 0;

	for (i = 0; i < count; i++) {
		ndx = map == NULL ? i : map[i];
		if (symtab_getsym(symtab, ndx, &sym) != NULL) {
			uint_t s_bind, s_type, type;

			if (sym.st_name >= strsz)	/* invalid st_name */
				continue;

			s_bind = GELF_ST_BIND(sym.st_info);
			s_type = GELF_ST_TYPE(sym.st_info);

			/*
			 * In case you haven't already guessed, this relies on
			 * the bitmask used in <libproc.h> for encoding symbol
			 * type and binding matching the order of STB and STT
			 * constants in <sys/elf.h>.  ELF can't change without
			 * breaking binary compatibility, so I think this is
			 * reasonably fair game.
			 */
			if (s_bind < STB_NUM && s_type < STT_NUM) {
				type = (1 << (s_type + 8)) | (1 << s_bind);
				if ((type & ~mask) != 0)
					continue;
			} else
				continue; /* Invalid type or binding */

# if defined(sun)
			/***********************************************/
			/*   On  Centos  5.3,  we  get a problem with  */
			/*   _dl_initial_error_catch_tsd           in  */
			/*   /lib64/ld-2.5.so.   Maybe   this   is  a  */
			/*   solaris  ELF difference. Or a bug in the  */
			/*   ELF sections/symtab on that release.      */
			/***********************************************/
			if (GELF_ST_TYPE(sym.st_info) != STT_TLS)
				sym.st_value += fptr->file_dyn_base;
#endif

			si.prs_name = strs + sym.st_name;

			/*
			 * If symbol's type is STT_SECTION, then try to lookup
			 * the name of the corresponding section.
			 */
			if (GELF_ST_TYPE(sym.st_info) == STT_SECTION &&
			    fptr->file_shstrs != NULL &&
			    gelf_getshdr(elf_getscn(fptr->file_elf,
			    sym.st_shndx), &shdr) != NULL &&
			    shdr.sh_name != 0 &&
			    shdr.sh_name < fptr->file_shstrsz)
				si.prs_name = fptr->file_shstrs + shdr.sh_name;

			si.prs_id = ndx;
//printf("calling func %s %lx\n", si.prs_name, sym.st_value);
			if ((rv = func(cd, &sym, si.prs_name, &si)) != 0)
				break;
		}
	}

	return (rv);
}

int
Pxsymbol_iter(struct ps_prochandle *P, Lmid_t lmid, const char *object_name,
    int which, int mask, proc_xsym_f *func, void *cd)
{
	return (Psymbol_iter_com(P, lmid, object_name, which, mask,
	    PRO_NATURAL, func, cd));
}

int
Psymbol_iter_by_lmid(struct ps_prochandle *P, Lmid_t lmid,
    const char *object_name, int which, int mask, proc_sym_f *func, void *cd)
{
	return (Psymbol_iter_com(P, lmid, object_name, which, mask,
	    PRO_NATURAL, (proc_xsym_f *)func, cd));
}

int
Psymbol_iter(struct ps_prochandle *P,
    const char *object_name, int which, int mask, proc_sym_f *func, void *cd)
{
	return (Psymbol_iter_com(P, PR_LMID_EVERY, object_name, which, mask,
	    PRO_NATURAL, (proc_xsym_f *)func, cd));
}

int
Psymbol_iter_by_addr(struct ps_prochandle *P,
    const char *object_name, int which, int mask, proc_sym_f *func, void *cd)
{
	return (Psymbol_iter_com(P, PR_LMID_EVERY, object_name, which, mask,
	    PRO_BYADDR, (proc_xsym_f *)func, cd));
}

int
Psymbol_iter_by_name(struct ps_prochandle *P,
    const char *object_name, int which, int mask, proc_sym_f *func, void *cd)
{
	return (Psymbol_iter_com(P, PR_LMID_EVERY, object_name, which, mask,
	    PRO_BYNAME, (proc_xsym_f *)func, cd));
}

/*
 * Get the platform string from the core file if we have it;
 * just perform the system call for the caller if this is a live process.
 */
char *
Pplatform(struct ps_prochandle *P, char *s, size_t n)
{
	if (P->state == PS_IDLE) {
		errno = ENODATA;
		return (NULL);
	}

	if (P->state == PS_DEAD) {
		if (P->core->core_platform == NULL) {
			errno = ENODATA;
			return (NULL);
		}
		(void) strncpy(s, P->core->core_platform, n - 1);
		s[n - 1] = '\0';

	} else if (sysinfo(SI_PLATFORM, s, n) == -1)
		return (NULL);

	return (s);
}

/*
 * Get the uname(2) information from the core file if we have it;
 * just perform the system call for the caller if this is a live process.
 */
int
Puname(struct ps_prochandle *P, struct utsname *u)
{
	if (P->state == PS_IDLE) {
		errno = ENODATA;
		return (-1);
	}

	if (P->state == PS_DEAD) {
		if (P->core->core_uts == NULL) {
			errno = ENODATA;
			return (-1);
		}
		(void) memcpy(u, P->core->core_uts, sizeof (struct utsname));
		return (0);
	}
	return (uname(u));
}

/*
 * Get the zone name from the core file if we have it; look up the
 * name based on the zone id if this is a live process.
 */
char *
Pzonename(struct ps_prochandle *P, char *s, size_t n)
{
	if (P->state == PS_IDLE) {
		errno = ENODATA;
		return (NULL);
	}

	if (P->state == PS_DEAD) {
		if (P->core->core_zonename == NULL) {
			errno = ENODATA;
			return (NULL);
		}
		(void) strlcpy(s, P->core->core_zonename, n);
	} else {
		if (getzonenamebyid(P->status.pr_zoneid, s, n) < 0)
			return (NULL);
		s[n - 1] = '\0';
	}
	return (s);
}

/*
 * Called from Pcreate(), Pgrab(), and Pfgrab_core() to initialize
 * the symbol table heads in the new ps_prochandle.
 */
void
Pinitsym(struct ps_prochandle *P)
{
	P->num_files = 0;
	list_link(&P->file_head, NULL);
}

/*
 * Called from Prelease() to destroy the symbol tables.
 * Must be called by the client after an exec() in the victim process.
 */
void
Preset_maps(struct ps_prochandle *P)
{
	int i;

	if (P->rap != NULL) {
		rd_delete(P->rap);
		P->rap = NULL;
	}

	if (P->execname != NULL) {
		free(P->execname);
		P->execname = NULL;
	}

	if (P->auxv != NULL) {
		free(P->auxv);
		P->auxv = NULL;
		P->nauxv = 0;
	}

	for (i = 0; i < P->map_count; i++)
		map_info_free(P, &P->mappings[i]);

	if (P->mappings != NULL) {
		free(P->mappings);
		P->mappings = NULL;
	}
	P->map_count = P->map_alloc = 0;

	P->info_valid = 0;
}

typedef struct getenv_data {
	char *buf;
	size_t bufsize;
	const char *search;
	size_t searchlen;
} getenv_data_t;

/*ARGSUSED*/
static int
getenv_func(void *data, struct ps_prochandle *P, uintptr_t addr,
    const char *nameval)
{
	getenv_data_t *d = data;
	size_t len;

	if (nameval == NULL)
		return (0);

	if (d->searchlen < strlen(nameval) &&
	    strncmp(nameval, d->search, d->searchlen) == 0 &&
	    nameval[d->searchlen] == '=') {
		len = MIN(strlen(nameval), d->bufsize - 1);
		(void) strncpy(d->buf, nameval, len);
		d->buf[len] = '\0';
		return (1);
	}

	return (0);
}

char *
Pgetenv(struct ps_prochandle *P, const char *name, char *buf, size_t buflen)
{
	getenv_data_t d;

	d.buf = buf;
	d.bufsize = buflen;
	d.search = name;
	d.searchlen = strlen(name);

	if (Penv_iter(P, getenv_func, &d) == 1) {
		char *equals = strchr(d.buf, '=');

		if (equals != NULL) {
			(void) memmove(d.buf, equals + 1,
			    d.buf + buflen - equals - 1);
			d.buf[d.buf + buflen - equals] = '\0';

			return (buf);
		}
	}

	return (NULL);
}

/* number of argument or environment pointers to read all at once */
#define	NARG	100

int
Penv_iter(struct ps_prochandle *P, proc_env_f *func, void *data)
{
	const psinfo_t *psp;
	uintptr_t envpoff;
	GElf_Sym sym;
	int ret;
	char *buf, *nameval;
	size_t buflen;

	int nenv = NARG;
	long envp[NARG];

	/*
	 * Attempt to find the "_environ" variable in the process.
	 * Failing that, use the original value provided by Ppsinfo().
	 */
	if ((psp = Ppsinfo(P)) == NULL)
		return (-1);

	envpoff = psp->pr_envp; /* Default if no _environ found */

	if (Plookup_by_name(P, PR_OBJ_EXEC, "_environ", &sym) == 0) {
		if (P->status.pr_dmodel == PR_MODEL_NATIVE) {
			if (Pread(P, &envpoff, sizeof (envpoff),
			    sym.st_value) != sizeof (envpoff))
				envpoff = psp->pr_envp;
		} else if (P->status.pr_dmodel == PR_MODEL_ILP32) {
			uint32_t envpoff32;

			if (Pread(P, &envpoff32, sizeof (envpoff32),
			    sym.st_value) != sizeof (envpoff32))
				envpoff = psp->pr_envp;
			else
				envpoff = envpoff32;
		}
	}

	buflen = 128;
	buf = malloc(buflen);

	ret = 0;
	for (;;) {
		uintptr_t envoff;

		if (nenv == NARG) {
			(void) memset(envp, 0, sizeof (envp));
			if (P->status.pr_dmodel == PR_MODEL_NATIVE) {
				if (Pread(P, envp,
				    sizeof (envp), envpoff) <= 0) {
					ret = -1;
					break;
				}
			} else if (P->status.pr_dmodel == PR_MODEL_ILP32) {
				uint32_t e32[NARG];
				int i;

				(void) memset(e32, 0, sizeof (e32));
				if (Pread(P, e32, sizeof (e32), envpoff) <= 0) {
					ret = -1;
					break;
				}
				for (i = 0; i < NARG; i++)
					envp[i] = e32[i];
			}
			nenv = 0;
		}

		if ((envoff = envp[nenv++]) == 0)
			break;

		/*
		 * Attempt to read the string from the process.
		 */
again:
		ret = Pread_string(P, buf, buflen, envoff);

		if (ret <= 0) {
			nameval = NULL;
		} else if (ret == buflen - 1) {
			free(buf);
			/*
			 * Bail if we have a corrupted environment
			 */
# if !defined(ARG_MAX)
#	define	ARG_MAX 131072
# endif
			if (buflen >= ARG_MAX)
				return (-1);
			buflen *= 2;
			buf = malloc(buflen);
			goto again;
		} else {
			nameval = buf;
		}

		if ((ret = func(data, P, envoff, nameval)) != 0)
			break;

		envpoff += (P->status.pr_dmodel == PR_MODEL_LP64)? 8 : 4;
	}

	free(buf);

	return (ret);
}
