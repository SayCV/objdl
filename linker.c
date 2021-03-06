/* Copyright (C) 2009 Jisheng Zhang <jszhang3 AT gmail.com>
 * The preceding source of this file is copied from Android's bionic libc code
 */
/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <zlib.h>

#include "dlfcn.h"
#include "linker.h"
#include "sym.h"
#include "linker_debug.h"

#define SO_MAX 64

static int socount = 0;
static soinfo sopool[SO_MAX];
static soinfo *freelist = NULL;
static soinfo *solist = NULL;
static soinfo *sonext = NULL;
static struct dl_symbol *nocexp = NULL;
static struct dl_symbol *syssyms = NULL;
extern struct dl_symbol *cexpSystemSymbols __attribute__((weak, alias("nocexp")));

int debug_verbosity;

static soinfo *alloc_info(const char *name)
{
    soinfo *si;

    if(strlen(name) >= SOINFO_NAME_LEN) {
        ERROR("library name %s too long\n", name);
        return 0;
    }

    /* The freelist is populated when we call free_info(), which in turn is
       done only by dlclose(), which is not likely to be used.
    */
    if (!freelist) {
        if(socount == SO_MAX) {
            ERROR("too many libraries when loading %s\n", name);
            return NULL;
        }
        freelist = sopool + socount++;
        freelist->next = NULL;
    }

    si = freelist;
    freelist = freelist->next;

    /* Make sure we get a clean block of soinfo */
    memset(si, 0, sizeof(soinfo));
    strcpy((char*) si->name, name);
    if(solist == NULL)
        sonext = solist = si;
    else {
        sonext->next = si;
        sonext = si;
    }
    si->next = NULL;
    si->refcount = 0;

    TRACE("name %s: allocated soinfo @ %p\n", name, si);
    return si;
}

static void free_info(soinfo *si)
{
    soinfo *prev = NULL, *trav;
    struct dl_symbol_list *p;

    TRACE("name %s: freeing soinfo @ %p\n", si->name, si);

    for (trav = solist; trav != NULL; trav = trav->next){
        if (trav == si)
            break;
        prev = trav;
    }
    if (trav == NULL) {
        /* si was not ni solist */
        ERROR("name %s is not in solist!\n", si->name);
        return;
    }

    for (p = si->dlsyms; p; p = p->next) {
        free(p->sym.name);
        p = p->next;
        free(p);
    }
    if (prev != NULL)
       prev->next = si->next;
    if (si == sonext) sonext = prev;
    si->next = freelist;
    freelist = si;
}

static const char *sopaths[] = {
    ".",
    0
};

static int _open_lib(const char *name)
{
    int fd;
    struct stat filestat;

    if ((stat(name, &filestat) >= 0) && S_ISREG(filestat.st_mode)) {
        if ((fd = open(name, O_RDONLY)) >= 0)
            return fd;
    }

    return -1;
}

/* TODO: Need to add support for initializing the so search path with
 * LD_LIBRARY_PATH env variable for non-setuid programs. */
static int open_library(const char *name)
{
    int fd;
    char buf[512];
    const char **path;

    TRACE("[ opening %s ]\n", name);

    if(name == 0) return -1;
    if(strlen(name) > 256) return -1;

    if ((name[0] == '/') && ((fd = _open_lib(name)) >= 0))
        return fd;

    for (path = sopaths; *path; path++) {
        snprintf(buf, sizeof(buf), "%s/%s", *path, name);
        if ((fd = _open_lib(buf)) >= 0)
            return fd;
    }

    return -1;
}

/* verify_elf_object
 *      Verifies if the object @ base is a valid ELF object
 *
 * Args:
 *
 * Returns:
 *       0 on success
 *      -1 if no valid ELF object is found @ base.
 */
