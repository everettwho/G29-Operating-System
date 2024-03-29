/* kernel.c - the C part of the kernel
 * vim:ts=4 noexpandtab
 */

#include "multiboot.h"
#include "x86_desc.h"
#include "lib.h"
#include "i8259.h"
#include "debug.h"
#include "rtc.h"
#include "exceptions.h"
#include "keyboard.h"
#include "paging.h"

extern uint32_t keyboard_wrapper;
extern uint32_t rtc_wrapper;

/* Macros. */
/* Check if the bit BIT in FLAGS is set. */
#define CHECK_FLAG(flags,bit)   ((flags) & (1 << (bit)))

/* Check if MAGIC is valid and print the Multiboot information structure
   pointed by ADDR. */
void
entry (unsigned long magic, unsigned long addr)
{
	multiboot_info_t *mbi;

	/* Clear the screen. */
	clear();

	paging_init();

	/* Am I booted by a Multiboot-compliant boot loader? */
	if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
	{
		printf ("Invalid magic number: 0x%#x\n", (unsigned) magic);
		return;
	}

	/* Set MBI to the address of the Multiboot information structure. */
	mbi = (multiboot_info_t *) addr;

	/* Print out the flags. */
	printf ("flags = 0x%#x\n", (unsigned) mbi->flags);

	/* Are mem_* valid? */
	if (CHECK_FLAG (mbi->flags, 0))
		printf ("mem_lower = %uKB, mem_upper = %uKB\n",
				(unsigned) mbi->mem_lower, (unsigned) mbi->mem_upper);

	/* Is boot_device valid? */
	if (CHECK_FLAG (mbi->flags, 1))
		printf ("boot_device = 0x%#x\n", (unsigned) mbi->boot_device);

	/* Is the command line passed? */
	if (CHECK_FLAG (mbi->flags, 2))
		printf ("cmdline = %s\n", (char *) mbi->cmdline);

	if (CHECK_FLAG (mbi->flags, 3)) {
		int mod_count = 0;
		int i;
		module_t* mod = (module_t*)mbi->mods_addr;
		while(mod_count < mbi->mods_count) {
			printf("Module %d loaded at address: 0x%#x\n", mod_count, (unsigned int)mod->mod_start);
			printf("Module %d ends at address: 0x%#x\n", mod_count, (unsigned int)mod->mod_end);
			printf("First few bytes of module:\n");
			for(i = 0; i<16; i++) {
				printf("0x%x ", *((char*)(mod->mod_start+i)));
			}
			printf("\n");
			mod_count++;
		}
	}
	/* Bits 4 and 5 are mutually exclusive! */
	if (CHECK_FLAG (mbi->flags, 4) && CHECK_FLAG (mbi->flags, 5))
	{
		printf ("Both bits 4 and 5 are set.\n");
		return;
	}

	/* Is the section header table of ELF valid? */
	if (CHECK_FLAG (mbi->flags, 5))
	{
		elf_section_header_table_t *elf_sec = &(mbi->elf_sec);

		printf ("elf_sec: num = %u, size = 0x%#x,"
				" addr = 0x%#x, shndx = 0x%#x\n",
				(unsigned) elf_sec->num, (unsigned) elf_sec->size,
				(unsigned) elf_sec->addr, (unsigned) elf_sec->shndx);
	}

	/* Are mmap_* valid? */
	if (CHECK_FLAG (mbi->flags, 6))
	{
		memory_map_t *mmap;

		printf ("mmap_addr = 0x%#x, mmap_length = 0x%x\n",
				(unsigned) mbi->mmap_addr, (unsigned) mbi->mmap_length);
		for (mmap = (memory_map_t *) mbi->mmap_addr;
				(unsigned long) mmap < mbi->mmap_addr + mbi->mmap_length;
				mmap = (memory_map_t *) ((unsigned long) mmap
					+ mmap->size + sizeof (mmap->size)))
			printf (" size = 0x%x,     base_addr = 0x%#x%#x\n"
					"     type = 0x%x,  length    = 0x%#x%#x\n",
					(unsigned) mmap->size,
					(unsigned) mmap->base_addr_high,
					(unsigned) mmap->base_addr_low,
					(unsigned) mmap->type,
					(unsigned) mmap->length_high,
					(unsigned) mmap->length_low);
	}

	/* Construct an LDT entry in the GDT */
	{
		seg_desc_t the_ldt_desc;
		the_ldt_desc.granularity    = 0;
		the_ldt_desc.opsize         = 1;
		the_ldt_desc.reserved       = 0;
		the_ldt_desc.avail          = 0;
		the_ldt_desc.present        = 1;
		the_ldt_desc.dpl            = 0x0;
		the_ldt_desc.sys            = 0;
		the_ldt_desc.type           = 0x2;

		SET_LDT_PARAMS(the_ldt_desc, &ldt, ldt_size);
		ldt_desc_ptr = the_ldt_desc;
		lldt(KERNEL_LDT);
	}

	/* Construct a TSS entry in the GDT */
	{
		seg_desc_t the_tss_desc;
		the_tss_desc.granularity    = 0;
		the_tss_desc.opsize         = 0;
		the_tss_desc.reserved       = 0;
		the_tss_desc.avail          = 0;
		the_tss_desc.seg_lim_19_16  = TSS_SIZE & 0x000F0000;
		the_tss_desc.present        = 1;
		the_tss_desc.dpl            = 0x0;
		the_tss_desc.sys            = 0;
		the_tss_desc.type           = 0x9;
		the_tss_desc.seg_lim_15_00  = TSS_SIZE & 0x0000FFFF;

		SET_TSS_PARAMS(the_tss_desc, &tss, tss_size);

		tss_desc_ptr = the_tss_desc;

		tss.ldt_segment_selector = KERNEL_LDT;
		tss.ss0 = KERNEL_DS;
		tss.esp0 = 0x800000;
		ltr(KERNEL_TSS);
	}


	idt_desc_t interrupt;

	interrupt.seg_selector = KERNEL_CS;
	interrupt.dpl = 0;
	interrupt.size = 1;
	interrupt.reserved0 = 0;
	interrupt.reserved1 = 1;
	interrupt.reserved2 = 1;
	interrupt.reserved3 = 0;
	interrupt.reserved4 = 0;
	interrupt.present = 1;

	idt_desc_t divide_error_desc = interrupt;
	SET_IDT_ENTRY(divide_error_desc, divide_error);
	idt[0] = divide_error_desc;

	idt_desc_t reserved_desc = interrupt;
	SET_IDT_ENTRY(reserved_desc, reserved);
	idt[1] = reserved_desc;

	idt_desc_t nmi_interrupt_desc = interrupt;
	SET_IDT_ENTRY(nmi_interrupt_desc, nmi_interrupt);
	idt[2] = nmi_interrupt_desc;

	idt_desc_t breakpoint_desc = interrupt;
	SET_IDT_ENTRY(breakpoint_desc, breakpoint);
	idt[3] = breakpoint_desc;

	idt_desc_t overflow_desc = interrupt;
	SET_IDT_ENTRY(overflow_desc, overflow);
	idt[4] = overflow_desc;

	idt_desc_t bound_range_exceeded_desc = interrupt;
	SET_IDT_ENTRY(bound_range_exceeded_desc, bound_range_exceeded);
	idt[5] = bound_range_exceeded_desc;

	idt_desc_t invalid_opcode_desc = interrupt;
	SET_IDT_ENTRY(invalid_opcode_desc, invalid_opcode);
	idt[6] = invalid_opcode_desc;

	idt_desc_t device_not_available_desc = interrupt;
	SET_IDT_ENTRY(device_not_available_desc, device_not_available);
	idt[7] = device_not_available_desc;

	idt_desc_t double_fault_desc = interrupt;
	SET_IDT_ENTRY(double_fault_desc, double_fault);
	idt[8] = double_fault_desc;

	idt_desc_t coprocessor_segment_overrun_desc = interrupt;
	SET_IDT_ENTRY(coprocessor_segment_overrun_desc, coprocessor_segment_overrun);
	idt[9] = divide_error_desc;

	idt_desc_t invalid_tss_desc = interrupt;
	SET_IDT_ENTRY(invalid_tss_desc, invalid_tss);
	idt[10] = invalid_tss_desc;

	idt_desc_t segment_not_present_desc = interrupt;
	segment_not_present_desc.present = 0;
	SET_IDT_ENTRY(segment_not_present_desc, segment_not_present);
	idt[11] = segment_not_present_desc;

	idt_desc_t stack_segment_fault_desc = interrupt;
	SET_IDT_ENTRY(stack_segment_fault_desc, stack_segment_fault);
	idt[12] = stack_segment_fault_desc;

	idt_desc_t general_protection_desc = interrupt;
	SET_IDT_ENTRY(general_protection_desc, general_protection);
	idt[13] = general_protection_desc;

	idt_desc_t page_fault_desc = interrupt;
	SET_IDT_ENTRY(page_fault_desc, page_fault);
	idt[14] = page_fault_desc;

	idt_desc_t math_fault_desc = interrupt;
	SET_IDT_ENTRY(math_fault_desc, math_fault);
	idt[16] = math_fault_desc;

	idt_desc_t alignment_check_desc = interrupt;
	SET_IDT_ENTRY(alignment_check_desc, alignment_check);
	idt[17] = alignment_check_desc;

	idt_desc_t machine_check_desc = interrupt;
	SET_IDT_ENTRY(machine_check_desc, machine_check);
	idt[18] = machine_check_desc;

	idt_desc_t simd_desc = interrupt;
	SET_IDT_ENTRY(simd_desc, simd_floating_exception);
	idt[19] = simd_desc;

	idt_desc_t keyboard_desc = interrupt;
	SET_IDT_ENTRY(keyboard_desc, &keyboard_wrapper);
	idt[0x21] = keyboard_desc;

	idt_desc_t rtc_desc = interrupt;
	SET_IDT_ENTRY(rtc_desc, &rtc_wrapper);
	idt[0x28] = rtc_desc;

	// load idt
	lidt(idt_desc_ptr);

	/* Init the PIC */
	i8259_init();

	/* Initialize devices, memory, filesystem, enable device interrupts on the
	 * PIC, any other initialization stuff... */

	// initialize rtc
	rtc_init();

	// enable irq1 for keyboard
	 enable_irq(1);


	/* Enable interrupts */
	/* Do not enable the following until after you have set up your
	 * IDT correctly otherwise QEMU will triple fault and simple close
	 * without showing you any output */
	printf("Enabling Interrupts\n");
	sti();

	int freq = 0x0010;
	write_rtc((char *) (&freq));

	// while(1) {
	// 	printf("h");
	// 	read_rtc();
	// }

	// int x = 3 / 0;
	// int * x = 0x12345000;
	// int y;
	// y = *x;

	/* Execute the first program (`shell') ... */

	/* Spin (nicely, so we don't chew up cycles) */
	asm volatile(".1: hlt; jmp .1;");
}

