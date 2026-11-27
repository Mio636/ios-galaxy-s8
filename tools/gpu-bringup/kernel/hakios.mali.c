/*
 * HakiosMali for the Galaxy S8 (dreamlte, Exynos 8895), Mali-G71 MP20.
 *
 * The interface and the queue are adapted from /home/mio/s9-ref/core/drivers/
 * gpu/hakios.mali.c (Galaxy S9, Mali-G72 MP18), whose own header says it was
 * "written, not yet run on the phone". The GPU facts are the S8's, taken from
 * the verified local branch origin/claude/zealous-planck-oi3hse (boots 36-49):
 * GPU_PA 0x13900000, GPU_ID 0x60a00001, SHADER_PRESENT 0x17771777,
 * CMU_G3D 0x13800000, PMU G3D config/status 0x40e0/0x40e4, and register
 * polling only -- no GIC SPI for the GPU was found. Its WRITE_VALUE job used
 * the Mali legacy (ADRMODE_TABLE) page tables; that is the format kept here.
 *
 * Nothing is touched unless boot-arg mali=N is present. mali=9 runs all nine
 * steps and starts the HakiosMali IOKit service for the S9-derived Mesa
 * 25.0.7 panfrost stack. Lower N stops after that step; 0 or absent leaves the
 * GPU alone. The PMIC rail and PLL are NOT written: S-Boot's voltage and clock
 * are used as found. If the G3D domain is off, step 2 applies only the S8
 * g3d_on register sequence; if that cannot bring it up, the run stops before
 * GPU_ID is read.
 *
 * Never combine mali=9 with S8GPU (s8gpu=4) or the /dev/s8mali interface
 * (sweetfb_gpu=1, s8metal=1). They are separate, incompatible GPU paths.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>

#include "hakios.mali_va.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern vm_offset_t phystokv(uint64_t pa);
extern boolean_t pmap_valid_address(uint64_t addr);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern void HakiosMaliStart(void);

#define LOG(fmt, ...) printf("HakiosMali: " fmt "\n", ##__VA_ARGS__)

#if defined(DREAMLTE) && defined(ARM64_BOARD_CONFIG_A5XELTE)

/* ---- the S8's addresses (verified on the branch, not guessed) ---- */
#define PMU_PHYS                0x16480000UL
#define PMU_SIZE                0x100000
#define PMU_G3D_CONFIGURATION   0x40e0
#define PMU_G3D_STATUS          0x40e4
#define G3D_ON                  0xfu
#define G3D_WAIT_US             100000u

#define CMU_G3D_PHYS            0x13800000UL
#define CMU_G3D_SIZE            0x100000
#define PLL_CON0                0x140
#define PLL_CON2                0x148
#define PLL_ENABLE              (1u << 31)
#define PLL_STABLE              (1u << 29)
#define MUX_SEL                 (1u << 4)
#define MUX_G3D_USER            0x100
#define MUX_CLK_G3D_BUSD        0x1000
#define DIV_CLK_G3D_BUSD        0x1800
#define DIV_CLK_G3D_BUSP        0x1804

#define MALI_PHYS               0x13900000UL
#define MALI_SIZE               0x100000
#define MALI_G71                0x60a0u          /* GPU_ID >> 16 of 0x60a00001 */
#define SHADER_PRESENT_S8       0x17771777u      /* 20 shader cores, branch boot 36 */

/* ---- panfrost_regs.h ---- */
#define GPU_ID                  0x000
#define GPU_L2_FEATURES         0x004
#define GPU_CORE_FEATURES       0x008
#define GPU_TILER_FEATURES      0x00c
#define GPU_MEM_FEATURES        0x010
#define GPU_MMU_FEATURES        0x014
#define GPU_AS_PRESENT          0x018
#define GPU_JS_PRESENT          0x01c
#define GPU_INT_RAWSTAT         0x020
#define GPU_INT_CLEAR           0x024
#define GPU_INT_MASK            0x028
#define GPU_CMD                 0x030
#define THREAD_MAX_THREADS      0x0a0
#define THREAD_TLS_ALLOC        0x310
#define SHADER_PRESENT          0x100
#define SHADER_PRESENT_HI       0x104
#define TILER_PRESENT           0x110
#define L2_PRESENT              0x120
#define SHADER_READY            0x140
#define TILER_READY             0x150
#define L2_READY                0x160
#define SHADER_PWRON            0x180
#define TILER_PWRON             0x190
#define L2_PWRON                0x1a0
#define COHERENCY_ENABLE        0x304
#define COHERENCY_NONE          31u
#define GPU_L2_MMU_CONFIG       0xf0c
#define L2_MMU_LIMIT_EXTERNAL   (0xfu << 24)
#define IRQ_RESET_COMPLETED     (1u << 8)
#define GPU_CMD_SOFT_RESET      1u
#define GPU_CMD_HARD_RESET      2u
#define RESET_WAIT_US           100000u
#define CORES_WAIT_US           50000u

