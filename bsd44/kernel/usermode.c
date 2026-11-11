/*
 * usermode.c - S2: ramfs + ELF exec + int 0x80 syscalls.
 *
 * A deliberately small VFS: a read-only ramfs table built from the
 * userland images embedded in the kernel.  execve() builds a fresh page
 * directory for the new image and switches CR3 from the syscall path.
 * This is the bridge to the real 4.4BSD VFS/vm_map work later.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/proc.h>
#include <sys/user.h>

#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_kern.h>

#include <machine/cpu.h>
#include <machine/pmap.h>

#define	USER_LOAD	0x08048000
#define	USER_STACK_TOP	0xBFC00000
#define	USER_STACK_SIZE	0x8000

#define	SCRATCH_PD	0xFE200000
#define	SCRATCH_PT	0xFE201000
#define	SCRATCH_PAGE	0xFE202000

#define	NFD		16

extern const unsigned char hello_elf[], init_elf[], sh_elf[];
extern const unsigned int hello_elf_len, init_elf_len, sh_elf_len;
extern int _ucodesel, _udatasel;
extern void enter_user(unsigned pd, unsigned entry, unsigned esp);
extern void syscall_entry(void);
extern int cngetc(void);
extern void cnputc(int c);

struct file {
	const char		*f_name;
	const unsigned char	*f_data;
	const unsigned int	*f_len;
};

static struct file ramfs[] = {
	{ "/sbin/init",	init_elf,	&init_elf_len },
	{ "/bin/sh",	sh_elf,		&sh_elf_len },
	{ "/hello",	hello_elf,	&hello_elf_len },
	{ 0, 0, 0 }
};

struct fd {
	const unsigned char	*d;
	unsigned		len;
	unsigned		off;
	int			used;
};

static struct fd fds[NFD];
static unsigned next_pa = 0x200000;

static unsigned
page_alloc(void)
{
	unsigned pa = next_pa;

	next_pa += PAGE_SIZE;
	if (next_pa >= 0x3f000000)
		panic("usermode: out of physical memory");
	return (pa);
}

static void *
map_page(unsigned va, unsigned pa)
{

	pmap_map((vm_offset_t)va, (vm_offset_t)pa,
	    (vm_offset_t)pa + PAGE_SIZE, VM_PROT_READ | VM_PROT_WRITE);
	return ((void *)va);
}

static void
zero_page(unsigned pa)
{
	char *p = map_page(SCRATCH_PAGE, pa);
	int i;

	for (i = 0; i < PAGE_SIZE; i++)
		p[i] = 0;
}

static unsigned
user_pd_create(void)
{
	unsigned pdpa = page_alloc();
	unsigned *pd = map_page(SCRATCH_PD, pdpa);
	int i;

	for (i = 0; i < PAGE_SIZE / 4; i++)
		pd[i] = 0;
	/* share kernel high-half + proc0 kernel stack mappings */
	for (i = 0x3f6; i <= 0x3fa; i++)
		pd[i] = ((unsigned *)kernel_pmap->pm_pdir)[i];
	return (pdpa);
}

static void
user_map(unsigned va, unsigned pa)
{
	unsigned pdi = va >> 22, pti = (va >> 12) & 0x3ff;
	unsigned *pd = (unsigned *)SCRATCH_PD;
	unsigned *pt;
	unsigned ptpa;
	int i;

	if ((pd[pdi] & PG_V) == 0) {
		ptpa = page_alloc();
		pt = map_page(SCRATCH_PT, ptpa);
		for (i = 0; i < PAGE_SIZE / 4; i++)
			pt[i] = 0;
		pd[pdi] = ptpa | PG_V | PG_RW | PG_u;
	} else {
		ptpa = pd[pdi] & PG_FRAME;
		pt = map_page(SCRATCH_PT, ptpa);
	}
	pt[pti] = pa | PG_V | PG_RW | PG_u;
}

static void
load_elf(unsigned pdpa, const unsigned char *img, unsigned len, unsigned *entry)
{
	const unsigned *eh = (const unsigned *)img;
	unsigned phoff = eh[7];
	unsigned phnum = *(const unsigned short *)(img + 0x2c);
	unsigned i;

	*entry = eh[6];
	for (i = 0; i < phnum; i++) {
		const unsigned *ph = (const unsigned *)(img + phoff + i * 32);
		unsigned type = ph[0], off = ph[1], vaddr = ph[2];
		unsigned filesz = ph[4], memsz = ph[5];
		unsigned n;

		if (type != 1)
			continue;
		for (n = 0; n < memsz; n += PAGE_SIZE) {
			unsigned pa = page_alloc();
			char *dst;
			unsigned chunk = memsz - n;

			if (chunk > PAGE_SIZE)
				chunk = PAGE_SIZE;
			zero_page(pa);
			dst = map_page(SCRATCH_PAGE, pa);
			if (n < filesz) {
				unsigned c = filesz - n;

				if (c > chunk)
					c = chunk;
				bcopy(img + off + n, dst, c);
			}
			user_map(vaddr + n, pa);
		}
	}
}

static void
map_stack(unsigned pdpa)
{
	unsigned va, pa;

	for (va = USER_STACK_TOP - USER_STACK_SIZE; va < USER_STACK_TOP;
	    va += PAGE_SIZE) {
		pa = page_alloc();
		zero_page(pa);
		user_map(va, pa);
	}
}