static int
verify_elf_object(void *base, const char *name)
{
    Elf32_Ehdr *hdr = (Elf32_Ehdr *) base;

    if (hdr->e_ident[EI_MAG0] != ELFMAG0) return -1;
    if (hdr->e_ident[EI_MAG1] != ELFMAG1) return -1;
    if (hdr->e_ident[EI_MAG2] != ELFMAG2) return -1;
    if (hdr->e_ident[EI_MAG3] != ELFMAG3) return -1;

    if (hdr->e_type != ET_REL) {
	    ERROR("error object file type\n");
	    return -1;
    }

    /* TODO: Should we verify anything else in the header? */

    return 0;
}

static void elf_loadsection(int fd, Elf32_Shdr *s, char *q)
{
	lseek(fd, s->sh_offset, SEEK_SET);
	read(fd, q, s->sh_size);
}

static void add_global_symbol(soinfo *si, char *name, unsigned long value)
{
	struct dl_symbol_list *dlsym;
	TRACE("%p add global symbol:%s@0x%lx\n", si, name, value);
	dlsym = malloc(sizeof(*dlsym));
	if (!dlsym) {
		ERROR("malloc failed!\n");
	}
	dlsym->sym.name = strdup(name);
	dlsym->sym.value = value;
	dlsym->next = si->dlsyms;
	si->dlsyms = dlsym;
}

static unsigned long lookup_global_symbol(const char *name)
{
	soinfo *si;
	struct dl_symbol *entry;
	struct dl_symbol_list *dlsym;

	for (entry = syssyms; entry->name; ++entry) {
		if (!strcmp(name, entry->name))
			return entry->value;
	}

	for (si = solist; !si; si=si->next) {
		for (dlsym = si->dlsyms; dlsym; dlsym=dlsym->next) {
			if (!strcmp(name, dlsym->sym.name))
				return dlsym->sym.value;
		}
	}
	return 0;
}

unsigned long lookup_in_library(soinfo *si, const char *name)
{
	struct dl_symbol_list *dlsym;
	TRACE("lookup symbol [%s] at %p\n", name, si);
	for (dlsym = si->dlsyms; dlsym; dlsym=dlsym->next) {
		if (!strcmp(name, dlsym->sym.name)){
			TRACE("[%s] found at %lx\n", name, dlsym->sym.value);
			return dlsym->sym.value;
		}
	}
	return 0;
}

unsigned long lookup(const char *name)
{
    return lookup_global_symbol(name);
}

//resolve all symbols
static void resolve_symbols(Elf32_Shdr *sechdrs, 
			unsigned int symindex, 
			char *strtab, soinfo *si)
{
	char *name;
	unsigned char type, bind;
	unsigned int i, num = sechdrs[symindex].sh_size / sizeof(Elf32_Sym);
	Elf32_Sym *sym = (Elf32_Sym *)sechdrs[symindex].sh_addr;

	TRACE("%d total symbols\n", num);
	for (i = 1; i < num; i++) {//ignore the first one entry
		type = ELF_ST_TYPE (sym[i].st_info);
		bind = ELF_ST_BIND (sym[i].st_info);
		name = strtab + sym[i].st_name;
		TRACE("%d symbol: %s---", i, name);
		switch (type) {
		case STT_SECTION:
		case STT_FILE:
			TRACE("Do nothing\n");
			break;
		case STT_NOTYPE://extern symbol
			if (sym[i].st_name != 0 && sym[i].st_shndx == 0) {
				TRACE("extern symbol\n");
				sym[i].st_value = lookup_global_symbol(name);
				if (!sym[i].st_value) {
					ERROR("Unknown symbol: %s\n", name);
					exit(-1);
				}
			}	
			break;
		case STT_OBJECT:
			TRACE("internal data symbol\n");
			sym[i].st_value += sechdrs[sym[i].st_shndx].sh_addr;
			if (bind == STB_GLOBAL)
				add_global_symbol(si, name, sym[i].st_value);
			break;
		case STT_FUNC:
			TRACE("internal function symbol\n");
			sym[i].st_value += sechdrs[sym[i].st_shndx].sh_addr;
			if (bind == STB_GLOBAL)
				add_global_symbol(si, name, sym[i].st_value);
			break;			
		default:
			ERROR("Unknow type %d\n", type);
			exit(-1);
			break;
		}
	}
}

