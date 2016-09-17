#include <assert.h>
#include <stdlib.h>
#include <time.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "target.h"
#include "target_type.h"
#include "log.h"
#include "jtag/jtag.h"
#include "opcodes.h"
#include "register.h"
#include "breakpoints.h"

/**
 * Since almost everything can be accomplish by scanning the dbus register, all
 * functions here assume dbus is already selected. The exception are functions
 * called directly by OpenOCD, which can't assume anything about what's
 * currently in IR. They should set IR to dbus explicitly.
 */

#define get_field(reg, mask) (((reg) & (mask)) / ((mask) & ~((mask) << 1)))
#define set_field(reg, mask, val) (((reg) & ~(mask)) | (((val) * ((mask) & ~((mask) << 1))) & (mask)))

#define DIM(x)		(sizeof(x)/sizeof(*x))

// Constants for legacy SiFive hardware breakpoints.
#define CSR_BPCONTROL_X			(1<<0)
#define CSR_BPCONTROL_W			(1<<1)
#define CSR_BPCONTROL_R			(1<<2)
#define CSR_BPCONTROL_U			(1<<3)
#define CSR_BPCONTROL_S			(1<<4)
#define CSR_BPCONTROL_H			(1<<5)
#define CSR_BPCONTROL_M			(1<<6)
#define CSR_BPCONTROL_BPMATCH	(0xf<<7)
#define CSR_BPCONTROL_BPACTION	(0xff<<11)

#define DEBUG_ROM_START         0x800
#define DEBUG_ROM_RESUME        (DEBUG_ROM_START + 4)
#define DEBUG_ROM_EXCEPTION     (DEBUG_ROM_START + 8)
#define DEBUG_RAM_START         0x400

#define SETHALTNOT				0x10c

/*** JTAG registers. ***/

#define DTMINFO					0x10
#define DTMINFO_ADDRBITS		(0xf<<4)
#define DTMINFO_VERSION			(0xf)

#define DBUS						0x11
#define DBUS_OP_START				0
#define DBUS_OP_SIZE				2
typedef enum {
	DBUS_OP_NOP = 0,
	DBUS_OP_READ = 1,
	DBUS_OP_WRITE = 2
} dbus_op_t;
typedef enum {
	DBUS_STATUS_SUCCESS = 0,
	DBUS_STATUS_FAILED = 2,
	DBUS_STATUS_BUSY = 3
} dbus_status_t;
#define DBUS_DATA_START				2
#define DBUS_DATA_SIZE				34
#define DBUS_ADDRESS_START			36

typedef enum {
	RE_OK,
	RE_FAIL,
	RE_AGAIN
} riscv_error_t;

typedef enum slot {
	SLOT0,
	SLOT1,
	SLOT_LAST,
} slot_t;

/*** Debug Bus registers. ***/

#define DMCONTROL				0x10
#define DMCONTROL_INTERRUPT		(((uint64_t)1)<<33)
#define DMCONTROL_HALTNOT		(((uint64_t)1)<<32)
#define DMCONTROL_BUSERROR		(7<<19)
#define DMCONTROL_SERIAL		(3<<16)
#define DMCONTROL_AUTOINCREMENT	(1<<15)
#define DMCONTROL_ACCESS		(7<<12)
#define DMCONTROL_HARTID		(0x3ff<<2)
#define DMCONTROL_NDRESET		(1<<1)
#define DMCONTROL_FULLRESET		1

#define DMINFO					0x11
#define DMINFO_ABUSSIZE			(0x7f<<25)
#define DMINFO_SERIALCOUNT		(0xf<<21)
#define DMINFO_ACCESS128		(1<<20)
#define DMINFO_ACCESS64			(1<<19)
#define DMINFO_ACCESS32			(1<<18)
#define DMINFO_ACCESS16			(1<<17)
#define DMINFO_ACCESS8			(1<<16)
#define DMINFO_DRAMSIZE			(0x3f<<10)
#define DMINFO_AUTHENTICATED	(1<<5)
#define DMINFO_AUTHBUSY			(1<<4)
#define DMINFO_AUTHTYPE			(3<<2)
#define DMINFO_VERSION			3

/*** Info about the core being debugged. ***/

#define DBUS_ADDRESS_UNKNOWN	0xffff

// gdb's register list is defined in riscv_gdb_reg_names gdb/riscv-tdep.c in
// its source tree. We must interpret the numbers the same here.
enum {
	REG_XPR0 = 0,
	REG_XPR31 = 31,
	REG_PC = 32,
	REG_FPR0 = 33,
	REG_FPR31 = 64,
	REG_CSR0 = 65,
	REG_CSR4095 = 4160,
	REG_PRIV = 4161,
	REG_COUNT
};

#define MAX_HWBPS			16
#define DRAM_CACHE_SIZE		16

struct trigger {
	uint64_t address;
	uint32_t length;
	uint64_t mask;
	uint64_t value;
	bool read, write, execute;
	int unique_id;
};

struct memory_cache_line {
	uint32_t data;
	bool valid;
	bool dirty;
};

typedef struct {
	/* Number of address bits in the dbus register. */
	uint8_t addrbits;
	/* Width of a GPR (and many other things) in bits. */
	uint8_t xlen;
	/* Number of words in Debug RAM. */
	unsigned int dramsize;
	uint64_t dcsr;
	uint64_t dpc;
	uint64_t misa;
	uint64_t tselect;
	bool tselect_dirty;

	struct memory_cache_line dram_cache[DRAM_CACHE_SIZE];

	struct reg *reg_list;
	/* Single buffer that contains all register names, instead of calling
	 * malloc for each register. Needs to be freed when reg_list is freed. */
	char *reg_names;
	/* Single buffer that contains all register values. */
	void *reg_values;

	// For each physical trigger, contains -1 if the hwbp is available, or the
	// unique_id of the breakpoint/watchpoint that is using it.
	int trigger_unique_id[MAX_HWBPS];

	// This value is incremented every time a dbus access comes back as "busy".
	// It's used to determine how many run-test/idle cycles to feed the target
	// in between accesses.
	unsigned int dbus_busy_delay;

	// This value is incremented every time we read the debug interrupt as
	// high.  It's used to add extra run-test/idle cycles after setting debug
	// interrupt high, so ideally we never have to perform a whole extra scan
	// before the interrupt is cleared.
	unsigned int interrupt_high_delay;

	// This cache is write-through, and always valid when the target is halted.
	uint64_t gpr_cache[32];

	bool need_strict_step;
} riscv_info_t;

typedef struct {
	bool haltnot;
	bool interrupt;
} bits_t;

/*** Necessary prototypes. ***/

static int riscv_poll(struct target *target);
static int poll_target(struct target *target, bool announce);

/*** Utility functions. ***/

static uint8_t ir_dtminfo[1] = {DTMINFO};
static struct scan_field select_dtminfo = {
	.in_value = NULL,
	.out_value = ir_dtminfo
};
static uint8_t ir_dbus[1] = {DBUS};
static struct scan_field select_dbus = {
	.in_value = NULL,
	.out_value = ir_dbus
};
static uint8_t ir_debug[1] = {0x5};
static struct scan_field select_debug = {
	.in_value = NULL,
	.out_value = ir_debug
};
#define DEBUG_LENGTH	264

static uint32_t load(const struct target *target, unsigned int rd,
		unsigned int base, uint16_t offset)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	switch (info->xlen) {
		case 32:
			return lw(rd, base, offset);
		case 64:
			return ld(rd, base, offset);
	}
	assert(0);
}

static uint32_t store(const struct target *target, unsigned int src,
		unsigned int base, uint16_t offset)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	switch (info->xlen) {
		case 32:
			return sw(src, base, offset);
		case 64:
			return sd(src, base, offset);
	}
	assert(0);
}

static unsigned int slot_offset(const struct target *target, slot_t slot)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	switch (info->xlen) {
		case 32:
			switch (slot) {
				case SLOT0: return 4;
				case SLOT1: return 5;
				case SLOT_LAST: return info->dramsize-1;
			}
		case 64:
			switch (slot) {
				case SLOT0: return 4;
				case SLOT1: return 6;
				case SLOT_LAST: return info->dramsize-2;
			}
	}
	LOG_ERROR("slot_offset called with xlen=%d, slot=%d",
			info->xlen, slot);
	assert(0);
}

static uint32_t load_slot(const struct target *target, unsigned int dest,
		slot_t slot)
{
	unsigned int offset = DEBUG_RAM_START + 4 * slot_offset(target, slot);
	return load(target, dest, ZERO, offset);
}

static uint32_t store_slot(const struct target *target, unsigned int src,
		slot_t slot)
{
	unsigned int offset = DEBUG_RAM_START + 4 * slot_offset(target, slot);
	return store(target, src, ZERO, offset);
}

static uint16_t dram_address(unsigned int index)
{
	if (index < 0x10)
		return index;
	else
		return 0x40 + index - 0x10;
}

static void increase_dbus_busy_delay(struct target *target)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	info->dbus_busy_delay++;
	LOG_INFO("Increment dbus_busy_delay to %d", info->dbus_busy_delay);
}

static void increase_interrupt_high_delay(struct target *target)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	info->interrupt_high_delay++;
	LOG_INFO("Increment interrupt_high_delay to %d", info->interrupt_high_delay);
}