#define MMU_INT_RAWSTAT         0x2000
#define MMU_INT_CLEAR           0x2004
#define JOB_INT_RAWSTAT         0x1000
#define JOB_INT_CLEAR           0x1004
#define JOB_INT_MASK            0x1008
#define JS0                     0x1800
#define JS1                     0x1880
#define JS_STATUS               0x24
#define JS_HEAD_NEXT_LO         0x40
#define JS_HEAD_NEXT_HI         0x44
#define JS_AFFINITY_NEXT_LO     0x50
#define JS_AFFINITY_NEXT_HI     0x54
#define JS_CONFIG_NEXT          0x58
#define JS_COMMAND_NEXT         0x60
#define JS_CONFIG               ((3u << 8) | (3u << 12) | (8u << 16))

/* ---- address space 0: Mali legacy LPAE, the S8's proven format ---- */
#define AS0                     0x2400
#define AS_TRANSTAB_LO          0x00
#define AS_TRANSTAB_HI          0x04
#define AS_MEMATTR_LO           0x08
#define AS_MEMATTR_HI           0x0c
#define AS_COMMAND              0x18
#define AS_FAULTSTATUS          0x1c
#define AS_STATUS               0x28
#define AS_TRANSCFG_LO          0x30
#define AS_TRANSCFG_HI          0x34
#define AS_ACTIVE               1u
#define AS_COMMAND_UPDATE       1u
#define AS_COMMAND_FLUSH_MEM    5u
#define AS_WAIT_US              50000u

#define TRANSTAB_TABLE          3u
#define TRANSTAB_READ_INNER     (1u << 2)
#define MEMATTR_LEGACY_LO       0x008d8888u     /* LinuxS8's panfrost job: attr 0 / inner 0x8d / outer 0x88 */
#define TRANSCFG_LEGACY         0u
#define TRANSCFG_WALK_NC        (1u << 24)      /* mali_fastmap: do not cache table walks */
#define PTE_TABLE               3ull
#define PTE_PAGE                (1ull | (3ull << 6) | (2ull << 8))
#define PTE_ADDRESS             0x0000fffffffff000ull
#define VA_LEVEL_SHIFT          39u             /* 4 x 512 entries, 48-bit GPU VA */
#define VA_L0_SHIFT             (VA_LEVEL_SHIFT)
#define VA_L1_SHIFT             (VA_LEVEL_SHIFT - 9u)
#define VA_L2_SHIFT             (VA_LEVEL_SHIFT - 18u)
#define VA_L3_SHIFT             (VA_LEVEL_SHIFT - 27u)
#define VA_BITS                 48u
#define PA_BITS_NEEDED          36u             /* S8 RAM reaches 0x8ffffffff */
#define FIRST_JOB_GPU           0x01000000ull   /* S8's JOB_VA */
#define SECOND_JOB_GPU          0x01100000ull

/* ---- state ---- */
SYSCTL_NODE(_kern, OID_AUTO, hakios, CTLFLAG_RW, 0, "Hakios");

static LCK_GRP_DECLARE(mali_lock_group, "hakios-mali");
static LCK_MTX_DECLARE(mali_lock, &mali_lock_group);
static LCK_MTX_DECLARE(mali_job_lock, &mali_lock_group);
static LCK_MTX_DECLARE(mali_queue_lock, &mali_lock_group);

static vm_offset_t pmu, cmu_g3d, mali;
static uint32_t last_step, reached, gpu_id, shader_mask, l2_mask;
static uint64_t *pt_l0;
static uint64_t pt_l0_phys, pt_transtab;
static vm_offset_t job_page;
static uint32_t pt_refused;
static int mali_fastmap, mali_trace;

static struct mali_va mali_va_state;
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali, CTLFLAG_RD | CTLFLAG_LOCKED, &reached, 0,
    "how far the S8 Mali came up: 9 is the GPU there for programs");
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_id, CTLFLAG_RD | CTLFLAG_LOCKED, &gpu_id, 0, "GPU_ID");

uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed, gMaliShown;
struct mali_made { uint64_t address, size; uint32_t asked, flags; };
struct mali_made gMaliMade[32];

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
wait_for(vm_offset_t base, uint32_t at, uint32_t mask, uint32_t want, uint32_t us)
{
	uint64_t deadline;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &deadline);
	deadline += mach_absolute_time();
	while ((rd(base, at) & mask) != want) {
		if (mach_absolute_time() >= deadline) {
			return (rd(base, at) & mask) == want;
		}
	}
	return true;
}