#ifdef __i386__
static int
do_relocate(Elf32_Shdr *sechdrs, unsigned int symindex, unsigned int relsec)
{
	int i, num;
	uint32_t *where;
	Elf32_Sym *sym;
	Elf32_Rel *rel = (void *)sechdrs[relsec].sh_addr;

	num = sechdrs[relsec].sh_size/sizeof(*rel);
	TRACE("%d relocations\n", num);
	for (i = 0; i < num; i++) {
		TRACE("[%d rel] sym=%d offset=0x%x\n", i, ELF32_R_SYM(rel[i].r_info), rel[i].r_offset);
		where = (void *)sechdrs[sechdrs[relsec].sh_info].sh_addr
			+ rel[i].r_offset;
		sym = (Elf32_Sym *)sechdrs[symindex].sh_addr + ELF32_R_SYM(rel[i].r_info);

		switch (ELF32_R_TYPE(rel[i].r_info)) {
		case R_386_32://s+a
			TRACE("R_386_32\n");
			*where += sym->st_value;
			break;
		case R_386_PC32://s+a-p
			TRACE("R_386_PC32\n");
			/* Add the value, subtract its postition */
			*where += sym->st_value - (uint32_t)where;
			break;
		default:
			ERROR("unknown/unsupported relocation type: %x\n",
			       ELF32_R_TYPE(rel[i].r_info));
			return -1;
			break;
		}
	}
	return 0;
}

static int
do_relocate_addend(Elf32_Shdr *sechdrs, unsigned int symindex, unsigned int relsec)
{
	ERROR("RELA relocation unsupported\n");
	return -1;
}
#endif

#ifdef __sparc__
static int
do_relocate(Elf32_Shdr *sechdrs, unsigned int symindex, unsigned int relsec)
{
	ERROR("RELA relocation unsupported\n");
	return -1;
}

static int
do_relocate_addend(Elf32_Shdr *sechdrs, unsigned int symindex, unsigned int relsec)
{
	int i, num;
	Elf32_Rela *rel = (void *)sechdrs[relsec].sh_addr;
	Elf32_Sym *sym;
	uint8_t *location;
	uint32_t *where, v;
	
	num = sechdrs[relsec].sh_size/sizeof(*rel);
	TRACE("%d relocations\n", num);

	for (i = 0; i < num; i++) {
		TRACE("[%d rel] sym=%d offset=0x%x\n", i, ELF32_R_SYM(rel[i].r_info), rel[i].r_offset);

		location = (uint8_t *)sechdrs[sechdrs[relsec].sh_info].sh_addr
			+ rel[i].r_offset;
		where = (uint32_t *)location;

		sym = (Elf32_Sym *)sechdrs[symindex].sh_addr + ELF32_R_SYM(rel[i].r_info);
		v = sym->st_value + rel[i].r_addend;

		/*refer http://docs.sun.com for SPARC 32 relocation types*/
		switch (ELF32_R_TYPE(rel[i].r_info) & 0xff) {

		case R_SPARC_WDISP30:
			/* V-disp30 (S + A - P) >> 2 */
			v -= (uint32_t) location;
			*where = (*where & ~0x3fffffff) |
				((v >> 2) & 0x3fffffff);
			break;

		case R_SPARC_WDISP22:
			/* V-disp22 (S + A - P) >> 2 */
			v -= (uint32_t) location;
			*where = (*where & ~0x3fffff) |
				((v >> 2) & 0x3fffff);
			break;

		case R_SPARC_HI22:
			/* T-imm22 (S + A) >> 10 */
			*where = (*where & ~0x3fffff) |
				((v >> 10) & 0x3fffff);
			break;

		case R_SPARC_LO10:
			/* T-simm13 (S + A) & 0x3ff */
			*where = (*where & ~0x3ff) | (v & 0x3ff);
			break;

		case R_SPARC_UA32:
			/* V-word32 S + A */
			location[0] = v >> 24;
			location[1] = v >> 16;
			location[2] = v >>  8;
			location[3] = v >>  0;
			break;

		default:
			/* need to support other relocation types? */
			ERROR("unknown/unsupported relocation type: %x\n",
			       ELF32_R_TYPE(rel[i].r_info));
			return -1;
		};
	}
	return 0;

}
#endif