static void add_dbus_scan(const struct target *target, struct scan_field *field,
		uint8_t *out_value, uint8_t *in_value, dbus_op_t op, uint16_t address, 
		uint64_t data)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	LOG_DEBUG("op=%d address=0x%02x data=0x%09" PRIx64, op, address, data);

	field->num_bits = info->addrbits + DBUS_OP_SIZE + DBUS_DATA_SIZE;
	field->in_value = in_value;
	field->out_value = out_value;

	buf_set_u64(out_value, DBUS_OP_START, DBUS_OP_SIZE, op);
	buf_set_u64(out_value, DBUS_DATA_START, DBUS_DATA_SIZE, data);
	buf_set_u64(out_value, DBUS_ADDRESS_START, info->addrbits, address);

	jtag_add_dr_scan(target->tap, 1, field, TAP_IDLE);

	// TODO: 1 should come from the dtminfo register
	int idle_count = 1 + info->dbus_busy_delay;
	if (data & DMCONTROL_INTERRUPT) {
		idle_count += info->interrupt_high_delay;
	}

	jtag_add_runtest(idle_count, TAP_IDLE);
}

static dbus_status_t dbus_scan(struct target *target, uint16_t *address_in,
		uint64_t *data_in, dbus_op_t op, uint16_t address_out, uint64_t data_out)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	uint8_t in[8] = {0};
	uint8_t out[8];
	struct scan_field field = {
		.num_bits = info->addrbits + DBUS_OP_SIZE + DBUS_DATA_SIZE,
		.out_value = out,
		.in_value = in
	};

	assert(info->addrbits != 0);

	buf_set_u64(out, DBUS_OP_START, DBUS_OP_SIZE, op);
	buf_set_u64(out, DBUS_DATA_START, DBUS_DATA_SIZE, data_out);
	buf_set_u64(out, DBUS_ADDRESS_START, info->addrbits, address_out);

	/* Assume dbus is already selected. */
	jtag_add_dr_scan(target->tap, 1, &field, TAP_IDLE);
	jtag_add_runtest(1, TAP_IDLE);

	int retval = jtag_execute_queue();
	if (retval != ERROR_OK) {
		LOG_ERROR("dbus_scan failed jtag scan");
		return retval;
	}

	if (data_in) {
		*data_in = buf_get_u64(in, DBUS_DATA_START, DBUS_DATA_SIZE);
	}

	if (address_in) {
		*address_in = buf_get_u32(in, DBUS_ADDRESS_START, info->addrbits);
	}

	static const char *op_string[] = {"nop", "r", "w", "cw"};
	static const char *status_string[] = {"+", "nw", "F", "b"};
	/*
	LOG_DEBUG("vvv $display(\"hardware: dbus scan %db %s %01x:%08x @%02x -> %s %01x:%08x @%02x\");",
			field.num_bits,
			op_string[buf_get_u32(out, 0, 2)],
			buf_get_u32(out, 34, 2), buf_get_u32(out, 2, 32),
			buf_get_u32(out, 36, info->addrbits),
			status_string[buf_get_u32(in, 0, 2)],
			buf_get_u32(in, 34, 2), buf_get_u32(in, 2, 32),
			buf_get_u32(in, 36, info->addrbits));
			*/
	LOG_DEBUG("dbus scan %db %s %01x:%08x @%02x -> %s %01x:%08x @%02x",
			field.num_bits,
			op_string[buf_get_u32(out, 0, 2)],
			buf_get_u32(out, 34, 2), buf_get_u32(out, 2, 32),
			buf_get_u32(out, 36, info->addrbits),
			status_string[buf_get_u32(in, 0, 2)],
			buf_get_u32(in, 34, 2), buf_get_u32(in, 2, 32),
			buf_get_u32(in, 36, info->addrbits));

	//debug_scan(target);

	return buf_get_u32(in, DBUS_OP_START, DBUS_OP_SIZE);
}

static uint64_t dbus_read(struct target *target, uint16_t address)
{
	uint64_t value;
	dbus_status_t status;
	uint16_t address_in;

	do {
		do {
			status = dbus_scan(target, &address_in, &value, DBUS_OP_READ, address, 0);
		} while (status == DBUS_STATUS_BUSY);
	} while (address_in != address);

	return value;
}

static void dbus_write(struct target *target, uint16_t address, uint64_t value)
{
	dbus_status_t status = DBUS_STATUS_BUSY;
	while (status == DBUS_STATUS_BUSY) {
		status = dbus_scan(target, NULL, NULL, DBUS_OP_WRITE, address, value);
	}
	if (status != DBUS_STATUS_SUCCESS) {
		LOG_ERROR("dbus_write failed write 0x%" PRIx64 " to 0x%x; status=%d\n",
				value, address, status);
	}
}

/*** scans "class" ***/

typedef struct {
	// Number of scans that space is reserved for.
	unsigned int scan_count;
	// Size reserved in memory for each scan, in bytes.
	unsigned int scan_size;
	unsigned int next_scan;
	uint8_t *in;
	uint8_t *out;
	struct scan_field *field;
	const struct target *target;
} scans_t;

static scans_t *scans_new(struct target *target, unsigned int scan_count)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	scans_t *scans = malloc(sizeof(scans_t));
	scans->scan_count = scan_count;
	scans->scan_size = 2 + info->xlen / 8;
	scans->next_scan = 0;
	scans->in = malloc(scans->scan_size * scans->scan_count);
	scans->out = malloc(scans->scan_size * scans->scan_count);
	scans->field = calloc(scans->scan_count, sizeof(struct scan_field));
	scans->target = target;
	return scans;
}

static scans_t *scans_delete(scans_t *scans)
{
	assert(scans);
	free(scans->field);
	free(scans->out);
	free(scans->in);
	free(scans);
	return NULL;
}

static void scans_reset(scans_t *scans)
{
	scans->next_scan = 0;
}

static void scans_add_write32(scans_t *scans, uint16_t address, uint32_t data,
		bool set_interrupt)
{
	const unsigned int i = scans->next_scan;
	add_dbus_scan(scans->target, &scans->field[i], scans->out + scans->scan_size * i,
			scans->in + scans->scan_size * i, DBUS_OP_WRITE, address,
			(set_interrupt ? DMCONTROL_INTERRUPT : 0) | DMCONTROL_HALTNOT | data);
	scans->next_scan++;
	assert(scans->next_scan <= scans->scan_count);
}

static void scans_add_write_jump(scans_t *scans, uint16_t address,
		bool set_interrupt)
{
	scans_add_write32(scans, address,
			jal(0, (uint32_t) (DEBUG_ROM_RESUME - (DEBUG_RAM_START + 4*address))),
			set_interrupt);
}

static void scans_add_write_load(scans_t *scans, uint16_t address,
		unsigned int reg, slot_t slot, bool set_interrupt)
{
	scans_add_write32(scans, address, load_slot(scans->target, reg, slot),
			set_interrupt);
}

static void scans_add_write_store(scans_t *scans, uint16_t address,
		unsigned int reg, slot_t slot, bool set_interrupt)
{
	scans_add_write32(scans, address, store_slot(scans->target, reg, slot),
			set_interrupt);
}

static void scans_add_read32(scans_t *scans, uint16_t address, bool set_interrupt)
{
	const unsigned int i = scans->next_scan;
	add_dbus_scan(scans->target, &scans->field[i],
			scans->out + scans->scan_size * i,
			scans->in + scans->scan_size * i, DBUS_OP_READ, address,
			(set_interrupt ? DMCONTROL_INTERRUPT : 0) | DMCONTROL_HALTNOT);
	scans->next_scan++;
	assert(scans->next_scan < scans->scan_count);
}

static void scans_add_read(scans_t *scans, slot_t slot, bool set_interrupt)
{
	const struct target *target = scans->target;
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	switch (info->xlen) {
		case 32:
			scans_add_read32(scans, slot_offset(target, slot), set_interrupt);
			break;
		case 64:
			scans_add_read32(scans, slot_offset(target, slot), false);
			scans_add_read32(scans, slot_offset(target, slot) + 1, set_interrupt);
			break;
	}
}

static uint32_t scans_get_u32(scans_t *scans, unsigned int index,
		unsigned first, unsigned num)
{
	return buf_get_u32(scans->in + scans->scan_size * index, first, num);
}

static uint64_t scans_get_u64(scans_t *scans, unsigned int index,
		unsigned first, unsigned num)
{
	return buf_get_u64(scans->in + scans->scan_size * index, first, num);
}

/*** end of scans class ***/

static uint32_t dtminfo_read(struct target *target)
{
	struct scan_field field;
	uint8_t in[4];

	jtag_add_ir_scan(target->tap, &select_dtminfo, TAP_IDLE);

	field.num_bits = 32;
	field.out_value = NULL;
	field.in_value = in;
	jtag_add_dr_scan(target->tap, 1, &field, TAP_IDLE);

	int retval = jtag_execute_queue();
	if (retval != ERROR_OK) {
		LOG_ERROR("dtminfo_read failed jtag scan");
		return retval;
	}

	/* Always return to dbus. */
	jtag_add_ir_scan(target->tap, &select_dbus, TAP_IDLE);

	return buf_get_u32(field.in_value, 0, 32);
}

static uint32_t dram_read32(struct target *target, unsigned int index)
{
	uint16_t address = dram_address(index);
	uint32_t value = dbus_read(target, address);
	return value;
}

static void dram_write32(struct target *target, unsigned int index, uint32_t value,
		bool set_interrupt)
{
	uint64_t dbus_value = DMCONTROL_HALTNOT | value;
	if (set_interrupt)
		dbus_value |= DMCONTROL_INTERRUPT;
	dbus_write(target, dram_address(index), dbus_value);
}