static inline uint32_t
pt_transcfg(void)
{
	return mali_fastmap ? TRANSCFG_WALK_NC : TRANSCFG_LEGACY;
}

static uint64_t *
table_of(uint64_t entry)
{
	uint64_t phys = entry & PTE_ADDRESS;

	if (!(entry & 1) || phys == 0 || !pmap_valid_address(phys)) {
		if (pt_refused++ < 4) {
			LOG("page-table entry %#llx is not kernel memory; not followed", entry);
		}
		return NULL;
	}
	return (uint64_t *)phystokv(phys);
}

static uint64_t *
pt_page(uint64_t *phys)
{
	vm_offset_t page = 0;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return NULL;
	}
	*phys = kvtophys(page);
	if (*phys == 0 || !pmap_valid_address(*phys)) {
		kmem_free(kernel_map, page, PAGE_SIZE);
		return NULL;
	}
	FlushPoC_DcacheRegion(page, PAGE_SIZE);
	return (uint64_t *)page;
}

/* The 512-entry table below `entry`, made as a zeroed page if there is none. */
static uint64_t *
pt_below(uint64_t *entry)
{
	uint64_t phys;
	uint64_t *table;

	if (*entry & 1) {
		return table_of(*entry);
	}
	table = pt_page(&phys);
	if (table == NULL) {
		return NULL;
	}
	*entry = phys | PTE_TABLE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return table;
}

static bool
map_page(uint64_t gpu, uint64_t phys)
{
	uint64_t *l1, *l2, *l3, *entry;

	if (gpu >= (1ull << VA_BITS) || phys == 0 || !pmap_valid_address(phys)) {
		return false;
	}
	if (pt_l0 == NULL && (pt_l0 = pt_page(&pt_l0_phys)) == NULL) {
		return false;
	}
	/* Four 9-bit levels cover a 48-bit GPU VA: L0 at bit 39, then 30, 21, 12. */
	if ((l1 = pt_below(&pt_l0[(gpu >> VA_L0_SHIFT) & 511])) == NULL ||
	    (l2 = pt_below(&l1[(gpu >> VA_L1_SHIFT) & 511])) == NULL ||
	    (l3 = pt_below(&l2[(gpu >> VA_L2_SHIFT) & 511])) == NULL) {
		return false;
	}
	entry = &l3[(gpu >> VA_L3_SHIFT) & 511];
	*entry = (phys & PTE_ADDRESS) | PTE_PAGE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	pt_transtab = pt_l0_phys | TRANSTAB_READ_INNER | TRANSTAB_TABLE;
	return true;
}

static void
unmap_pages(uint64_t gpu, uint64_t size)
{
	uint64_t at;

	if (pt_l0 == NULL) {
		return;
	}
	for (at = gpu; at < gpu + size; at += 4096) {
		uint64_t *l1 = table_of(pt_l0[(at >> VA_L0_SHIFT) & 511]);
		uint64_t *l2, *l3, *entry;

		if (l1 == NULL || (l2 = table_of(l1[(at >> VA_L1_SHIFT) & 511])) == NULL ||
		    (l3 = table_of(l2[(at >> VA_L2_SHIFT) & 511])) == NULL) {
			continue;
		}
		entry = &l3[(at >> VA_L3_SHIFT) & 511];
		*entry = 0;
		FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	}
}

static void
as0_update(void)
{
	(void)wait_for(mali, AS0 + AS_STATUS, AS_ACTIVE, 0, AS_WAIT_US);
	wr(mali, AS0 + AS_COMMAND, AS_COMMAND_FLUSH_MEM);
	(void)wait_for(mali, AS0 + AS_STATUS, AS_ACTIVE, 0, AS_WAIT_US);
	wr(mali, AS0 + AS_TRANSTAB_LO, (uint32_t)pt_transtab);
	wr(mali, AS0 + AS_TRANSTAB_HI, (uint32_t)(pt_transtab >> 32));
	wr(mali, AS0 + AS_MEMATTR_LO, MEMATTR_LEGACY_LO);
	wr(mali, AS0 + AS_MEMATTR_HI, 0);
	wr(mali, AS0 + AS_TRANSCFG_LO, pt_transcfg());
	wr(mali, AS0 + AS_TRANSCFG_HI, 0);
	wr(mali, AS0 + AS_COMMAND, AS_COMMAND_UPDATE);
	(void)wait_for(mali, AS0 + AS_STATUS, AS_ACTIVE, 0, AS_WAIT_US);
}