#ifdef __arm__
static int
do_relocate(Elf32_Shdr *sechdrs, unsigned int symindex, unsigned int relsec)
{
	ERROR("RELA relocation unsupported\n");
	return -1;
}

static int
do_relocate_addend(Elf32_Shdr *sechdrs, unsigned int symindex, unsigned int relsec)
{
	int i, num;
	Elf32_Rela *rel = (void *)sechdrs[relsec].sh_addr;
	Elf32_Sym *sym;
	uint8_t *location;
	uint32_t *where, v;
	
	num = sechdrs[relsec].sh_size/sizeof(*rel);
	TRACE("%d relocations\n", num);

	for (i = 0; i < num; i++) {
		TRACE("[%d rel] sym=%d offset=0x%x\n", i, ELF32_R_SYM(rel[i].r_info), rel[i].r_offset);

		location = (uint8_t *)sechdrs[sechdrs[relsec].sh_info].sh_addr
			+ rel[i].r_offset;
		where = (uint32_t *)location;

		sym = (Elf32_Sym *)sechdrs[symindex].sh_addr + ELF32_R_SYM(rel[i].r_info);
		v = sym->st_value + rel[i].r_addend;

		/*refer http://docs.sun.com for SPARC 32 relocation types*/
		switch (ELF32_R_TYPE(rel[i].r_info) & 0xff) {

		case R_SPARC_WDISP30:
			/* V-disp30 (S + A - P) >> 2 */
			v -= (uint32_t) location;
			*where = (*where & ~0x3fffffff) |
				((v >> 2) & 0x3fffffff);
			break;

		case R_SPARC_WDISP22:
			/* V-disp22 (S + A - P) >> 2 */
			v -= (uint32_t) location;
			*where = (*where & ~0x3fffff) |
				((v >> 2) & 0x3fffff);
			break;

		case R_SPARC_HI22:
			/* T-imm22 (S + A) >> 10 */
			*where = (*where & ~0x3fffff) |
				((v >> 10) & 0x3fffff);
			break;

		case R_SPARC_LO10:
			/* T-simm13 (S + A) & 0x3ff */
			*where = (*where & ~0x3ff) | (v & 0x3ff);
			break;

		case R_SPARC_UA32:
			/* V-word32 S + A */
			location[0] = v >> 24;
			location[1] = v >> 16;
			location[2] = v >>  8;
			location[3] = v >>  0;
			break;

		default:
			/* need to support other relocation types? */
			ERROR("unknown/unsupported relocation type: %x\n",
			       ELF32_R_TYPE(rel[i].r_info));
			return -1;
		};
	}
	return 0;

}
#endif