/** Read the haltnot and interrupt bits. */
static bits_t read_bits(struct target *target)
{
	uint64_t value;
	dbus_status_t status;
	uint16_t address_in;

	do {
		do {
			status = dbus_scan(target, &address_in, &value, DBUS_OP_READ, 0, 0);
		} while (status == DBUS_STATUS_BUSY);
	} while (address_in > 0x10 && address_in != DMCONTROL);

	bits_t result = {
		.haltnot = get_field(value, DMCONTROL_HALTNOT),
		.interrupt = get_field(value, DMCONTROL_INTERRUPT)
	};
	return result;
}

static int wait_for_debugint_clear(struct target *target, bool ignore_first)
{
	time_t start = time(NULL);
	if (ignore_first) {
		// Throw away the results of the first read, since they'll contain the
		// result of the read that happened just before debugint was set.
		// (Assuming the last scan before calling this function was one that
		// sets debugint.)
		read_bits(target);
	}
	while (1) {
		bits_t bits = read_bits(target);
		if (!bits.interrupt) {
			return ERROR_OK;
		}
		if (time(NULL) - start > 2) {
			LOG_ERROR("Timed out waiting for debug int to clear.");
			return ERROR_FAIL;
		}
	}
}

static int dram_check32(struct target *target, unsigned int index,
               uint32_t expected)
{
	uint16_t address = dram_address(index);
	uint32_t actual = dbus_read(target, address);
	if (expected != actual) {
		LOG_ERROR("Wrote 0x%x to Debug RAM at %d, but read back 0x%x",
				expected, index, actual);
		return ERROR_FAIL;
	}
	return ERROR_OK;
}

static void cache_set32(struct target *target, unsigned int index, uint32_t data)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	if (false && info->dram_cache[index].valid &&
			info->dram_cache[index].data == data) {
		// This is already preset on the target.
		LOG_DEBUG("cache[0x%x] = 0x%x (hit)", index, data);
		return;
	}
	LOG_DEBUG("cache[0x%x] = 0x%x", index, data);
	info->dram_cache[index].data = data;
	info->dram_cache[index].valid = true;
	info->dram_cache[index].dirty = true;
}

static void cache_set(struct target *target, slot_t slot, uint64_t data)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	unsigned int offset = slot_offset(target, slot);
	cache_set32(target, offset, data);
	if (info->xlen > 32) {
		cache_set32(target, offset + 1, data >> 32);
	}
}

static void cache_set_jump(struct target *target, unsigned int index)
{
	cache_set32(target, index,
			jal(0, (uint32_t) (DEBUG_ROM_RESUME - (DEBUG_RAM_START + 4*index))));
}

static void cache_set_load(struct target *target, unsigned int index,
		unsigned int reg, slot_t slot)
{
	uint16_t offset = DEBUG_RAM_START + 4 * slot_offset(target, slot);
	cache_set32(target, index, load(target, reg, ZERO, offset));
}

static void cache_set_store(struct target *target, unsigned int index,
		unsigned int reg, slot_t slot)
{
	uint16_t offset = DEBUG_RAM_START + 4 * slot_offset(target, slot);
	cache_set32(target, index, store(target, reg, ZERO, offset));
}

static void dump_debug_ram(struct target *target)
{
	for (unsigned int i = 0; i < 16; i++) {
		uint32_t value = dram_read32(target, i);
		LOG_ERROR("Debug RAM 0x%x: 0x%08x", i, value);
	}
}

/* Call this if the code you just ran writes to debug RAM entries 0 through 3. */
static void cache_invalidate(struct target *target)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	for (unsigned int i = 0; i < DRAM_CACHE_SIZE; i++) {
		info->dram_cache[i].valid = false;
		info->dram_cache[i].dirty = false;
	}
}

/* Called by cache_write() after the program has run. Also call this if you're
 * running programs without calling cache_write(). */
static void cache_clean(struct target *target)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	for (unsigned int i = 0; i < DRAM_CACHE_SIZE; i++) {
		if (i >= 4) {
			info->dram_cache[i].valid = false;
		}
		info->dram_cache[i].dirty = false;
	}
}

static int cache_check(struct target *target)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	int error = 0;

	for (unsigned int i = 0; i < DRAM_CACHE_SIZE; i++) {
		if (info->dram_cache[i].valid && !info->dram_cache[i].dirty) {
			if (dram_check32(target, i, info->dram_cache[i].data) != ERROR_OK) {
				error++;
			}
		}
	}

	if (error) {
		dump_debug_ram(target);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

/** Write cache to the target, and optionally run the program.
 * Then read the value at address into the cache, assuming address < 128. */
#define CACHE_NO_READ	128
static int cache_write(struct target *target, unsigned int address, bool run)
{
	LOG_DEBUG("enter");
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	scans_t *scans = scans_new(target, DRAM_CACHE_SIZE + 2);

	unsigned int last = DRAM_CACHE_SIZE;
	for (unsigned int i = 0; i < DRAM_CACHE_SIZE; i++) {
		if (info->dram_cache[i].dirty) {
			assert(i < info->dramsize);
			last = i;
		}
	}

	if (last == DRAM_CACHE_SIZE) {
		// Nothing needs to be written to RAM.
		dbus_write(target, DMCONTROL, DMCONTROL_HALTNOT | DMCONTROL_INTERRUPT);

	} else {
		for (unsigned int i = 0; i < DRAM_CACHE_SIZE; i++) {
			if (info->dram_cache[i].dirty) {
				bool set_interrupt = (i == last && run);
				scans_add_write32(scans, i, info->dram_cache[i].data,
						set_interrupt);
			}
		}
	}

	if (run || address < CACHE_NO_READ) {
		// Throw away the results of the first read, since it'll contain the
		// result of the read that happened just before debugint was set.
		scans_add_read32(scans, address, false);

		// This scan contains the results of the read the caller requested, as
		// well as an interrupt bit worth looking at.
		scans_add_read32(scans, address, false);
	}

	int retval = jtag_execute_queue();
	if (retval != ERROR_OK) {
		LOG_ERROR("JTAG execute failed.");
		return retval;
	}

	int errors = 0;
	for (unsigned int i = 0; i < scans->next_scan; i++) {
		dbus_status_t status = scans_get_u32(scans, i, DBUS_OP_START,
				DBUS_OP_SIZE);
		switch (status) {
			case DBUS_STATUS_SUCCESS:
				break;
			case DBUS_STATUS_FAILED:
				LOG_ERROR("Debug RAM write failed. Hardware error?");
				return ERROR_FAIL;
			case DBUS_STATUS_BUSY:
				errors++;
				break;
			default:
				LOG_ERROR("Got invalid bus access status: %d", status);
				return ERROR_FAIL;
		}
	}

	if (errors) {
		increase_dbus_busy_delay(target);

		// Try again, using the slow careful code.
		for (unsigned int i = 0; i < DRAM_CACHE_SIZE; i++) {
			if (i == last && run) {
				dram_write32(target, last, info->dram_cache[last].data, true);
			} else {
				dram_write32(target, i, info->dram_cache[i].data, false);
			}
			info->dram_cache[i].dirty = false;
		}
		cache_clean(target);

		if (wait_for_debugint_clear(target, true) != ERROR_OK) {
			LOG_ERROR("Debug interrupt didn't clear.");
			dump_debug_ram(target);
			return ERROR_FAIL;
		}

	} else {
		cache_clean(target);

		if (run || address < CACHE_NO_READ) {
			int interrupt = scans_get_u32(scans, scans->next_scan-1,
					DBUS_DATA_START + 33, 1);
			if (interrupt) {
				increase_interrupt_high_delay(target);
				// Slow path wait for it to clear.
				if (wait_for_debugint_clear(target, false) != ERROR_OK) {
					LOG_ERROR("Debug interrupt didn't clear.");
					dump_debug_ram(target);
					return ERROR_FAIL;
				}
			} else {
				// We read a useful value in that last scan.
				unsigned int read_addr = scans_get_u32(scans, scans->next_scan-1,
						DBUS_ADDRESS_START, info->addrbits);
				if (read_addr != address) {
					LOG_INFO("Got data from 0x%x but expected it from 0x%x",
							read_addr, address);
				}
				info->dram_cache[read_addr].data =
					scans_get_u32(scans, scans->next_scan-1, DBUS_DATA_START, 32);
				info->dram_cache[read_addr].valid = true;
			}
		}
	}

	scans_delete(scans);
	LOG_DEBUG("exit");

	return ERROR_OK;
}

uint32_t cache_get32(struct target *target, unsigned int address)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	if (!info->dram_cache[address].valid) {
		info->dram_cache[address].data = dram_read32(target, address);
		info->dram_cache[address].valid = true;
	}
	return info->dram_cache[address].data;
}

uint64_t cache_get(struct target *target, slot_t slot)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	unsigned int offset = slot_offset(target, slot);
	uint64_t value = cache_get32(target, offset);
	if (info->xlen > 32) {
		value |= ((uint64_t) cache_get32(target, offset + 1)) << 32;
	}
	return value;
}

/* Write instruction that jumps from the specified word in Debug RAM to resume
 * in Debug ROM. */
static void dram_write_jump(struct target *target, unsigned int index,
		bool set_interrupt)
{
	dram_write32(target, index,
			jal(0, (uint32_t) (DEBUG_ROM_RESUME - (DEBUG_RAM_START + 4*index))),
			set_interrupt);
}

static int wait_for_state(struct target *target, enum target_state state)
{
	time_t start = time(NULL);
	while (1) {
		int result = riscv_poll(target);
		if (result != ERROR_OK) {
			return result;
		}
		if (target->state == state) {
			return ERROR_OK;
		}
		if (time(NULL) - start > 2) {
			LOG_ERROR("Timed out waiting for state %d.", state);
			return ERROR_FAIL;
		}
	}
}