static bool
gpu_reset(void)
{
	wr(mali, GPU_INT_MASK, 0);
	wr(mali, GPU_INT_CLEAR, IRQ_RESET_COMPLETED);
	wr(mali, GPU_CMD, GPU_CMD_SOFT_RESET);
	if (!wait_for(mali, GPU_INT_RAWSTAT, IRQ_RESET_COMPLETED, IRQ_RESET_COMPLETED, RESET_WAIT_US)) {
		LOG("soft reset did not finish (raw %#x); hard reset", rd(mali, GPU_INT_RAWSTAT));
		wr(mali, GPU_CMD, GPU_CMD_HARD_RESET);
		if (!wait_for(mali, GPU_INT_RAWSTAT, IRQ_RESET_COMPLETED, IRQ_RESET_COMPLETED, RESET_WAIT_US)) {
			return false;
		}
	}
	wr(mali, GPU_INT_CLEAR, 0xffffffffu);
	wr(mali, MMU_INT_CLEAR, 0xffffffffu);
	wr(mali, COHERENCY_ENABLE, COHERENCY_NONE);
	wr(mali, GPU_L2_MMU_CONFIG, rd(mali, GPU_L2_MMU_CONFIG) & ~L2_MMU_LIMIT_EXTERNAL);
	return true;
}

static bool
gpu_cores_on(const char **which)
{
	uint32_t tiler = rd(mali, TILER_PRESENT);

	wr(mali, L2_PWRON, l2_mask);
	if (!wait_for(mali, L2_READY, l2_mask, l2_mask, CORES_WAIT_US)) {
		*which = "L2";
		return false;
	}
	wr(mali, SHADER_PWRON, shader_mask);
	if (!wait_for(mali, SHADER_READY, shader_mask, shader_mask, CORES_WAIT_US)) {
		*which = "shader cores";
		return false;
	}
	wr(mali, TILER_PWRON, tiler);
	if (!wait_for(mali, TILER_READY, tiler, tiler, CORES_WAIT_US)) {
		*which = "tiler";
		return false;
	}
	return true;
}

/* A WRITE_VALUE job at `gpu`, storing into the second half of the same page. */
static bool
run_write_job(vm_offset_t page, uint64_t gpu, const char *what)
{
	volatile uint32_t *job = (volatile uint32_t *)page;
	volatile uint32_t *target = (volatile uint32_t *)(page + 0x800);
	bool ok;

	bzero((void *)page, PAGE_SIZE);
	job[4] = 1u | (2u << 1) | (1u << 16);
	job[8] = (uint32_t)(gpu + 0x800);
	job[9] = (uint32_t)((gpu + 0x800) >> 32);
	job[10] = 6;                    /* payload: immediate 32 */
	job[12] = 0xc0ffee01u;
	FlushPoC_DcacheRegion(page, PAGE_SIZE);

	wr(mali, JOB_INT_MASK, 0);
	wr(mali, JOB_INT_CLEAR, 0xffffffffu);
	wr(mali, MMU_INT_CLEAR, 0xffffffffu);
	wr(mali, JS1 + JS_HEAD_NEXT_LO, (uint32_t)gpu);
	wr(mali, JS1 + JS_HEAD_NEXT_HI, (uint32_t)(gpu >> 32));
	wr(mali, JS1 + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(mali, JS1 + JS_AFFINITY_NEXT_HI, 0);
	wr(mali, JS1 + JS_CONFIG_NEXT, JS_CONFIG);
	wr(mali, JS1 + JS_COMMAND_NEXT, 1);
	(void)wait_for(mali, JOB_INT_RAWSTAT, (1u << 1) | (1u << 17), 1u << 1, 1000000);

	FlushPoC_DcacheRegion(page, PAGE_SIZE);
	ok = *target == 0xc0ffee01u;
	LOG("%s: raw %#x, slot status %#x, MMU raw %#x, fault %#x, word %#x (%s)", what,
	    rd(mali, JOB_INT_RAWSTAT), rd(mali, JS1 + JS_STATUS), rd(mali, MMU_INT_RAWSTAT),
	    rd(mali, AS0 + AS_FAULTSTATUS), *target, ok ? "THE GPU WROTE IT" : "NOT WRITTEN");
	wr(mali, JOB_INT_CLEAR, 0xffffffffu);
	wr(mali, MMU_INT_CLEAR, 0xffffffffu);
	return ok;
}

/* ---- the nine steps ---- */

static bool
step_look(void)
{
	uint32_t pll, m, p, s;

	pmu = ml_io_map(PMU_PHYS, PMU_SIZE);
	cmu_g3d = ml_io_map(CMU_G3D_PHYS, CMU_G3D_SIZE);
	mali = ml_io_map(MALI_PHYS, MALI_SIZE);
	if (pmu == 0 || cmu_g3d == 0 || mali == 0) {
		LOG("step 1: could not map PMU/CMU_G3D/GPU");
		return false;
	}
	pll = rd(cmu_g3d, PLL_CON0);
	m = (pll >> 16) & 0x3ff;
	p = (pll >> 8) & 0x3f;
	s = pll & 7;
	LOG("step 1: read only; PD_G3D config %#x status %#x, PLL_CON0 %#x (%u.%03u MHz, %s%s)",
	    rd(pmu, PMU_G3D_CONFIGURATION), rd(pmu, PMU_G3D_STATUS), pll,
	    p ? (unsigned)(26ULL * m / ((uint64_t)p << s)) : 0,
	    p ? (unsigned)(26000ULL * m / ((uint64_t)p << s) % 1000) : 0,
	    (pll & PLL_ENABLE) ? "enabled" : "disabled", (pll & PLL_STABLE) ? "" : ", NOT STABLE");
	return true;
}

static bool
step_power(void)
{
	uint32_t status = rd(pmu, PMU_G3D_STATUS);
	uint32_t pll = rd(cmu_g3d, PLL_CON0);

	if ((pll & (PLL_ENABLE | PLL_STABLE)) != (PLL_ENABLE | PLL_STABLE)) {
		LOG("step 2: PLL_G3D is not running; the boot clock is not usable, nothing written");
		return false;
	}
	if ((status & G3D_ON) == G3D_ON) {
		LOG("step 2: G3D domain already on; rail and clock left as S-Boot set them");
		return true;
	}
	LOG("step 2: G3D domain off; applying the S8 g3d_on register sequence (no PMIC write)");
	wr(cmu_g3d, PLL_CON2, 0x30000003);
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) | MUX_SEL);
	wr(cmu_g3d, MUX_G3D_USER, rd(cmu_g3d, MUX_G3D_USER) | MUX_SEL);
	wr(pmu, PMU_G3D_CONFIGURATION, (rd(pmu, PMU_G3D_CONFIGURATION) & ~G3D_ON) | G3D_ON);
	if (!wait_for(pmu, PMU_G3D_STATUS, G3D_ON, G3D_ON, G3D_WAIT_US)) {
		LOG("step 2: G3D did not come up (status %#x); the GPU is not read", rd(pmu, PMU_G3D_STATUS));
		return false;
	}
	LOG("step 2: G3D on (status %#x)", rd(pmu, PMU_G3D_STATUS));
	return true;
}

