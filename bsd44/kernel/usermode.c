/*
 * usermode.c - enter ring 3 and run a static ELF32 image.
 *
 * This is the S1 bootstrap of userland: a private page directory that
 * shares the kernel's high mappings, ELF PT_LOAD pages mapped with
 * PG_U|PG_RW, a user stack, an int 0x80 syscall gate, and a minimal
 * write/exit dispatcher.  It deliberately avoids vm_map for now; when
 * vm_page/vm_map are initialised this code is replaced by the real
 * execve path.
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

extern const unsigned char hello_elf[];
extern const unsigned int hello_elf_len;
extern int _ucodesel, _udatasel;
extern int IdlePTD;
extern void enter_user(unsigned pd, unsigned entry, unsigned esp);
extern void syscall_entry(void);

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

	pmap_map((vm_offset_t)va, (vm_offset_t)pa, (vm_offset_t)pa + PAGE_SIZE,
	    VM_PROT_READ | VM_PROT_WRITE);
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
	/* share the kernel's high-half mappings (0xFE000000..0xFE3FFFFF) */
	for (i = 0x3f6; i <= 0x3fa; i++)
		pd[i] = ((unsigned *)kernel_pmap->pm_pdir)[i];
	return (pdpa);
}

static void
user_map(unsigned pdpa, unsigned va, unsigned pa)
{
	unsigned pdi = va >> 22, pti = (va >> 12) & 0x3ff;
	unsigned *pd = (unsigned *)SCRATCH_PD;
	unsigned *pt;
	unsigned ptpa;
	int i;

	(void)pdpa;
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
	unsigned phoff = eh[7];		/* e_phoff */
	unsigned phnum = *(const unsigned short *)(img + 0x2c);
	unsigned entryv = eh[6];		/* e_entry */
	unsigned i;

	*entry = entryv;
	for (i = 0; i < phnum; i++) {
		const unsigned *ph = (const unsigned *)(img + phoff + i * 32);
		unsigned type = ph[0];
		unsigned off = ph[1], vaddr = ph[2];
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
			user_map(pdpa, vaddr + n, pa);
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
		user_map(pdpa, va, pa);
	}
}

/* int 0x80 handler ------------------------------------------------------ */

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

void
syscall_dispatch(struct sysregs *r)
{
	unsigned n = r->eax;

	switch (n) {
	case 4: {			/* write(fd, buf, len) */
		unsigned fd = r->ebx, buf = r->ecx, len = r->edx;
		unsigned i;

		if (fd != 1 && fd != 2 || !user_ok(buf, len)) {
			r->eax = (unsigned)-1;
			break;
		}
			for (i = 0; i < len; i++)
			cnputc(((const char *)buf)[i]);
		r->eax = len;
		break;
	}
	case 1:				/* exit(code) */
		printf("\n[user exit %d]\n", r->ebx);
		for (;;)
			__asm__ volatile("cli; hlt");
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
	unsigned pd, entry;

	if (hello_elf_len == 0) {
		printf("no userland image embedded\n");
		return;
	}
	pd = user_pd_create();
	load_elf(pd, hello_elf, hello_elf_len, &entry);
	map_stack(pd);
	syscall_gate_init();
	printf("entering user mode: entry %x pd %x cs %x ds %x\n", entry, pd, _ucodesel, _udatasel);
	enter_user(pd, entry, USER_STACK_TOP - 16);
}