static soinfo *
load_library(const char *name)
{
	int fd = open_library(name);
	int i, cnt;
	soinfo *si = NULL;
	Elf32_Ehdr hdr;
	Elf32_Shdr *sechdrs, *p;
	char *sname, *q, *shstrtbl, *strtab;
	int totalsize = 0;
	unsigned int symindex = 0;

	if(fd == -1)
		return NULL;

	/* We have to read the ELF header to figure out what to do with this image
	*/
	TRACE("loading elf header...\n");
	if (lseek(fd, 0, SEEK_SET) < 0) {
		ERROR("lseek() failed!\n");
		goto fail;
	}
	if ((cnt = read(fd, &hdr, sizeof(hdr))) < 0) {
		ERROR("read() failed!\n");
		goto fail;
	}
	if (verify_elf_object(&hdr, name) < 0) {
        	ERROR("%s is not a valid ELF object\n", name);
		goto fail;
	}

	si = alloc_info(name);
	if (si == NULL)
		goto fail;

	TRACE("loading %d section headers...\n", hdr.e_shnum);
	sechdrs = calloc(sizeof(Elf32_Shdr), hdr.e_shnum);
	if (sechdrs == NULL) {
		ERROR("calloc failed!\n");
		goto fail;
	}
	if (lseek(fd, hdr.e_shoff, SEEK_SET) < 0) {
		ERROR("lseek() failed!\n");
		goto fail;
	}
	i = hdr.e_shnum * sizeof(*sechdrs);
	if (read(fd, sechdrs, i) != i) {
		ERROR("read failed!\n");
		goto fail;
	}
	
	TRACE("loading section name string table...\n");
	p = sechdrs + hdr.e_shstrndx;
	shstrtbl = calloc(p->sh_size, 1);
	if (shstrtbl == NULL) {
		ERROR("calloc failed!\n");
		goto fail;
	}
	if (lseek(fd, p->sh_offset, SEEK_SET) < 0) {
		ERROR("lseek() failed!\n");
		goto fail;
	}
	if (read(fd, shstrtbl, p->sh_size) != p->sh_size) {
		ERROR("read failed!\n");
		goto fail;
	}

	TRACE("collecting info of needed sections...\n");
	for (i = 0; i < hdr.e_shnum; i++) {
		p = sechdrs + i;
		sname = shstrtbl + p->sh_name;
		switch (p->sh_type) {
			case SHT_PROGBITS:
				if (!strcmp(sname,".data") ||
					!strcmp(sname,".text")) {
					totalsize += p->sh_size;
					TRACE("section:%s %uB bytes\n", sname, p->sh_size);
				}
				break;
			case SHT_NOBITS:
				TRACE("section:%s %uB bytes\n", sname, p->sh_size);
				totalsize += p->sh_size;
				break;
			case SHT_SYMTAB:
				TRACE("section:%s %dB bytes\n", sname, p->sh_size);
				totalsize += p->sh_size;
				break;
			case SHT_RELA:
			case SHT_REL:
				if (!strcmp(sname,".rel.data") ||
					!strcmp(sname,".rel.text") || 
					!strcmp(sname,".rela.data") ||
					!strcmp(sname,".rela.text")) {
					TRACE("section:%s %uB bytes\n", sname, p->sh_size);
					totalsize += p->sh_size;
				}
				break;
		}
	}
	q = si->image = calloc(1, totalsize);
	if (q == NULL) {
		ERROR("calloc failed!\n");
		goto fail;
	}
	TRACE("need to load %dB bytes\n", totalsize);
	TRACE("loading needed sections...\n");
	for (i = 0; i < hdr.e_shnum; i++) {
		p = sechdrs + i;
		sname = shstrtbl + p->sh_name;
		TRACE("check section: %s\n", sname);
		switch (p->sh_type) {
			case SHT_PROGBITS:
				if (!strcmp(sname,".data") ||
					!strcmp(sname,".text")){
					TRACE("loading section: %s\n", sname);
					elf_loadsection(fd, p, q);
					p->sh_addr = (unsigned long)q;
					q += p->sh_size;
				}
				break;
			case SHT_NOBITS:
				TRACE("loading section: %s\n", sname);
				elf_loadsection(fd, p, q);
				p->sh_addr = (unsigned long)q;
				q += p->sh_size;
				break;
			case SHT_SYMTAB:
				TRACE("loading section: %s\n", sname);
				symindex = i;
				elf_loadsection(fd, p, q);
				p->sh_addr = (unsigned long)q;
				q += p->sh_size;
				strtab = malloc(sechdrs[p->sh_link].sh_size);
				TRACE("string size: %u\n", sechdrs[p->sh_link].sh_size);
				elf_loadsection(fd, &sechdrs[p->sh_link], strtab);
				sechdrs[p->sh_link].sh_addr = (unsigned long)strtab;
				break;
			case SHT_RELA:
			case SHT_REL:
				if (!strcmp(sname,".rel.data") ||
					!strcmp(sname,".rel.text") ||
					!strcmp(sname,".rela.data") ||
					!strcmp(sname,".rela.text")) {
					TRACE("loading section: %x %s\n", p->sh_name, sname);
					elf_loadsection(fd, p, q);
					p->sh_addr = (unsigned long)q;
					q += p->sh_size;
					TRACE("%d-----%p\n", i, q);
				}
				break;
		}
	}

	TRACE("resolving symbols...\n");
	resolve_symbols(sechdrs, symindex, strtab, si);

	//relocation
	TRACE("relocating...\n");
	for (i = 1; i < hdr.e_shnum; i++) {
		sname = shstrtbl + sechdrs[i].sh_name;
		if (sechdrs[i].sh_type == SHT_REL) {
			if (!strcmp(sname,".rel.data") ||
				!strcmp(sname,".rel.text")) {
				TRACE("SHT_REL relocate %s\n", sname);
				if (do_relocate(sechdrs, symindex, i))
					goto fail;
			}
		}
		else if (sechdrs[i].sh_type == SHT_RELA) {
			if (!strcmp(sname,".rela.data") ||
				!strcmp(sname,".rela.text")) {
				TRACE("SHT_REL relocate %s\n", sname);
				if (do_relocate_addend(sechdrs, symindex, i))
					goto fail;
			}
		}
	}
	TRACE("DONE\n");
  
	close(fd);
	return si;

fail:
	if (si) free_info(si);
		close(fd);
	return NULL;
}