static bool
step_bus(void)
{
	LOG("step 3: bus dividers BUSD mux %#x BUSD %#x BUSP %#x; no SMC/DTZPC call is made",
	    rd(cmu_g3d, MUX_CLK_G3D_BUSD), rd(cmu_g3d, DIV_CLK_G3D_BUSD), rd(cmu_g3d, DIV_CLK_G3D_BUSP));
	return true;
}

static bool
step_gpu(void)
{
	uint32_t mmu, va_bits, pa_bits, shaders_hi, as_present, js_present;

	gpu_id = rd(mali, GPU_ID);
	LOG("step 4: GPU_ID %#x (arch %u.%u, product %#x), expected product %#x", gpu_id, gpu_id >> 28,
	    (gpu_id >> 24) & 0xf, (gpu_id >> 16) & 0xffff, MALI_G71);
	if ((gpu_id >> 16) != MALI_G71) {
		LOG("step 4: not the S8's Mali-G71; nothing more is done");
		return false;
	}
	mmu = rd(mali, GPU_MMU_FEATURES);
	va_bits = mmu & 0xff;
	pa_bits = (mmu >> 8) & 0xff;
	LOG("step 4: MMU_FEATURES %#x (%u VA bits, %u PA bits)", mmu, va_bits, pa_bits);
	if (va_bits < VA_BITS || pa_bits < PA_BITS_NEEDED) {
		LOG("step 4: address bits are too few for the S8's page tables; stopping");
		return false;
	}
	shader_mask = rd(mali, SHADER_PRESENT);
	shaders_hi = rd(mali, SHADER_PRESENT_HI);
	l2_mask = rd(mali, L2_PRESENT);
	as_present = rd(mali, GPU_AS_PRESENT);
	js_present = rd(mali, GPU_JS_PRESENT);
	LOG("step 4: shaders %#x (expected %#x), hi %#x, tiler %#x, L2 %#x, AS %#x, JS %#x", shader_mask,
	    SHADER_PRESENT_S8, shaders_hi, rd(mali, TILER_PRESENT), l2_mask, as_present, js_present);
	if (shader_mask == 0 || shaders_hi != 0 || shader_mask != SHADER_PRESENT_S8) {
		LOG("step 4: shader mask is not the verified S8 mask; stopping");
		return false;
	}
	if (!(as_present & 1) || (js_present & 3) != 3 || rd(mali, TILER_PRESENT) == 0 || l2_mask == 0) {
		LOG("step 4: AS0, both job slots, tiler or L2 is missing; stopping");
		return false;
	}
	return true;
}