static unsigned
build_image(const unsigned char *img, unsigned len, unsigned *entry)
{
	unsigned pd = user_pd_create();

	load_elf(pd, img, len, entry);
	map_stack(pd);
	return (pd);
}

/* ------------------------------------------------------------------ ramfs */

static int
user_str(unsigned p, char *dst, int max)
{
	int i;

	if (p < USER_LOAD || p >= 0xC0000000)
		return (-1);
	for (i = 0; i < max - 1; i++) {
		dst[i] = ((const char *)p)[i];
		if (dst[i] == 0)
			return (i);
	}
	dst[max - 1] = 0;
	return (-1);
}

static struct file *
fs_lookup(const char *name)
{
	struct file *f;

	for (f = ramfs; f->f_name; f++)
		if (!strcmp(f->f_name, name))
			return (f);
	return (0);
}

static int
fd_alloc(struct file *f)
{
	int i;

	for (i = 3; i < NFD; i++) {
		if (!fds[i].used) {
			fds[i].used = 1;
			fds[i].d = f->f_data;
			fds[i].len = *f->f_len;
			fds[i].off = 0;
			return (i);
		}
	}
	return (-1);
}

/* --------------------------------------------------------------- syscalls */

struct sysregs {
	unsigned ds, es, edi, esi, ebp, esp, ebx, edx, ecx, eax;
	unsigned eip, cs, eflags, uesp, ss;
};

static int
user_ok(unsigned p, unsigned len)
{

	if (p < USER_LOAD || p >= 0xC0000000)
		return (0);
	if (len > 0xC0000000 - p)
		return (0);
	return (1);
}

static int
exec_image(const char *path, struct sysregs *r)
{
	struct file *f;
	unsigned pd, entry;

	f = fs_lookup(path);
	if (f == 0)
		return (-1);
	pd = build_image(f->f_data, *f->f_len, &entry);
	__asm__ volatile("movl %0, %%cr3" :: "r"(pd) : "memory");
	r->eip = entry;
	r->uesp = USER_STACK_TOP - 16;
	r->eax = 0;
	return (0);
}

void
syscall_dispatch(struct sysregs *r)
{
	unsigned n = r->eax;

	switch (n) {
	case 4: {			/* write(fd, buf, len) */
		unsigned fd = r->ebx, buf = r->ecx, len = r->edx;
		unsigned i;

		if ((fd != 1 && fd != 2) || !user_ok(buf, len)) {
			r->eax = (unsigned)-1;
			break;
		}
		for (i = 0; i < len; i++)
			cnputc(((const char *)buf)[i]);
		r->eax = len;
		break;
	}
	case 3: {			/* read(fd, buf, len) */
		unsigned fd = r->ebx, buf = r->ecx, len = r->edx;
		unsigned i = 0;
		int c;

		if (!user_ok(buf, len) || len == 0) {
			r->eax = (unsigned)-1;
			break;
		}
		if (fd == 0) {
			while (i < len - 1) {
				c = cngetc();
				if (c == 0)
					continue;
				if (c == '\r' || c == '\n') {
					cnputc('\n');
					((char *)buf)[i++] = '\n';
					break;
				}
				if (c == '\b') {
					if (i > 0) {
						i--;
						cnputc('\b');
						cnputc(' ');
						cnputc('\b');
					}
					continue;
				}
				cnputc(c);
				((char *)buf)[i++] = c;
			}
			r->eax = i;
			break;
		}
		if (fd >= NFD || !fds[fd].used) {
			r->eax = (unsigned)-1;
			break;
		}
		while (i < len && fds[fd].off < fds[fd].len)
			((char *)buf)[i++] = fds[fd].d[fds[fd].off++];
		r->eax = i;
		break;
	}
	case 5: {			/* open(path, flags, mode) */
		char path[128];
		struct file *f;
		int fd;

		if (user_str(r->ebx, path, sizeof(path)) < 0) {
			r->eax = (unsigned)-1;
			break;
		}
		f = fs_lookup(path);
		if (f == 0) {
			r->eax = (unsigned)-1;
			break;
		}
		fd = fd_alloc(f);
		r->eax = (fd < 0) ? (unsigned)-1 : (unsigned)fd;
		break;
	}
	case 6:				/* close(fd) */
		if (r->ebx < NFD)
			fds[r->ebx].used = 0;
		r->eax = 0;
		break;
	case 59: {			/* execve(path, argv, envp) */
		char path[128];

		if (user_str(r->ebx, path, sizeof(path)) < 0 ||
		    exec_image(path, r) < 0) {
			r->eax = (unsigned)-1;
			break;
		}
		break;
	}
	case 1:				/* exit(code) */
		printf("\n[user exit %d]\n", r->ebx);
		for (;;)
			__asm__ volatile("cli; hlt");
	case 17:			/* brk(addr) */
		r->eax = 0x09000000;
		break;
	default:
		r->eax = (unsigned)-1;
		break;
	}
}

void
syscall_gate_init(void)
{

	setidt(0x80, (char *)&syscall_entry, SDT_SYS386TGT, SEL_UPL);
}

void
start_userland(void)
{
	struct file *f;
	unsigned pd, entry;

	f = fs_lookup("/sbin/init");
	if (f == 0) {
		printf("ramfs: no /sbin/init\n");
		return;
	}
	pd = build_image(f->f_data, *f->f_len, &entry);
	syscall_gate_init();
	printf("exec /sbin/init: entry %x pd %x\n", entry, pd);
	enter_user(pd, entry, USER_STACK_TOP - 16);
}