static int read_csr(struct target *target, uint64_t *value, uint32_t csr)
{
	cache_set32(target, 0, csrr(S0, csr));
	cache_set_store(target, 1, S0, SLOT0);
	cache_set_jump(target, 2);
	if (cache_write(target, 4, true) != ERROR_OK) {
		return ERROR_FAIL;
	}
	*value = cache_get(target, SLOT0);

	return ERROR_OK;
}

static int write_csr(struct target *target, uint32_t csr, uint64_t value)
{
	cache_set_load(target, 0, S0, SLOT0);
	cache_set32(target, 1, csrw(S0, csr));
	cache_set_jump(target, 2);
	cache_set(target, SLOT0, value);
	if (cache_write(target, 4, true) != ERROR_OK) {
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int write_gpr(struct target *target, unsigned int gpr, uint64_t value)
{
	cache_set_load(target, 0, gpr, SLOT0);
	cache_set_jump(target, 1);
	cache_set(target, SLOT0, value);
	if (cache_write(target, 4, true) != ERROR_OK) {
		return ERROR_FAIL;
	}
	return ERROR_OK;
}

static int maybe_read_tselect(struct target *target)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	if (info->tselect_dirty) {
		int result = read_csr(target, &info->tselect, CSR_TSELECT);
		if (result != ERROR_OK)
			return result;
		info->tselect_dirty = false;
	}

	return ERROR_OK;
}

static int maybe_write_tselect(struct target *target)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	if (!info->tselect_dirty) {
		int result = write_csr(target, CSR_TSELECT, info->tselect);
		if (result != ERROR_OK)
			return result;
		info->tselect_dirty = true;
	}

	return ERROR_OK;
}

static int execute_resume(struct target *target, bool step)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	LOG_DEBUG("resume(step=%d)", step);

	maybe_write_tselect(target);

	// TODO: check if dpc is dirty (which also is true if an exception was hit
	// at any time)
	cache_set_load(target, 0, S0, SLOT0);
	cache_set32(target, 1, csrw(S0, CSR_DPC));
	cache_set_jump(target, 2);
	cache_set(target, SLOT0, info->dpc);
	if (cache_write(target, 4, true) != ERROR_OK) {
		return ERROR_FAIL;
	}

	info->dcsr |= DCSR_EBREAKM | DCSR_EBREAKH | DCSR_EBREAKS | DCSR_EBREAKU;
	info->dcsr &= ~DCSR_HALT;

	if (step) {
		info->dcsr |= DCSR_STEP;
	} else {
		info->dcsr &= ~DCSR_STEP;
	}

	dram_write32(target, 0, lw(S0, ZERO, DEBUG_RAM_START + 16), false);
	dram_write32(target, 1, csrw(S0, CSR_DCSR), false);
	dram_write32(target, 2, fence_i(), false);
	dram_write_jump(target, 3, false);

	// Write DCSR value, set interrupt and clear haltnot.
	uint64_t dbus_value = DMCONTROL_INTERRUPT | info->dcsr;
	dbus_write(target, dram_address(4), dbus_value);

	cache_invalidate(target);

	if (wait_for_debugint_clear(target, true) != ERROR_OK) {
		LOG_ERROR("Debug interrupt didn't clear.");
		return ERROR_FAIL;
	}

	target->state = TARGET_RUNNING;
	for (unsigned int i = 0; i < 32; i++) {
		info->gpr_cache[i] = 0xbadbad;
	}

	return ERROR_OK;
}

// Execute a step, and wait for reentry into Debug Mode.
static int full_step(struct target *target, bool announce)
{
	int result = execute_resume(target, true);
	if (result != ERROR_OK)
		return result;
	time_t start = time(NULL);
	while (1) {
		result = poll_target(target, announce);
		if (result != ERROR_OK)
			return result;
		if (target->state != TARGET_DEBUG_RUNNING)
			break;
		if (time(NULL) - start > 2) {
			LOG_ERROR("Timed out waiting for step to complete.");
			return ERROR_FAIL;
		}
	}
	return ERROR_OK;
}

static int resume(struct target *target, int current, uint32_t address,
		int handle_breakpoints, int debug_execution, bool step)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	if (!current) {
		if (info->xlen > 32) {
			LOG_WARNING("Asked to resume at 32-bit PC on %d-bit target.",
					info->xlen);
		}
		LOG_ERROR("TODO: current is false");
		return ERROR_FAIL;
	}

	if (handle_breakpoints) {
		LOG_ERROR("TODO: handle_breakpoints is true");
		return ERROR_FAIL;
	}

	if (debug_execution) {
		LOG_ERROR("TODO: debug_execution is true");
		return ERROR_FAIL;
	}

	return execute_resume(target, step);
}

/** Update register sizes based on xlen. */
static void update_reg_list(struct target *target)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	if (info->reg_values) {
		free(info->reg_values);
	}
	info->reg_values = malloc(REG_COUNT * info->xlen / 4);

	for (unsigned int i = 0; i < REG_COUNT; i++) {
		struct reg *r = &info->reg_list[i];
		r->value = info->reg_values + i * info->xlen / 4;
		if (r->dirty) {
			LOG_ERROR("Register %d was dirty. Its value is lost.", i);
		}
		if (i == REG_PRIV) {
			r->size = 8;
		} else {
			r->size = info->xlen;
		}
		r->valid = false;
	}
}

/*** OpenOCD target functions. ***/

static int register_get(struct reg *reg)
{
	struct target *target = (struct target *) reg->arch_info;
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	maybe_write_tselect(target);

	if (reg->number <= REG_XPR31) {
		buf_set_u64(reg->value, 0, info->xlen, info->gpr_cache[reg->number]);
		LOG_DEBUG("%s=0x%" PRIx64, reg->name, info->gpr_cache[reg->number]);
		return ERROR_OK;
	} else if (reg->number == REG_PC) {
		buf_set_u32(reg->value, 0, 32, info->dpc);
		LOG_DEBUG("%s=0x%" PRIx64 " (cached)", reg->name, info->dpc);
		return ERROR_OK;
	} else if (reg->number >= REG_FPR0 && reg->number <= REG_FPR31) {
		cache_set32(target, 0, fsw(reg->number - REG_FPR0, 0, DEBUG_RAM_START + 16));
		cache_set_jump(target, 1);
	} else if (reg->number >= REG_CSR0 && reg->number <= REG_CSR4095) {
		cache_set32(target, 0, csrr(S0, reg->number - REG_CSR0));
		cache_set_store(target, 1, S0, SLOT0);
		cache_set_jump(target, 2);
	} else if (reg->number == REG_PRIV) {
		buf_set_u64(reg->value, 0, 8, get_field(info->dcsr, DCSR_PRV));
		LOG_DEBUG("%s=%d (cached)", reg->name,
				(int) get_field(info->dcsr, DCSR_PRV));
		return ERROR_OK;
	} else {
		LOG_ERROR("Don't know how to read register %d (%s)", reg->number, reg->name);
		return ERROR_FAIL;
	}

	if (cache_write(target, 4, true) != ERROR_OK) {
		return ERROR_FAIL;
	}

	uint64_t value = cache_get(target, SLOT0);
	if (reg->number < 32 && info->gpr_cache[reg->number] != value) {
		LOG_ERROR("cached value for %s is 0x%" PRIx64 " but just read 0x%" PRIx64,
				reg->name, info->gpr_cache[reg->number], value);
		assert(info->gpr_cache[reg->number] == value);
	}

	uint32_t exception = cache_get32(target, info->dramsize-1);
	if (exception) {
		LOG_ERROR("Got exception 0x%x when reading register %d", exception,
				reg->number);
		return ERROR_FAIL;
	}

	LOG_DEBUG("%s=0x%" PRIx64, reg->name, value);
	buf_set_u64(reg->value, 0, info->xlen, value);

	return ERROR_OK;
}