static bool
step_reset(void)
{
	if (!gpu_reset()) {
		LOG("step 5: reset did not finish (raw %#x)", rd(mali, GPU_INT_RAWSTAT));
		return false;
	}
	LOG("step 5: reset done; COHERENCY_ENABLE %#x, L2_MMU_CONFIG %#x", rd(mali, COHERENCY_ENABLE),
	    rd(mali, GPU_L2_MMU_CONFIG));
	return true;
}

static bool
step_cores(void)
{
	const char *which = NULL;

	if (!gpu_cores_on(&which)) {
		LOG("step 6: %s did not power up", which);
		return false;
	}
	LOG("step 6: %u shader cores, tiler %#x and L2 %#x powered", __builtin_popcount(shader_mask),
	    rd(mali, TILER_PRESENT), l2_mask);
	return true;
}

static bool
step_job_first(void)
{
	vm_offset_t phys;

	if (kmem_alloc(kernel_map, &job_page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		job_page = 0;
		LOG("step 7: no page for the first job");
		return false;
	}
	phys = kvtophys(job_page);
	if (!map_page(FIRST_JOB_GPU, phys)) {
		LOG("step 7: no memory for the S8 page tables");
		return false;
	}
	as0_update();
	return run_write_job(job_page, FIRST_JOB_GPU, "step 7: first job through the S8 tables");
}

static bool
step_job_tables(void)
{
	if (!map_page(SECOND_JOB_GPU, kvtophys(job_page))) {
		LOG("step 8: no memory for the second GPU address");
		return false;
	}
	as0_update();
	return run_write_job(job_page, SECOND_JOB_GPU, "step 8: second job, through the allocator's VA");
}

static bool
step_serve(void)
{
	reached = 9;
	LOG("step 9: starting HakiosMali for Mesa panfrost");
	HakiosMaliStart();
	return true;
}

static const struct {
	const char *name;
	bool (*make)(void);
} steps[] = {
	{ "read what the boot left", step_look },
	{ "G3D power domain", step_power },
	{ "bus dividers (read only)", step_bus },
	{ "GPU_ID and capabilities", step_gpu },
	{ "reset", step_reset },
	{ "cores", step_cores },
	{ "first S8 WRITE_VALUE job", step_job_first },
	{ "second job through a second GPU VA", step_job_tables },
	{ "HakiosMali service", step_serve },
};
#define STEPS ((uint32_t)(sizeof(steps) / sizeof(steps[0])))

/* ---- the interface HakiosMaliUserClient uses ---- */

bool
hakios_mali_ready(void)
{
	return reached >= STEPS;
}

bool
hakios_mali_parameter(uint32_t which, uint64_t *value)
{
	static const uint16_t simple[] = {
		[5] = 0xe00, [6] = 0x018, [7] = 0x01c, [8] = 0x004, [9] = 0x008, [10] = 0x00c,
		[11] = 0x010, [12] = 0x014, [13] = 0x0ac, [14] = 0x0a0, [15] = 0x0a4, [16] = 0x0a8,
		[17] = 0x300, [18] = 0x0b0, [19] = 0x0b4, [20] = 0x0b8, [21] = 0x0bc,
	};

	if (!hakios_mali_ready()) {
		return false;
	}
	switch (which) {
	case 0:
		*value = gpu_id >> 16;
		return true;
	case 1:
		*value = gpu_id & 0xffff;
		return true;
	case 2: case 3: case 4:
		*value = rd(mali, 0x100 + (which - 2) * 0x10) | ((uint64_t)rd(mali, 0x104 + (which - 2) * 0x10) << 32);
		return true;
	case 38:
		*value = __builtin_popcount(l2_mask) != 0 ? (uint64_t)__builtin_popcount(l2_mask) : 1;
		return true;
	case 39:
		*value = rd(mali, THREAD_TLS_ALLOC);
		if (*value < rd(mali, THREAD_MAX_THREADS)) {
			*value = rd(mali, THREAD_MAX_THREADS);
		}
		return true;
	case 40:
		*value = 0;             /* Mesa reads AFBC_FEATURES 0 as AFBC supported on Bifrost */
		return true;
	default:
		if (which >= 22 && which <= 37) {
			*value = rd(mali, 0x0c0 + (which - 22) * 4);
			return true;
		}
		if (which < sizeof(simple) / sizeof(simple[0]) && simple[which] != 0) {
			*value = rd(mali, simple[which]);
			return true;
		}
		return false;
	}
}

uint64_t
hakios_mali_address(uint64_t size, bool executable)
{
	uint64_t address;

	lck_mtx_lock(&mali_lock);
	address = mali_va_alloc(&mali_va_state, size, executable);
	if (address != 0 && gMaliBuffers < 32) {
		gMaliMade[gMaliBuffers].address = address;
		gMaliMade[gMaliBuffers].size = size;
		gMaliMade[gMaliBuffers].asked = (uint32_t)size;
		gMaliMade[gMaliBuffers].flags = executable ? 0 : 1;
	}
	lck_mtx_unlock(&mali_lock);
	return address;
}

bool
hakios_mali_map(uint64_t gpu, uint64_t phys)
{
	bool ok;

	lck_mtx_lock(&mali_lock);
	ok = map_page(gpu, phys);
	lck_mtx_unlock(&mali_lock);
	return ok;
}

void
hakios_mali_mapped(void)
{
	if (mali_fastmap) {
		return;         /* TRANSCFG_WALK_NC: the GPU does not cache the tables */
	}
	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	as0_update();
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
}

void
hakios_mali_unmap(uint64_t gpu, uint64_t size)
{
	lck_mtx_lock(&mali_lock);
	unmap_pages(gpu, size);
	lck_mtx_unlock(&mali_lock);

	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	as0_update();
	(void)mali_va_free(&mali_va_state, gpu, size);
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
}

void
hakios_mali_tag(__unused uint64_t address, __unused uint32_t kind, __unused uint32_t width,
    __unused uint32_t height, __unused uint32_t row_bytes, __unused uint32_t format,
    __unused uint32_t offset)
{
	/* This bring-up client has no IOSurface/display integration. */
}

uint32_t
hakios_mali_submit(uint64_t chain, bool fragment)
{
	vm_offset_t js = fragment ? JS0 : JS1;
	uint32_t slot = fragment ? 0 : 1;
	uint32_t status = 0;
	bool finished;

	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	wr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));
	wr(mali, js + JS_HEAD_NEXT_LO, (uint32_t)chain);
	wr(mali, js + JS_HEAD_NEXT_HI, (uint32_t)(chain >> 32));
	wr(mali, js + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(mali, js + JS_AFFINITY_NEXT_HI, 0);
	wr(mali, js + JS_CONFIG_NEXT, JS_CONFIG);
	wr(mali, js + JS_COMMAND_NEXT, 1);
	lck_mtx_unlock(&mali_lock);
	finished = wait_for(mali, JOB_INT_RAWSTAT, (1u << slot) | (1u << (slot + 16)), 1u << slot, 1000000);
	lck_mtx_lock(&mali_lock);
	if (!finished) {
		status = (rd(mali, JOB_INT_RAWSTAT) & (1u << (slot + 16))) != 0 ? rd(mali, js + JS_STATUS) : 0xff;
		gMaliFailed++;
		LOG("job slot %u did not finish (raw %#x, status %#x, MMU raw %#x, fault %#x)", slot,
		    rd(mali, JOB_INT_RAWSTAT), rd(mali, js + JS_STATUS), rd(mali, MMU_INT_RAWSTAT),
		    rd(mali, AS0 + AS_FAULTSTATUS));
		(void)gpu_reset();
		{
			const char *which = NULL;
			(void)gpu_cores_on(&which);
		}
		as0_update();
	}
	wr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));
	wr(mali, MMU_INT_CLEAR, 0xffffffffu);
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
	return status;
}

