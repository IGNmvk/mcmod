#define _GNU_SOURCE
#include "gothook.h"
#include <link.h>
#include <elf.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>

#if defined(__LP64__)
#define NC_R_SYM(i)  ELF64_R_SYM(i)
#define NC_R_TYPE(i) ELF64_R_TYPE(i)
#else
#define NC_R_SYM(i)  ELF32_R_SYM(i)
#define NC_R_TYPE(i) ELF32_R_TYPE(i)
#endif

/* relocation types that hold a function/data address in the GOT */
#if defined(__arm__)
#define NC_IS_GOT_RELOC(t) ((t) == 22 /*JUMP_SLOT*/ || (t) == 21 /*GLOB_DAT*/ || (t) == 2 /*ABS32*/)
#elif defined(__aarch64__)
#define NC_IS_GOT_RELOC(t) ((t) == 1026 /*JUMP_SLOT*/ || (t) == 1025 /*GLOB_DAT*/ || (t) == 257 /*ABS64*/)
#elif defined(__x86_64__)
#define NC_IS_GOT_RELOC(t) ((t) == 7 /*JUMP_SLOT*/ || (t) == 6 /*GLOB_DAT*/ || (t) == 1 /*64*/)
#else
#define NC_IS_GOT_RELOC(t) ((t) == 7 || (t) == 6)
#endif

struct find_ctx { const char *lib; ElfW(Addr) bias; ElfW(Dyn) *dyn; int found; };

static int find_cb(struct dl_phdr_info *info, size_t size, void *data) {
    struct find_ctx *c = (struct find_ctx *)data;
    (void)size;
    if (!info->dlpi_name || !strstr(info->dlpi_name, c->lib)) return 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            c->bias = info->dlpi_addr;
            c->dyn = (ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            c->found = 1;
            return 1;
        }
    }
    return 0;
}

/* Some loaders leave dynamic pointers as link-time offsets, others relocate them. Handle both. */
static uintptr_t fix(uintptr_t p, uintptr_t bias) { return p < bias ? p + bias : p; }

static int patch_table(const char *symbol, void *repl, void **orig, uintptr_t bias,
                       const ElfW(Sym) *symtab, const char *strtab,
                       uintptr_t table, size_t bytes, int is_rela) {
    int patched = 0;
    size_t ent = is_rela ? sizeof(ElfW(Rela)) : sizeof(ElfW(Rel));
    for (size_t off = 0; off + ent <= bytes; off += ent) {
        ElfW(Addr) r_offset; uint64_t r_info;
        if (is_rela) { const ElfW(Rela) *r = (const ElfW(Rela) *)(table + off); r_offset = r->r_offset; r_info = r->r_info; }
        else         { const ElfW(Rel)  *r = (const ElfW(Rel)  *)(table + off); r_offset = r->r_offset; r_info = r->r_info; }
        if (!NC_IS_GOT_RELOC(NC_R_TYPE(r_info))) continue;
        const ElfW(Sym) *s = &symtab[NC_R_SYM(r_info)];
        if (!s->st_name || strcmp(strtab + s->st_name, symbol) != 0) continue;

        void **slot = (void **)(bias + r_offset);
        long page = sysconf(_SC_PAGESIZE);
        uintptr_t start = (uintptr_t)slot & ~(uintptr_t)(page - 1);
        if (mprotect((void *)start, (size_t)page, PROT_READ | PROT_WRITE) != 0) continue;
        if (*slot == repl) { patched++; continue; }
        if (orig && !*orig) *orig = *slot;        /* remember the original BEFORE patching */
        *slot = repl;
        patched++;
    }
    return patched;
}

int nc_got_hook(const char *lib_part, const char *symbol, void *replacement, void **original) {
    struct find_ctx ctx = { lib_part, 0, 0, 0 };
    dl_iterate_phdr(find_cb, &ctx);
    if (!ctx.found) return -1;

    uintptr_t bias = (uintptr_t)ctx.bias;
    uintptr_t symtab = 0, strtab = 0, jmprel = 0, rel = 0;
    size_t jmprelsz = 0, relsz = 0;
    int pltrel = DT_REL;
    for (ElfW(Dyn) *d = ctx.dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB:   symtab = fix(d->d_un.d_ptr, bias); break;
        case DT_STRTAB:   strtab = fix(d->d_un.d_ptr, bias); break;
        case DT_JMPREL:   jmprel = fix(d->d_un.d_ptr, bias); break;
        case DT_PLTRELSZ: jmprelsz = d->d_un.d_val; break;
        case DT_PLTREL:   pltrel = (int)d->d_un.d_val; break;
#if defined(__LP64__) || defined(__x86_64__)
        case DT_RELA:     rel = fix(d->d_un.d_ptr, bias); break;
        case DT_RELASZ:   relsz = d->d_un.d_val; break;
#else
        case DT_REL:      rel = fix(d->d_un.d_ptr, bias); break;
        case DT_RELSZ:    relsz = d->d_un.d_val; break;
#endif
        }
    }
    if (!symtab || !strtab) return 0;

    int n = 0;
    if (jmprel && jmprelsz)
        n += patch_table(symbol, replacement, original, bias, (const ElfW(Sym) *)symtab, (const char *)strtab,
                         jmprel, jmprelsz, pltrel == DT_RELA);
    if (rel && relsz)
        n += patch_table(symbol, replacement, original, bias, (const ElfW(Sym) *)symtab, (const char *)strtab,
                         rel, relsz,
#if defined(__LP64__) || defined(__x86_64__)
                         1
#else
                         0
#endif
                         );
    return n;
}