static int register_write(struct target *target, unsigned int number,
		uint64_t value)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	maybe_write_tselect(target);

	if (number == S0) {
		cache_set_load(target, 0, S0, SLOT0);
		cache_set32(target, 1, csrw(S0, CSR_DSCRATCH));
		cache_set_jump(target, 2);
	} else if (number == S1) {
		cache_set_load(target, 0, S0, SLOT0);
		cache_set_store(target, 1, S0, SLOT_LAST);
		cache_set_jump(target, 2);
	} else if (number <= REG_XPR31) {
		cache_set_load(target, 0, number - REG_XPR0, SLOT0);
		cache_set_jump(target, 1);
	} else if (number == REG_PC) {
		info->dpc = value;
		return ERROR_OK;
	} else if (number >= REG_FPR0 && number <= REG_FPR31) {
		// TODO: fld
		cache_set32(target, 0, flw(number - REG_FPR0, 0, DEBUG_RAM_START + 16));
		cache_set_jump(target, 1);
	} else if (number >= REG_CSR0 && number <= REG_CSR4095) {
		cache_set_load(target, 0, S0, SLOT0);
		cache_set32(target, 1, csrw(S0, number - REG_CSR0));
		cache_set_jump(target, 2);
	} else if (number == REG_PRIV) {
		info->dcsr = set_field(info->dcsr, DCSR_PRV, value);
		return ERROR_OK;
	} else {
		LOG_ERROR("Don't know how to write register %d", number);
		return ERROR_FAIL;
	}

	cache_set(target, SLOT0, value);
	if (cache_write(target, 4, true) != ERROR_OK) {
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int register_set(struct reg *reg, uint8_t *buf)
{
	struct target *target = (struct target *) reg->arch_info;
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	uint64_t value = buf_get_u64(buf, 0, info->xlen);

	LOG_DEBUG("write 0x%" PRIx64 " to %s", value, reg->name);
	if (reg->number <= REG_XPR31) {
		info->gpr_cache[reg->number] = value;
	}

	return register_write(target, reg->number, value);
}

static struct reg_arch_type riscv_reg_arch_type = {
	.get = register_get,
	.set = register_set
};

static int riscv_init_target(struct command_context *cmd_ctx,
		struct target *target)
{
	LOG_DEBUG("riscv_init_target()");
	target->arch_info = calloc(1, sizeof(riscv_info_t));
	if (!target->arch_info)
		return ERROR_FAIL;
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	select_dtminfo.num_bits = target->tap->ir_length;
	select_dbus.num_bits = target->tap->ir_length;
	select_debug.num_bits = target->tap->ir_length;

	const unsigned int max_reg_name_len = 12;
	info->reg_list = calloc(REG_COUNT, sizeof(struct reg));

	info->reg_names = calloc(1, REG_COUNT * max_reg_name_len);
	char *reg_name = info->reg_names;
	info->reg_values = NULL;

	for (unsigned int i = 0; i < REG_COUNT; i++) {
		struct reg *r = &info->reg_list[i];
		r->number = i;
		r->caller_save = true;
		r->dirty = false;
		r->valid = false;
		r->exist = true;
		r->type = &riscv_reg_arch_type;
		r->arch_info = target;
		if (i <= REG_XPR31) {
			sprintf(reg_name, "x%d", i);
		} else if (i == REG_PC) {
			sprintf(reg_name, "pc");
		} else if (i >= REG_FPR0 && i <= REG_FPR31) {
			sprintf(reg_name, "f%d", i - REG_FPR0);
		} else if (i >= REG_CSR0 && i <= REG_CSR4095) {
			sprintf(reg_name, "csr%d", i - REG_CSR0);
		} else if (i == REG_PRIV) {
			sprintf(reg_name, "priv");
		}
		if (reg_name[0]) {
			r->name = reg_name;
		}
		reg_name += strlen(reg_name) + 1;
		assert(reg_name < info->reg_names + REG_COUNT * max_reg_name_len);
	}
	update_reg_list(target);

	memset(info->trigger_unique_id, 0xff, sizeof(info->trigger_unique_id));

	return ERROR_OK;
}

static void riscv_deinit_target(struct target *target)
{
	LOG_DEBUG("riscv_deinit_target()");
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	free(info);
	target->arch_info = NULL;
}

static int riscv_halt(struct target *target)
{
	LOG_DEBUG("riscv_halt()");
	jtag_add_ir_scan(target->tap, &select_dbus, TAP_IDLE);

	cache_set32(target, 0, csrsi(CSR_DCSR, DCSR_HALT));
	cache_set32(target, 1, csrr(S0, CSR_MHARTID));
	cache_set32(target, 2, sw(S0, ZERO, SETHALTNOT));
	cache_set_jump(target, 3);

	if (cache_write(target, 4, true) != ERROR_OK) {
		LOG_ERROR("cache_write() failed.");
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int add_trigger(struct target *target, struct trigger *trigger)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	maybe_read_tselect(target);

	int i;
	for (i = 0; i < MAX_HWBPS; i++) {
		if (info->trigger_unique_id[i] != -1) {
			continue;
		}

		uint64_t tselect = i;
		write_csr(target, CSR_TSELECT, tselect);
		uint64_t tselect_rb;
		read_csr(target, &tselect_rb, CSR_TSELECT);
		if (tselect_rb != tselect) {
			// We've run out of breakpoints.
			LOG_ERROR("Couldn't find an available hardware trigger. "
					"(0x%" PRIx64 " != 0x%" PRIx64 ")", tselect,
					tselect_rb);
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}

		uint64_t tdata1;
		read_csr(target, &tdata1, CSR_TDATA1);
		int type = get_field(tdata1, MCONTROL_TYPE(info->xlen));

		if (type != 2) {
			continue;
		}

		if (tdata1 & (MCONTROL_EXECUTE | MCONTROL_STORE | MCONTROL_LOAD)) {
			// Trigger is already in use, presumably by user code.
			continue;
		}

		// address/data match trigger
		tdata1 |= MCONTROL_DMODE(info->xlen);
		tdata1 = set_field(tdata1, MCONTROL_ACTION,
				MCONTROL_ACTION_DEBUG_MODE);
		tdata1 = set_field(tdata1, MCONTROL_MATCH, MCONTROL_MATCH_EQUAL);
		tdata1 |= MCONTROL_M;
		if (info->misa & (1 << ('H' - 'A')))
			tdata1 |= MCONTROL_H;
		if (info->misa & (1 << ('S' - 'A')))
			tdata1 |= MCONTROL_S;
		if (info->misa & (1 << ('U' - 'A')))
			tdata1 |= MCONTROL_U;

		if (trigger->execute)
			tdata1 |= MCONTROL_EXECUTE;
		if (trigger->read)
			tdata1 |= MCONTROL_LOAD;
		if (trigger->write)
			tdata1 |= MCONTROL_STORE;

		write_csr(target, CSR_TDATA1, tdata1);

		uint64_t tdata1_rb;
		read_csr(target, &tdata1_rb, CSR_TDATA1);
		LOG_DEBUG("tdata1=0x%" PRIx64, tdata1_rb);

		if (tdata1 != tdata1_rb) {
			LOG_DEBUG("Trigger %d doesn't support what we need; After writing 0x%"
					PRIx64 " to tdata1 it contains 0x%" PRIx64,
					i, tdata1, tdata1_rb);
			write_csr(target, CSR_TDATA1, 0);
			continue;
		}

		write_csr(target, CSR_TDATA2, trigger->address);

		LOG_DEBUG("Using resource %d for bp %d", i,
				trigger->unique_id);
		info->trigger_unique_id[i] = trigger->unique_id;
		break;
	}
	if (i >= MAX_HWBPS) {
		LOG_ERROR("Couldn't find an available hardware trigger.");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	return ERROR_OK;
}

static int remove_trigger(struct target *target, struct trigger *trigger)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	maybe_read_tselect(target);

	int i;
	for (i = 0; i < MAX_HWBPS; i++) {
		if (info->trigger_unique_id[i] == trigger->unique_id) {
			break;
		}
	}
	if (i >= MAX_HWBPS) {
		LOG_ERROR("Couldn't find the hardware resources used by hardware "
				"trigger.");
		return ERROR_FAIL;
	}
	LOG_DEBUG("Stop using resource %d for bp %d", i, trigger->unique_id);
	write_csr(target, CSR_TSELECT, i);
	write_csr(target, CSR_TDATA1, 0);
	info->trigger_unique_id[i] = -1;

	return ERROR_OK;
}

static void trigger_from_breakpoint(struct trigger *trigger,
		const struct breakpoint *breakpoint)
{
	trigger->address = breakpoint->address;
	trigger->length = breakpoint->length;
	trigger->mask = ~0LL;
	trigger->read = false;
	trigger->write = false;
	trigger->execute = true;
	// unique_id is unique across both breakpoints and watchpoints.
	trigger->unique_id = breakpoint->unique_id;
}

static void trigger_from_watchpoint(struct trigger *trigger,
		const struct watchpoint *watchpoint)
{
	trigger->address = watchpoint->address;
	trigger->length = watchpoint->length;
	trigger->mask = watchpoint->mask;
	trigger->value = watchpoint->value;
	trigger->read = (watchpoint->rw == WPT_READ || watchpoint->rw == WPT_ACCESS);
	trigger->write = (watchpoint->rw == WPT_WRITE || watchpoint->rw == WPT_ACCESS);
	trigger->execute = false;
	// unique_id is unique across both breakpoints and watchpoints.
	trigger->unique_id = watchpoint->unique_id;
}

static int riscv_add_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	if (breakpoint->type == BKPT_SOFT) {
		if (target_read_memory(target, breakpoint->address, breakpoint->length, 1,
					breakpoint->orig_instr) != ERROR_OK) {
			LOG_ERROR("Failed to read original instruction at 0x%x",
					breakpoint->address);
			return ERROR_FAIL;
		}

		int retval;
		if (breakpoint->length == 4) {
			retval = target_write_u32(target, breakpoint->address, ebreak());
		} else {
			retval = target_write_u16(target, breakpoint->address, ebreak_c());
		}
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to write %d-byte breakpoint instruction at 0x%x",
					breakpoint->length, breakpoint->address);
			return ERROR_FAIL;
		}

	} else if (breakpoint->type == BKPT_HARD) {
		struct trigger trigger;
		trigger_from_breakpoint(&trigger, breakpoint);
		int result = add_trigger(target, &trigger);
		if (result != ERROR_OK) {
			return result;
		}

	} else {
        LOG_INFO("OpenOCD only supports hardware and software breakpoints.");
        return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
    }

    breakpoint->set = true;

    return ERROR_OK;
}

static int riscv_remove_breakpoint(struct target *target,
		struct breakpoint *breakpoint)
{
    if (breakpoint->type == BKPT_SOFT) {
		if (target_write_memory(target, breakpoint->address, breakpoint->length, 1,
					breakpoint->orig_instr) != ERROR_OK) {
			LOG_ERROR("Failed to restore instruction for %d-byte breakpoint at "
					"0x%x", breakpoint->length, breakpoint->address);
			return ERROR_FAIL;
		}

	} else if (breakpoint->type == BKPT_HARD) {
		struct trigger trigger;
		trigger_from_breakpoint(&trigger, breakpoint);
		int result = remove_trigger(target, &trigger);
		if (result != ERROR_OK) {
			return result;
		}

	} else {
		LOG_INFO("OpenOCD only supports hardware and software breakpoints.");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	breakpoint->set = false;

	return ERROR_OK;
}

static int riscv_add_watchpoint(struct target *target,
		struct watchpoint *watchpoint)
{
	struct trigger trigger;
	trigger_from_watchpoint(&trigger, watchpoint);