#define MALI_QUEUE 64
static struct {
	uint64_t chain, seq;
	bool fragment;
} mali_queue[MALI_QUEUE];
static unsigned mali_queue_head, mali_queue_count;
static uint64_t mali_queued_seq, mali_done_seq;
static bool mali_worker_started;

static void
mali_worker(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		uint64_t chain, seq;
		bool fragment;

		lck_mtx_lock(&mali_queue_lock);
		while (mali_queue_count == 0) {
			lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_count, THREAD_UNINT);
		}
		chain = mali_queue[mali_queue_head].chain;
		seq = mali_queue[mali_queue_head].seq;
		fragment = mali_queue[mali_queue_head].fragment;
		lck_mtx_unlock(&mali_queue_lock);

		if (hakios_mali_submit(chain, fragment) != 0) {
			gMaliFailed++;
		}

		lck_mtx_lock(&mali_queue_lock);
		mali_queue_head = (mali_queue_head + 1) % MALI_QUEUE;
		mali_queue_count--;
		mali_done_seq = seq;
		lck_mtx_unlock(&mali_queue_lock);
		if (mali_trace) {
			LOG("job %llu %c done", seq, fragment ? 'f' : 'v');
		}
		wakeup((caddr_t)&mali_done_seq);
		wakeup((caddr_t)&mali_queue_head);
	}
}

