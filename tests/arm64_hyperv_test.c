/* SPDX-License-Identifier: BSD-2-Clause */
#include <arch/arm64/hyperv.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int raw_stdio;
static unsigned char ram[8192];
static void *map(void *arg, uint64_t gpa, size_t n)
{
	(void)arg;
	return gpa <= sizeof(ram) && n <= sizeof(ram) - gpa ? ram + gpa : NULL;
}
static uint64_t invoke(struct arm64_hyperv *hv, uint64_t x[18], unsigned imm, unsigned cpu)
{
	assert(arm64_hyperv_call(hv, imm, x, cpu, 4, map, NULL));
	return x[0];
}
static uint64_t fast(struct arm64_hyperv *hv, bool set, bool legacy,
    unsigned cpu, uint32_t name, uint64_t value, uint64_t x[18])
{
	unsigned c = legacy ? 0 : 1, p = c + 1;
	memset(x, 0, 18 * sizeof(*x));
	if (!legacy) x[0] = 0x46000001;
	x[c] = (UINT64_C(1) << 32) | 0x10000 | (set ? 0x51 : 0x50);
	x[p] = UINT64_MAX; x[p + 1] = 0xfffffffe; x[p + 2] = name;
	if (set) x[p + 4] = value;
	return invoke(hv, x, legacy ? 1 : 0, cpu);
}
static void put32(size_t off, uint32_t v) { memcpy(ram + off, &v, 4); }
static void put64(size_t off, uint64_t v) { memcpy(ram + off, &v, 8); }
static void kick(void *arg)
{
	atomic_fetch_add((_Atomic unsigned *)arg, 1);
}
static uint64_t get(struct arm64_hyperv *hv, unsigned cpu, uint32_t name)
{
	uint64_t x[18];
	assert(fast(hv, false, true, cpu, name, 0, x) == UINT64_C(1) << 32);
	return x[15];
}
static void set(struct arm64_hyperv *hv, unsigned cpu, uint32_t name, uint64_t value)
{
	uint64_t x[18];
	assert(fast(hv, true, true, cpu, name, value, x) == UINT64_C(1) << 32);
}
static int collect_irq(void *arg, uint32_t bits)
{
    *(uint32_t *)arg |= bits;
    return 0;
}
static int reject_irq(void *arg, uint32_t bits)
{
    (void)arg; (void)bits;
    return -1;
}
static uint32_t take_irqs(struct arm64_hyperv *hv, unsigned cpu)
{
    uint32_t bits = 0;
    assert(arm64_hyperv_process_timers(hv, cpu, collect_irq, &bits) == 0);
    return bits;
}
static void wait_irq(struct arm64_hyperv *hv, unsigned cpu, uint32_t expected)
{
	for (unsigned i = 0; i < 1000; i++) {
		uint32_t bits = take_irqs(hv, cpu);
		if (bits) { assert(bits == expected); return; }
		usleep(1000);
	}
	assert(!"synthetic timer interrupt timed out");
}
static void test_timers(struct arm64_hyperv *hv)
{
	_Atomic unsigned kicks = 0;
	uint64_t x[18];
	assert(arm64_hyperv_enable_timers(hv, map, NULL) == 0);
	arm64_hyperv_set_vp(hv, 0, kick, &kicks);
	arm64_hyperv_set_vp(hv, 1, kick, &kicks);
	assert(fast(hv, false, false, 0, 0x200, 0, x) == UINT64_C(1) << 32);
	assert((x[6] & 14) == 14 && (x[7] & (1u << 13)));
	uint64_t now = get(hv, 0, 0x90004);
	usleep(1000);
	assert(get(hv, 1, 0x90004) > now);
	/* Auto-enabled one-shot, using Windows' direct SGI4 configuration. */
	set(hv, 1, 0xb0000, 0x1048);
	set(hv, 1, 0xb0001, get(hv, 1, 0x90004) + 20000);
	wait_irq(hv, 1, 1u << 4);
	assert(!(get(hv, 1, 0xb0000) & 1));
	assert(!take_irqs(hv, 0));
	/* Periodic mode remains enabled and count=0 cancels future delivery. */
	set(hv, 0, 0xb0000, 0x105a);
	set(hv, 0, 0xb0001, 20000);
	wait_irq(hv, 0, 1u << 5);
	wait_irq(hv, 0, 1u << 5);
	set(hv, 0, 0xb0001, 0);
	take_irqs(hv, 0);
	usleep(5000);
	assert(!take_irqs(hv, 0));
	/* A busy message slot defers delivery until EOM without overwriting it. */
	memset(ram + 4096, 0, 4096);
	put32(4096 + 3 * 256, 0x1234);
	set(hv, 0, 0xa0013, 4097);
	set(hv, 0, 0xa0003, 6);
	set(hv, 0, 0xb0002, (3u << 16) | 8);
	set(hv, 0, 0xb0003, 1); /* Already expired. */
	usleep(10000);
	assert(!take_irqs(hv, 0));
	uint32_t *type = (uint32_t *)(ram + 4096 + 3 * 256);
	assert(__atomic_load_n(type, __ATOMIC_ACQUIRE) == 0x1234);
	assert(__atomic_load_n((unsigned char *)type + 5, __ATOMIC_ACQUIRE) & 1);
	__atomic_store_n(type, 0, __ATOMIC_RELEASE);
	set(hv, 0, 0xa0014, 0);
	wait_irq(hv, 0, 1u << 6);
	assert(__atomic_load_n(type, __ATOMIC_ACQUIRE) == 0x80000010);
	assert(((unsigned char *)type)[4] == 24);
	assert(type[4] == 1); /* Timer index. */
	assert(fast(hv, true, true, 0, 0xb0000, 1u << 9, x) == 5);
	assert(fast(hv, true, true, 0, 0xa0013, 8193, x) == 5);
    /* Expiration only marks pending: no guest memory access until vCPU runs. */
    memset(ram + 4096, 0, 4096);
    set(hv, 0, 0xb0002, (3u << 16) | 8);
    set(hv, 0, 0xb0003, 1);
    usleep(10000);
    assert(*type == 0);
    wait_irq(hv, 0, 1u << 6);
    assert(*type == 0x80000010);
    /* Backend failure retains a one-shot; cancellation removes pending work. */
    set(hv, 1, 0xb0000, 0x1168);
    set(hv, 1, 0xb0001, 1);
    usleep(10000);
    assert(arm64_hyperv_process_timers(hv, 1, reject_irq, NULL) == -1);
    assert(get(hv, 1, 0xb0000) & 1);
    wait_irq(hv, 1, 1u << 22);
    assert(!(get(hv, 1, 0xb0000) & 1));
    set(hv, 1, 0xb0001, 1);
    usleep(10000);
    set(hv, 1, 0xb0001, 0);
    assert(!take_irqs(hv, 1));
    /* Reset value zero and periodic-only count writes must not auto-enable. */
    set(hv, 1, 0xb0000, 2);
    set(hv, 1, 0xb0001, 1);
    assert(get(hv, 1, 0xb0000) == 2);
    set(hv, 1, 0xb0000, 9);
    assert(get(hv, 1, 0xb0000) == 8); /* SINT0 disables enabled indirect. */
    set(hv, 1, 0xb0001, 1);
    assert(get(hv, 1, 0xb0000) == 8);
	/* Reset cancels armed timers and pending interrupts on all CPUs. */
	set(hv, 1, 0xb0000, 0x104a);
	set(hv, 1, 0xb0001, 200000);
	arm64_hyperv_reset(hv);
	usleep(30000);
	assert(!get(hv, 1, 0xb0000));
	assert(!take_irqs(hv, 1));
	/* Unregister excludes subsequent callbacks even while a timer expires. */
	arm64_hyperv_set_vp(hv, 0, NULL, NULL);
	arm64_hyperv_set_vp(hv, 1, NULL, NULL);
	unsigned saved = atomic_load(&kicks);
	set(hv, 0, 0xb0000, 0x1048);
	set(hv, 0, 0xb0001, 1);
	usleep(5000);
	assert(atomic_load(&kicks) == saved);
}
int main(void)
{
	struct arm64_hyperv hv, other;
	uint64_t x[18] = {0}, saved[18];
	FILE *log = tmpfile(); assert(log);
	int old_stderr = dup(STDERR_FILENO); assert(old_stderr >= 0);
	assert(dup2(fileno(log), STDERR_FILENO) >= 0);
	assert(arm64_hyperv_init(&hv) == 0);
	assert(arm64_hyperv_init(&other) == 0);
	/* Default Hyper-V discovery must not start or expose synthetic timers. */
	assert(!hv.timers_enabled && !hv.worker_started);
	assert(fast(&hv, false, true, 0, 0xb0002, 0, x) == 5);
	assert(fast(&hv, true, true, 0, 0xb0003, 10000, x) == 5);
	assert(fast(&hv, false, true, 0, 0x90004, 0, x) == 5);
	assert(!hv.timers_enabled && !hv.worker_started);
	/* Unrelated PSCI calls are left for the existing dispatcher. */
	x[0] = 0x84000000; memcpy(saved, x, sizeof(x));
	assert(!arm64_hyperv_call(&hv, 0, x, 0, 4, map, NULL));
	assert(memcmp(x, saved, sizeof(x)) == 0);
	for (unsigned uid = 0; uid < 2; uid++) {
		x[0] = uid ? 0xc600ff01 : 0x8600ff01;
		invoke(&hv, x, 0, 0);
		assert(x[0] == 0x4d32ba58 && x[1] == 0xcd244764 &&
		    x[2] == 0x8eef6c75 && x[3] == 0x16597024);
	}
	/* Match the real ARM64 fast GET ABI (X6/X7 for SMCCC). */
	assert(fast(&hv, false, false, 0, 0x200, 0, x) == UINT64_C(1) << 32);
	assert(x[6] == (1u << 6) && x[7] == (1u << 8));
	assert(x[4] == 0x200); /* Input registers survive. */
	assert(fast(&hv, false, true, 3, 0x90003, 0, x) == UINT64_C(1) << 32);
	/* Windows HvlInvokeFastExtendedHypercall takes one result at X15/X16. */
	assert(x[15] == 3 && x[16] == 0 && x[3] == 0x90003);
	assert(fast(&hv, false, true, 0, 0x90006, 0, x) == UINT64_C(1) << 32);
	assert(x[15] == 0x31237648 && x[16] == 0);
	assert(fast(&hv, true, false, 0, 0x90002, 0x1234, x) == UINT64_C(1) << 32);
	assert(fast(&hv, true, false, 1, 0x90002, 0x5678, x) == 6);
	for (unsigned i = 0; i < 5; i++)
		assert(fast(&hv, true, i & 1, 2, 0x210 + i, 0xa0 + i, x) == UINT64_C(1) << 32);
	assert(fast(&hv, false, false, 1, 0x215, 0, x) == UINT64_C(1) << 32);
	assert(x[6] == UINT64_C(1) << 63 && x[7] == 0);
	assert(fast(&hv, true, false, 2, 0x215, UINT64_C(1) << 63, x) == UINT64_C(1) << 32);
	assert(fast(&hv, true, false, 2, 0x215, UINT64_C(3) << 62, x) == 5);
	assert(fast(&hv, true, false, 0, 0x200, 1, x) == 5); /* read-only */
	assert(fast(&hv, false, false, 0, 0xdead, 0, x) == 5);
	assert(fast(&other, false, false, 0, 0x210, 0, x) == UINT64_C(1) << 32);
	assert(x[6] == 0); /* Partition state is not global. */
	/* Memory-based multi-register GET, including repetition resume/errors. */
	put64(0, UINT64_MAX); put32(8, 0xfffffffe);
	put32(16, 0x210); put32(20, 0x214); put32(24, 0xdead);
	memset(x, 0, sizeof(x)); x[0] = 0x46000001;
	x[1] = (UINT64_C(3) << 32) | 0x50; x[3] = 4096;
	assert(invoke(&hv, x, 0, 0) == ((UINT64_C(2) << 32) | 5));
	uint64_t v; memcpy(&v, ram + 4096, 8); assert(v == 0xa0);
	memcpy(&v, ram + 4112, 8); assert(v == 0xa4);
	/* SET with one completed element followed by a malformed association. */
	memset(ram, 0, sizeof(ram)); put64(0, UINT64_MAX); put32(8, 0xfffffffe);
	put32(16, 0x210); put64(32, 0xbeef); put32(48, 0x211); put32(52, 1);
	memset(x, 0, sizeof(x)); x[0] = (UINT64_C(2) << 32) | 0x51;
	assert(invoke(&hv, x, 1, 0) == ((UINT64_C(1) << 32) | 5));
	put32(52, 0); put64(64, 0xcafe);
	x[0] = (UINT64_C(1) << 48) | (UINT64_C(2) << 32) | 0x51;
	assert(invoke(&hv, x, 1, 0) == UINT64_C(2) << 32);
	assert(fast(&hv, false, false, 0, 0x210, 0, x) == UINT64_C(1) << 32);
	assert(x[6] == 0xbeef);
	/* Windows phase 0 always registers this page, even with no optional
	 * enlightenments. Rejecting the initial GET causes a boot bugcheck.
	 */
	assert(fast(&hv, false, true, 0, 0x90015, 0, x) == UINT64_C(1) << 32);
	assert(x[15] == 0);
	memset(ram + 4096, 0xaa, 4096);
	assert(fast(&hv, true, true, 0, 0x90015, 4097, x) == UINT64_C(1) << 32);
	for (unsigned i = 4096; i < sizeof(ram); i++) assert(ram[i] == 0);
	assert(fast(&hv, false, true, 1, 0x90015, 0, x) == UINT64_C(1) << 32);
	assert(x[15] == 4097);
	assert(fast(&hv, true, true, 0, 0x90015, 8193, x) == 5);
	assert(fast(&hv, true, true, 0, 0x90015, 4099, x) == 5);
	assert(fast(&hv, false, true, 0, 0x90015, 0, x) == UINT64_C(1) << 32);
	assert(x[15] == 4097); /* Invalid registrations preserve the old state. */
	assert(fast(&hv, true, true, 0, 0x90015, 0, x) == UINT64_C(1) << 32);
	assert(fast(&hv, true, true, 0, 0x90016, 1, x) == UINT64_C(1) << 32);
	assert(fast(&hv, false, true, 0, 0x90016, 0, x) == UINT64_C(1) << 32 && x[15] == 1);
	assert(fast(&hv, false, true, 1, 0x90016, 0, x) == UINT64_C(1) << 32 && x[15] == 0);
	assert(fast(&hv, true, true, 0, 0x90016, 2, x) == 5);
	assert(fast(&hv, false, true, 64, 0x90016, 0, x) == 14);
	assert(fast(&hv, false, true, 0, 0x209, 0, x) == UINT64_C(1) << 32);
	assert(x[15] == 0 && x[16] == 0);
	assert(fast(&hv, true, true, 2, 0x40400, 0xfffff80012340000, x) == UINT64_C(1) << 32);
	assert(fast(&hv, false, true, 2, 0x40400, 0, x) == UINT64_C(1) << 32 && x[15] == UINT64_C(0xfffff80012340000));
	assert(fast(&hv, false, true, 0, 0x40400, 0, x) == UINT64_C(1) << 32 && x[15] == 0);
	assert(fast(&hv, true, true, 2, 0x40400, 1, x) == 5);
	/* Invalid pointers, page crossings, output overlap, and control fields. */
	uint64_t bad_gpas[] = {1, 4088, 8192, UINT64_MAX - 7};
	for (unsigned i = 0; i < sizeof(bad_gpas)/sizeof(bad_gpas[0]); i++) {
		memset(x, 0, sizeof(x)); x[0] = (UINT64_C(1) << 32) | 0x50;
		x[1] = bad_gpas[i]; x[2] = 4096;
		assert(invoke(&hv, x, 1, 0) == 4);
	}
	memset(x, 0, sizeof(x)); x[0] = (UINT64_C(1) << 32) | 0x50;
	x[2] = 16; assert(invoke(&hv, x, 1, 0) == 4);
	memset(x, 0, sizeof(x)); x[0] = 0x50; assert(invoke(&hv, x, 1, 0) == 3);
	x[0] = (UINT64_C(1) << 32) | 0x20050; assert(invoke(&hv, x, 1, 0) == 3);
	x[0] = 0x99; assert(invoke(&hv, x, 1, 0) == 2);
	arm64_hyperv_reset(&hv);
	assert(fast(&hv, false, true, 0, 0x90015, 0, x) == UINT64_C(1) << 32 && x[15] == 0);
	assert(fast(&hv, false, true, 0, 0x90016, 0, x) == UINT64_C(1) << 32 && x[15] == 0);
	assert(fast(&hv, false, true, 2, 0x40400, 0, x) == UINT64_C(1) << 32 && x[15] == 0);
	assert(fast(&hv, false, false, 0, 0x210, 0, x) == UINT64_C(1) << 32 && x[6] == 0);
	assert(fast(&hv, false, false, 0, 0x90002, 0, x) == UINT64_C(1) << 32 && x[6] == 0);
	test_timers(&hv);
	arm64_hyperv_destroy(&hv); arm64_hyperv_destroy(&other);
	fflush(stderr); assert(dup2(old_stderr, STDERR_FILENO) >= 0); close(old_stderr);
	rewind(log); char text[4096] = {0}; fread(text, 1, sizeof(text)-1, log); fclose(log);
	assert(strstr(text, "Hyper-V guest crash: vcpu=2 guestOsId=0x0000000000001234"));
	assert(strstr(text, "P0=0x00000000000000a0"));
	assert(strstr(text, "P4=0x00000000000000a4"));
	puts("ARM64 Hyper-V register, crash and timer tests passed");
	return 0;
}