	int result = add_trigger(target, &trigger);
	if (result != ERROR_OK) {
		return result;
	}
	watchpoint->set = true;

	return ERROR_OK;
}

static int riscv_remove_watchpoint(struct target *target,
		struct watchpoint *watchpoint)
{
	struct trigger trigger;
	trigger_from_watchpoint(&trigger, watchpoint);

	int result = remove_trigger(target, &trigger);
	if (result != ERROR_OK) {
		return result;
	}
	watchpoint->set = false;

	return ERROR_OK;
}

static int strict_step(struct target *target, bool announce)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	LOG_DEBUG("enter");

	struct breakpoint *breakpoint = target->breakpoints;
	while (breakpoint) {
		riscv_remove_breakpoint(target, breakpoint);
		breakpoint = breakpoint->next;
	}

	struct watchpoint *watchpoint = target->watchpoints;
	while (watchpoint) {
		riscv_remove_watchpoint(target, watchpoint);
		watchpoint = watchpoint->next;
	}

	int result = full_step(target, announce);
	if (result != ERROR_OK)
		return result;

	breakpoint = target->breakpoints;
	while (breakpoint) {
		riscv_add_breakpoint(target, breakpoint);
		breakpoint = breakpoint->next;
	}

	watchpoint = target->watchpoints;
	while (watchpoint) {
		riscv_add_watchpoint(target, watchpoint);
		watchpoint = watchpoint->next;
	}

	info->need_strict_step = false;

	return ERROR_OK;
}

static int riscv_step(struct target *target, int current, uint32_t address,
		int handle_breakpoints)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	jtag_add_ir_scan(target->tap, &select_dbus, TAP_IDLE);

	if (info->need_strict_step) {
		int result = strict_step(target, true);
		if (result != ERROR_OK)
			return result;
	} else {
		return resume(target, current, address, handle_breakpoints, 0, true);
	}

	return ERROR_OK;
}

static int riscv_examine(struct target *target)
{
	LOG_DEBUG("riscv_examine()");
	if (target_was_examined(target)) {
		return ERROR_OK;
	}

	// Don't need to select dbus, since the first thing we do is read dtminfo.

	uint32_t dtminfo = dtminfo_read(target);
	LOG_DEBUG("dtminfo=0x%x", dtminfo);
	LOG_DEBUG("  addrbits=%d", get_field(dtminfo, DTMINFO_ADDRBITS));
	LOG_DEBUG("  version=%d", get_field(dtminfo, DTMINFO_VERSION));
	// TODO: Add support for the idle field, once it's implemented in the FPGA
	// image.
	if (dtminfo == 0) {
		LOG_ERROR("dtminfo is 0. Check JTAG connectivity/board power.");
		return ERROR_FAIL;
	}
	if (get_field(dtminfo, DTMINFO_VERSION) != 0) {
		LOG_ERROR("Unsupported DTM version %d. (dtminfo=0x%x)",
				get_field(dtminfo, DTMINFO_VERSION), dtminfo);
		return ERROR_FAIL;
	}

	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	info->addrbits = get_field(dtminfo, DTMINFO_ADDRBITS);

	uint32_t dminfo = dbus_read(target, DMINFO);
	LOG_DEBUG("dminfo: 0x%08x", dminfo);
	LOG_DEBUG("  abussize=0x%x", get_field(dminfo, DMINFO_ABUSSIZE));
	LOG_DEBUG("  serialcount=0x%x", get_field(dminfo, DMINFO_SERIALCOUNT));
	LOG_DEBUG("  access128=%d", get_field(dminfo, DMINFO_ACCESS128));
	LOG_DEBUG("  access64=%d", get_field(dminfo, DMINFO_ACCESS64));
	LOG_DEBUG("  access32=%d", get_field(dminfo, DMINFO_ACCESS32));
	LOG_DEBUG("  access16=%d", get_field(dminfo, DMINFO_ACCESS16));
	LOG_DEBUG("  access8=%d", get_field(dminfo, DMINFO_ACCESS8));
	LOG_DEBUG("  dramsize=0x%x", get_field(dminfo, DMINFO_DRAMSIZE));
	LOG_DEBUG("  authenticated=0x%x", get_field(dminfo, DMINFO_AUTHENTICATED));
	LOG_DEBUG("  authbusy=0x%x", get_field(dminfo, DMINFO_AUTHBUSY));
	LOG_DEBUG("  authtype=0x%x", get_field(dminfo, DMINFO_AUTHTYPE));
	LOG_DEBUG("  version=0x%x", get_field(dminfo, DMINFO_VERSION));

	if (get_field(dminfo, DMINFO_VERSION) != 1) {
		LOG_ERROR("OpenOCD only supports Debug Module version 1, not %d "
				"(dminfo=0x%x)", get_field(dminfo, DMINFO_VERSION), dminfo);
		return ERROR_FAIL;
	}

	info->dramsize = get_field(dminfo, DMINFO_DRAMSIZE) + 1;

	if (get_field(dminfo, DMINFO_AUTHTYPE) != 0) {
		LOG_ERROR("Authentication required by RISC-V core but not "
				"supported by OpenOCD. dminfo=0x%x", dminfo);
		return ERROR_FAIL;
	}

	// Figure out XLEN.
	cache_set32(target, 0, xori(S1, ZERO, -1));
	// 0xffffffff  0xffffffff:ffffffff  0xffffffff:ffffffff:ffffffff:ffffffff
	cache_set32(target, 1, srli(S1, S1, 31));
	// 0x00000001  0x00000001:ffffffff  0x00000001:ffffffff:ffffffff:ffffffff
	cache_set32(target, 2, sw(S1, ZERO, DEBUG_RAM_START));
	cache_set32(target, 3, srli(S1, S1, 31));
	// 0x00000000  0x00000000:00000003  0x00000000:00000003:ffffffff:ffffffff
	cache_set32(target, 4, sw(S1, ZERO, DEBUG_RAM_START + 4));
	cache_set_jump(target, 5);

	cache_write(target, 0, false);

	// Check that we can actually read/write dram.
	if (cache_check(target) != ERROR_OK) {
		return ERROR_FAIL;
	}

	cache_write(target, 0, true);
	cache_invalidate(target);

	uint32_t word0 = cache_get32(target, 0);
	uint32_t word1 = cache_get32(target, 1);
	if (word0 == 1 && word1 == 0) {
		info->xlen = 32;
	} else if (word0 == 0xffffffff && word1 == 3) {
		info->xlen = 64;
	} else if (word0 == 0xffffffff && word1 == 0xffffffff) {
		info->xlen = 128;
	} else {
		uint32_t exception = cache_get32(target, info->dramsize-1);
		LOG_ERROR("Failed to discover xlen; word0=0x%x, word1=0x%x, exception=0x%x",
				word0, word1, exception);
		dump_debug_ram(target);
		return ERROR_FAIL;
	}
	LOG_DEBUG("Discovered XLEN is %d", info->xlen);

	// Update register list to match discovered XLEN.
	update_reg_list(target);

	target_set_examined(target);

	if (read_csr(target, &info->misa, CSR_MISA) != ERROR_OK) {
		LOG_ERROR("Failed to read misa.");
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static riscv_error_t handle_halt_routine(struct target *target)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	scans_t *scans = scans_new(target, 256);

	// Read all GPRs as fast as we can, because gdb is going to ask for them
	// anyway. Reading them one at a time is much slower.

	// Write the jump back to address 1.
	scans_add_write_jump(scans, 1, false);
	for (int reg = 1; reg < 32; reg++) {
		if (reg == S0 || reg == S1) {
			continue;
		}

		// Write store instruction.
		scans_add_write_store(scans, 0, reg, SLOT0, true);

		// Read value.
		scans_add_read(scans, SLOT0, false);
	}

	// Write store of s0 at index 1.
	scans_add_write_store(scans, 1, S0, SLOT0, false);
	// Write jump at index 2.
	scans_add_write_jump(scans, 2, false);

	// Read S1 from debug RAM
	scans_add_write_load(scans, 0, S0, SLOT_LAST, true);
	// Read value.
	scans_add_read(scans, SLOT0, false);

	// Read S0 from dscratch
	unsigned int csr[] = {CSR_DSCRATCH, CSR_DPC, CSR_DCSR};
	for (unsigned int i = 0; i < DIM(csr); i++) {
		scans_add_write32(scans, 0, csrr(S0, csr[i]), true);
		scans_add_read(scans, SLOT0, false);
	}

	// Final read to get the last value out.
	scans_add_read32(scans, 4, false);

	int retval = jtag_execute_queue();
	if (retval != ERROR_OK) {
		LOG_ERROR("JTAG execute failed: %d", retval);
		goto error;
	}

	unsigned int dbus_busy = 0;
	unsigned int interrupt_set = 0;
	unsigned result = 0;
	info->gpr_cache[0] = 0;
	// The first scan result is the result from something old we don't care
	// about.
	for (unsigned int i = 1; i < scans->next_scan && dbus_busy == 0; i++) {
		dbus_status_t status = scans_get_u32(scans, i, DBUS_OP_START,
				DBUS_OP_SIZE);
		uint64_t data = scans_get_u64(scans, i, DBUS_DATA_START, DBUS_DATA_SIZE);
		uint32_t address = scans_get_u32(scans, i, DBUS_ADDRESS_START,
				info->addrbits);
		LOG_DEBUG("read scan=%d result=%d data=%09" PRIx64 " address=%02x",
				i, status, data, address);
		switch (status) {
			case DBUS_STATUS_SUCCESS:
				break;
			case DBUS_STATUS_FAILED:
				LOG_ERROR("Debug access failed. Hardware error?");
				goto error;
			case DBUS_STATUS_BUSY:
				dbus_busy++;
				break;
			default:
				LOG_ERROR("Got invalid bus access status: %d", status);
				return ERROR_FAIL;
		}
		if (data & DMCONTROL_INTERRUPT) {
			interrupt_set++;
			break;
		}
		if (address == 4 || address == 5) {
			uint64_t *vptr = NULL;
			switch (result) {
				case 0: vptr = &info->gpr_cache[1]; break;
				case 1: vptr = &info->gpr_cache[2]; break;
				case 2: vptr = &info->gpr_cache[3]; break;
				case 3: vptr = &info->gpr_cache[4]; break;
				case 4: vptr = &info->gpr_cache[5]; break;
				case 5: vptr = &info->gpr_cache[6]; break;
				case 6: vptr = &info->gpr_cache[7]; break;
						// S0
						// S1
				case 7: vptr = &info->gpr_cache[10]; break;
				case 8: vptr = &info->gpr_cache[11]; break;
				case 9: vptr = &info->gpr_cache[12]; break;
				case 10: vptr = &info->gpr_cache[13]; break;
				case 11: vptr = &info->gpr_cache[14]; break;
				case 12: vptr = &info->gpr_cache[15]; break;
				case 13: vptr = &info->gpr_cache[16]; break;
				case 14: vptr = &info->gpr_cache[17]; break;
				case 15: vptr = &info->gpr_cache[18]; break;
				case 16: vptr = &info->gpr_cache[19]; break;
				case 17: vptr = &info->gpr_cache[20]; break;
				case 18: vptr = &info->gpr_cache[21]; break;
				case 19: vptr = &info->gpr_cache[22]; break;
				case 20: vptr = &info->gpr_cache[23]; break;
				case 21: vptr = &info->gpr_cache[24]; break;
				case 22: vptr = &info->gpr_cache[25]; break;
				case 23: vptr = &info->gpr_cache[26]; break;
				case 24: vptr = &info->gpr_cache[27]; break;
				case 25: vptr = &info->gpr_cache[28]; break;
				case 26: vptr = &info->gpr_cache[29]; break;
				case 27: vptr = &info->gpr_cache[30]; break;
				case 28: vptr = &info->gpr_cache[31]; break;
				case 29: vptr = &info->gpr_cache[S1]; break;
				case 30: vptr = &info->gpr_cache[S0]; break;
				case 31: vptr = &info->dpc; break;
				case 32: vptr = &info->dcsr; break;
				default:
						 assert(0);
			}
			if (info->xlen == 32) {
				*vptr = data & 0xffffffff;
				result++;
			} else if (info->xlen == 64) {
				if (address == 4) {
					*vptr = data & 0xffffffff;
				} else if (address == 5) {
					*vptr |= (data & 0xffffffff) << 32;
					result++;
				}
			}
		}
	}

	scans = scans_delete(scans);

	cache_invalidate(target);

	if (dbus_busy) {
		increase_dbus_busy_delay(target);
		return RE_AGAIN;
	}
	if (interrupt_set) {
		increase_interrupt_high_delay(target);
		return RE_AGAIN;
	}

	return RE_OK;

error:
	scans = scans_delete(scans);
	return RE_FAIL;
}

static int handle_halt(struct target *target, bool announce)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	target->state = TARGET_HALTED;

	riscv_error_t re;
	do {
		re = handle_halt_routine(target);
	} while (re == RE_AGAIN);
	if (re != RE_OK) {
		LOG_ERROR("handle_halt_routine failed");
		return ERROR_FAIL;
	}

	int cause = get_field(info->dcsr, DCSR_CAUSE);
	LOG_DEBUG("halt cause is %d; dcsr=0x%" PRIx64, cause, info->dcsr);
	switch (cause) {
		case DCSR_CAUSE_SWBP:
			target->debug_reason = DBG_REASON_BREAKPOINT;
			break;
		case DCSR_CAUSE_HWBP:
			target->debug_reason = DBG_REASON_WPTANDBKPT;
			// If we halted because of a data trigger, gdb doesn't know to do
			// the disable-breakpoints-step-enable-breakpoints dance.
			info->need_strict_step = true;
			break;
		case DCSR_CAUSE_DEBUGINT:
			target->debug_reason = DBG_REASON_DBGRQ;
			break;
		case DCSR_CAUSE_STEP:
			target->debug_reason = DBG_REASON_SINGLESTEP;
			break;
		case DCSR_CAUSE_HALT:
		default:
			LOG_ERROR("Invalid halt cause %d in DCSR (0x%" PRIx64 ")",
					cause, info->dcsr);
	}

	if (announce) {
		target_call_event_callbacks(target, TARGET_EVENT_HALTED);
	}

	LOG_DEBUG("halted at 0x%" PRIx64, info->dpc);

	return ERROR_OK;
}