uint64_t
hakios_mali_enqueue(uint64_t chain, bool fragment)
{
	uint64_t seq;

	lck_mtx_lock(&mali_queue_lock);
	if (!mali_worker_started) {
		thread_t thread;

		if (kernel_thread_start(mali_worker, NULL, &thread) == KERN_SUCCESS) {
			thread_set_thread_name(thread, "hakios-mali-jobs");
			thread_deallocate(thread);
			mali_worker_started = true;
		}
	}
	while (mali_queue_count == MALI_QUEUE) {
		lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_head, THREAD_UNINT);
	}
	seq = ++mali_queued_seq;
	{
		unsigned at = (mali_queue_head + mali_queue_count) % MALI_QUEUE;

		mali_queue[at].chain = chain;
		mali_queue[at].fragment = fragment;
		mali_queue[at].seq = seq;
		mali_queue_count++;
	}
	lck_mtx_unlock(&mali_queue_lock);
	wakeup((caddr_t)&mali_queue_count);
	return seq;
}

bool
hakios_mali_wait(uint64_t seq, uint64_t timeout_ns)
{
	uint64_t deadline;
	bool done;

	if (timeout_ns > 10ull * NSEC_PER_SEC) {
		timeout_ns = 10ull * NSEC_PER_SEC;
	}
	nanoseconds_to_absolutetime(timeout_ns, &deadline);
	deadline += mach_absolute_time();
	lck_mtx_lock(&mali_queue_lock);
	while (mali_done_seq < seq && timeout_ns != 0 && mach_absolute_time() < deadline) {
		lck_mtx_sleep_deadline(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_done_seq, THREAD_UNINT, deadline);
	}
	done = mali_done_seq >= seq;
	lck_mtx_unlock(&mali_queue_lock);
	return done;
}

static void
mali_thread(__unused void *parameter, __unused wait_result_t result)
{
	LOG("the Galaxy S8's Mali-G71: steps 1 to %u of %u (mali=%u)", last_step, STEPS, last_step);
	for (uint32_t step = 1; step <= last_step; step++) {
		LOG("step %u: %s", step, steps[step - 1].name);
		if (!steps[step - 1].make()) {
			LOG("step %u: FAILED; the steps after it are not made", step);
			break;
		}
		reached = step;
	}
	if (job_page != 0) {
		lck_mtx_lock(&mali_job_lock);
		lck_mtx_lock(&mali_lock);
		unmap_pages(FIRST_JOB_GPU, PAGE_SIZE);
		unmap_pages(SECOND_JOB_GPU, PAGE_SIZE);
		as0_update();
		lck_mtx_unlock(&mali_lock);
		lck_mtx_unlock(&mali_job_lock);
		kmem_free(kernel_map, job_page, PAGE_SIZE);
		job_page = 0;
	}
	LOG("came up to step %u of %u%s", reached, STEPS, hakios_mali_ready() ? ": HakiosMali is serving" : "");
}

void
hakios_mali_init(void)
{
	thread_t thread;

	(void)PE_parse_boot_argn("mali_fastmap", &mali_fastmap, sizeof(mali_fastmap));
	(void)PE_parse_boot_argn("mali_trace", &mali_trace, sizeof(mali_trace));
	if (!PE_parse_boot_argn("mali", &last_step, sizeof(last_step)) || last_step == 0) {
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	if (kernel_thread_start(mali_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

#else /* not the S8 config: compiled but inert */

bool hakios_mali_ready(void) { return false; }
bool hakios_mali_parameter(uint32_t which, uint64_t *value) { (void)which; (void)value; return false; }
uint64_t hakios_mali_address(uint64_t size, bool executable) { (void)size; (void)executable; return 0; }
bool hakios_mali_map(uint64_t gpu, uint64_t phys) { (void)gpu; (void)phys; return false; }
void hakios_mali_mapped(void) { }
void hakios_mali_unmap(uint64_t gpu, uint64_t size) { (void)gpu; (void)size; }
void hakios_mali_tag(uint64_t a, uint32_t k, uint32_t w, uint32_t h, uint32_t r, uint32_t f, uint32_t o)
{ (void)a; (void)k; (void)w; (void)h; (void)r; (void)f; (void)o; }
uint32_t hakios_mali_submit(uint64_t chain, bool fragment) { (void)chain; (void)fragment; return 1; }
uint64_t hakios_mali_enqueue(uint64_t chain, bool fragment) { (void)chain; (void)fragment; return 0; }
bool hakios_mali_wait(uint64_t seq, uint64_t timeout_ns) { (void)seq; (void)timeout_ns; return false; }
void hakios_mali_init(void) { }

#endif /* DREAMLTE && ARM64_BOARD_CONFIG_A5XELTE */