soinfo *find_library(const char *name)
{
	soinfo *si;

	for(si = solist; si != 0; si = si->next){
		if(!strcmp(name, si->name)) {
			if(si->flags & FLAG_ERROR) return 0;
			if(si->flags & FLAG_LINKED) return si;
			ERROR("OOPS: recursive link to '%s'\n", si->name);
			return 0;
		}
	}

	TRACE("[ '%s' has not been loaded yet.  Locating...]\n", name);
	si = load_library(name);
	if(si == NULL)
		return NULL;
//	return init_library(si);
	return si;
}

unsigned unload_library(soinfo *si)
{
	if (si->refcount == 1) {
		free_info(si);
		si->refcount = 0;
	} else {
		si->refcount--;
		PRINT("not unloading '%s', decrementing refcount to %d\n",
			si->name, si->refcount);
	}
	return si->refcount;
}

//read the core sym and initialize
void __linker_init(char *filename)
{
	gzFile gzfile;
	char buf[BUFSIZ/2];
	char otype, *rest;
	unsigned long val;
	int count = 0;
	struct dl_symbol *entry;

	if (cexpSystemSymbols) {
		syssyms = cexpSystemSymbols;
		return;
	}
	gzfile = gzopen(filename, "rb");
	if (gzfile == NULL) {
		ERROR("error gzopen sym file:%s\n", filename);
		exit(-1);
	}
	
	while (gzgets(gzfile, buf, sizeof(buf))) {
		for (rest = buf; *rest && *rest!=' ' && *rest!='\t' &&*rest!='\n'; ++rest)
			;
		*rest++ = 0;
		sscanf(rest, "%c%lx", &otype, &val);
		if(toupper(otype) == 'U')
			continue;
		++count;
	}
	entry = syssyms = calloc(count+1, sizeof(struct dl_symbol));
	if (!syssyms) {
		ERROR("No Memory\n");
		exit(-1);
	}
	
	gzrewind(gzfile);
	while (gzgets(gzfile, buf, sizeof(buf))) {
		for (rest = buf; *rest && *rest!=' ' && *rest!='\t' &&*rest!='\n'; ++rest)
			;
		*rest++ = 0;
		sscanf(rest, "%c%lx", &otype, &val);
		if(toupper(otype) == 'U')
			continue;
		entry->value = val;
		entry->name = strdup(buf);
		++entry;
	}

	gzclose(gzfile);
}