static int poll_target(struct target *target, bool announce)
{
	jtag_add_ir_scan(target->tap, &select_dbus, TAP_IDLE);
	bits_t bits = read_bits(target);

	if (bits.haltnot && bits.interrupt) {
		target->state = TARGET_DEBUG_RUNNING;
		LOG_DEBUG("debug running");
	} else if (bits.haltnot && !bits.interrupt) {
		if (target->state != TARGET_HALTED) {
			return handle_halt(target, announce);
		}
	} else if (!bits.haltnot && bits.interrupt) {
		// Target is halting. There is no state for that, so don't change anything.
		LOG_DEBUG("halting");
	} else if (!bits.haltnot && !bits.interrupt) {
		target->state = TARGET_RUNNING;
		LOG_DEBUG("running");
	}

	return ERROR_OK;
}

static int riscv_poll(struct target *target)
{
	return poll_target(target, true);
}

static int riscv_resume(struct target *target, int current, uint32_t address,
		int handle_breakpoints, int debug_execution)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	jtag_add_ir_scan(target->tap, &select_dbus, TAP_IDLE);

	if (info->need_strict_step) {
		int result = strict_step(target, false);
		if (result != ERROR_OK)
			return result;
	}

	return resume(target, current, address, handle_breakpoints,
			debug_execution, false);
}

static int riscv_assert_reset(struct target *target)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	// TODO: Maybe what I implemented here is more like soft_reset_halt()?

	jtag_add_ir_scan(target->tap, &select_dbus, TAP_IDLE);

	// The only assumption we can make is that the TAP was reset.
	if (wait_for_debugint_clear(target, true) != ERROR_OK) {
		LOG_ERROR("Debug interrupt didn't clear.");
		return ERROR_FAIL;
	}

	// Not sure what we should do when there are multiple cores.
	// Here just reset the single hart we're talking to.
	info->dcsr |= DCSR_EBREAKM | DCSR_EBREAKH | DCSR_EBREAKS |
		DCSR_EBREAKU | DCSR_HALT;
	if (target->reset_halt) {
		info->dcsr |= DCSR_NDRESET;
	} else {
		info->dcsr |= DCSR_FULLRESET;
	}
	dram_write32(target, 0, lw(S0, ZERO, DEBUG_RAM_START + 16), false);
	dram_write32(target, 1, csrw(S0, CSR_DCSR), false);
	// We shouldn't actually need the jump because a reset should happen.
	dram_write_jump(target, 2, false);
	dram_write32(target, 4, info->dcsr, true);
	cache_invalidate(target);

	target->state = TARGET_RESET;

	return ERROR_OK;
}

static int riscv_deassert_reset(struct target *target)
{
	jtag_add_ir_scan(target->tap, &select_dbus, TAP_IDLE);
	if (target->reset_halt) {
		return wait_for_state(target, TARGET_HALTED);
	} else {
		return wait_for_state(target, TARGET_RUNNING);
	}
}

static int riscv_read_memory(struct target *target, uint32_t address,
		uint32_t size, uint32_t count, uint8_t *buffer)
{
	jtag_add_ir_scan(target->tap, &select_dbus, TAP_IDLE);

	cache_set32(target, 0, lw(S0, ZERO, DEBUG_RAM_START + 16));
	switch (size) {
		case 1:
			cache_set32(target, 1, lb(S1, S0, 0));
			cache_set32(target, 2, sw(S1, ZERO, DEBUG_RAM_START + 16));
			break;
		case 2:
			cache_set32(target, 1, lh(S1, S0, 0));
			cache_set32(target, 2, sw(S1, ZERO, DEBUG_RAM_START + 16));
			break;
		case 4:
			cache_set32(target, 1, lw(S1, S0, 0));
			cache_set32(target, 2, sw(S1, ZERO, DEBUG_RAM_START + 16));
			break;
		default:
			LOG_ERROR("Unsupported size: %d", size);
			return ERROR_FAIL;
	}
	cache_set_jump(target, 3);
	cache_write(target, CACHE_NO_READ, false);

	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	const int max_batch_size = 256;
	scans_t *scans = scans_new(target, max_batch_size);

	uint32_t result_value = 0x777;
	uint32_t i = 0;
	while (i < count + 3) {
		unsigned int batch_size = MIN(count + 3 - i, max_batch_size);
		scans_reset(scans);

		for (unsigned int j = 0; j < batch_size; j++) {
			if (i + j == count) {
				// Just insert a read so we can scan out the last value.
				scans_add_read32(scans, 4, false);
			} else if (i + j >= count + 1) {
				// And check for errors.
				scans_add_read32(scans, info->dramsize-1, false);
			} else {
				// Write the next address and set interrupt.
				uint32_t offset = size * (i + j);
				scans_add_write32(scans, 4, address + offset, true);
			}
		}

		int retval = jtag_execute_queue();
		if (retval != ERROR_OK) {
			LOG_ERROR("JTAG execute failed: %d", retval);
			goto error;
		}

		int dbus_busy = 0;
		int execute_busy = 0;
		for (unsigned int j = 0; j < batch_size; j++) {
			dbus_status_t status = scans_get_u32(scans, j, DBUS_OP_START,
					DBUS_OP_SIZE);
			switch (status) {
				case DBUS_STATUS_SUCCESS:
					break;
				case DBUS_STATUS_FAILED:
					LOG_ERROR("Debug RAM write failed. Hardware error?");
					goto error;
				case DBUS_STATUS_BUSY:
					dbus_busy++;
					break;
				default:
					LOG_ERROR("Got invalid bus access status: %d", status);
					return ERROR_FAIL;
			}
			uint64_t data = scans_get_u64(scans, j, DBUS_DATA_START,
					DBUS_DATA_SIZE);
			if (data & DMCONTROL_INTERRUPT) {
				execute_busy++;
			}
			if (i + j == count + 2) {
				result_value = data;
			} else if (i + j > 1) {
				uint32_t offset = size * (i + j - 2);
				switch (size) {
					case 1:
						buffer[offset] = data;
						break;
					case 2:
						buffer[offset] = data;
						buffer[offset+1] = data >> 8;
						break;
					case 4:
						buffer[offset] = data;
						buffer[offset+1] = data >> 8;
						buffer[offset+2] = data >> 16;
						buffer[offset+3] = data >> 24;
						break;
				}
			}
			LOG_DEBUG("j=%d status=%d data=%09" PRIx64, j, status, data);
		}
		if (dbus_busy) {
			increase_dbus_busy_delay(target);
		}
		if (execute_busy) {
			increase_interrupt_high_delay(target);
		}
		if (dbus_busy || execute_busy) {
			wait_for_debugint_clear(target, false);

			// Retry.
			LOG_INFO("Retrying memory read starting from 0x%x with more delays",
					address + size * i);
		} else {
			i += batch_size;
		}
	}

	if (result_value != 0) {
		LOG_ERROR("Core got an exception (0x%x) while reading from 0x%x",
				result_value, address + size * (count-1));
		if (count > 1) {
			LOG_ERROR("(It may have failed between 0x%x and 0x%x as well, but we "
					"didn't check then.)",
					address, address + size * (count-2) + size - 1);
		}
		goto error;
	}

	scans_delete(scans);
	cache_clean(target);
	return ERROR_OK;

error:
	scans_delete(scans);
	cache_clean(target);
	return ERROR_FAIL;
}

static int setup_write_memory(struct target *target, uint32_t size)
{
	switch (size) {
		case 1:
			cache_set32(target, 0, lb(S0, ZERO, DEBUG_RAM_START + 16));
			cache_set32(target, 1, sb(S0, T0, 0));
			break;
		case 2:
			cache_set32(target, 0, lh(S0, ZERO, DEBUG_RAM_START + 16));
			cache_set32(target, 1, sh(S0, T0, 0));
			break;
		case 4:
			cache_set32(target, 0, lw(S0, ZERO, DEBUG_RAM_START + 16));
			cache_set32(target, 1, sw(S0, T0, 0));
			break;
		default:
			LOG_ERROR("Unsupported size: %d", size);
			return ERROR_FAIL;
	}
	cache_set32(target, 2, addi(T0, T0, size));
	cache_set_jump(target, 3);
	cache_write(target, 4, false);

	return ERROR_OK;
}

static int riscv_write_memory(struct target *target, uint32_t address,
		uint32_t size, uint32_t count, const uint8_t *buffer)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	jtag_add_ir_scan(target->tap, &select_dbus, TAP_IDLE);

	// Set up the address.
	cache_set_store(target, 0, T0, SLOT1);
	cache_set_load(target, 1, T0, SLOT0);
	cache_set_jump(target, 2);
	cache_set(target, SLOT0, address);
	if (cache_write(target, 5, true) != ERROR_OK) {
		return ERROR_FAIL;
	}

	uint64_t t0 = cache_get(target, SLOT1);
	LOG_DEBUG("t0 is 0x%" PRIx64, t0);

	if (setup_write_memory(target, size) != ERROR_OK) {
		return ERROR_FAIL;
	}

	const int max_batch_size = 256;
	scans_t *scans = scans_new(target, max_batch_size);

	uint32_t result_value = 0x777;
	uint32_t i = 0;
	while (i < count + 2) {
		unsigned int batch_size = MIN(count + 2 - i, max_batch_size);
		scans_reset(scans);

		for (unsigned int j = 0; j < batch_size; j++) {
			if (i + j >= count) {
				// Check for an exception.
				scans_add_read32(scans, info->dramsize-1, false);
			} else {
				// Write the next value and set interrupt.
				uint32_t value;
				uint32_t offset = size * (i + j);
				switch (size) {
					case 1:
						value = buffer[offset];
						break;
					case 2:
						value = buffer[offset] |
							(buffer[offset+1] << 8);
						break;
					case 4:
						value = buffer[offset] |
							((uint32_t) buffer[offset+1] << 8) |
							((uint32_t) buffer[offset+2] << 16) |
							((uint32_t) buffer[offset+3] << 24);
						break;
					default:
						goto error;
				}

				scans_add_write32(scans, 4, value, true);
			}
		}

		int retval = jtag_execute_queue();
		if (retval != ERROR_OK) {
			LOG_ERROR("JTAG execute failed: %d", retval);
			goto error;
		}

		int dbus_busy = 0;
		int execute_busy = 0;
		for (unsigned int j = 0; j < batch_size; j++) {
			dbus_status_t status = scans_get_u32(scans, j, DBUS_OP_START,
					DBUS_OP_SIZE);
			switch (status) {
				case DBUS_STATUS_SUCCESS:
					break;
				case DBUS_STATUS_FAILED:
					LOG_ERROR("Debug RAM write failed. Hardware error?");
					goto error;
				case DBUS_STATUS_BUSY:
					dbus_busy++;
					break;
				default:
					LOG_ERROR("Got invalid bus access status: %d", status);
					return ERROR_FAIL;
			}
			int interrupt = scans_get_u32(scans, j, DBUS_DATA_START + 33, 1);
			if (interrupt) {
				execute_busy++;
			}
			if (i + j == count + 1) {
				result_value = scans_get_u32(scans, j, DBUS_DATA_START, 32);
			}
		}
		if (dbus_busy) {
			increase_dbus_busy_delay(target);
		}
		if (execute_busy) {
			increase_interrupt_high_delay(target);
		}
		if (dbus_busy || execute_busy) {
			wait_for_debugint_clear(target, false);

			// Retry.
			// Set t0 back to what it should have been at the beginning of this
			// batch.
			LOG_INFO("Retrying memory write starting from 0x%x with more delays",
					address + size * i);

			cache_clean(target);

			if (write_gpr(target, T0, address + size * i) != ERROR_OK) {
				goto error;
			}

			if (setup_write_memory(target, size) != ERROR_OK) {
				goto error;
			}
		} else {
			i += batch_size;
		}
	}

	if (result_value != 0) {
		LOG_ERROR("Core got an exception (0x%x) while writing to 0x%x",
				result_value, address + size * (count-1));
		if (count > 1) {
			LOG_ERROR("(It may have failed between 0x%x and 0x%x as well, but we "
					"didn't check then.)",
					address, address + size * (count-2) + size - 1);
		}
		goto error;
	}

	cache_clean(target);
	return register_write(target, T0, t0);

error:
	scans_delete(scans);
	cache_clean(target);
	return ERROR_FAIL;
}

static int riscv_get_gdb_reg_list(struct target *target,
		struct reg **reg_list[], int *reg_list_size,
		enum target_register_class reg_class)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	LOG_DEBUG("reg_class=%d", reg_class);

	switch (reg_class) {
		case REG_CLASS_GENERAL:
			*reg_list_size = 32;
			break;
		case REG_CLASS_ALL:
			*reg_list_size = REG_COUNT;
			break;
		default:
			LOG_ERROR("Unsupported reg_class: %d", reg_class);
			return ERROR_FAIL;
	}

	*reg_list = calloc(*reg_list_size, sizeof(struct reg *));
	if (!*reg_list) {
		return ERROR_FAIL;
	}
	for (int i = 0; i < *reg_list_size; i++) {
		(*reg_list)[i] = &info->reg_list[i];
	}

	return ERROR_OK;
}

int riscv_arch_state(struct target *target)
{
	return ERROR_OK;
}

struct target_type riscv_target =
{
	.name = "riscv",

	.init_target = riscv_init_target,
	.deinit_target = riscv_deinit_target,
	.examine = riscv_examine,

	/* poll current target status */
	.poll = riscv_poll,

	.halt = riscv_halt,
	.resume = riscv_resume,
	.step = riscv_step,

	.assert_reset = riscv_assert_reset,
	.deassert_reset = riscv_deassert_reset,

	.read_memory = riscv_read_memory,
	.write_memory = riscv_write_memory,

	.get_gdb_reg_list = riscv_get_gdb_reg_list,

	.add_breakpoint = riscv_add_breakpoint,
	.remove_breakpoint = riscv_remove_breakpoint,

	.add_watchpoint = riscv_add_watchpoint,
	.remove_watchpoint = riscv_remove_watchpoint,

	.arch_state = riscv_arch_state,
};
