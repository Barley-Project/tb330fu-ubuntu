/* cpumhz: estimate the clock of the CPU this runs on.  Freestanding aarch64 Linux program (no libc).
 * A chain of dependent 1-cycle ALU operations (add x,x,#1) runs at one per cycle on Cortex-A55 and A75/A76, the generic timer (CNTVCT_EL0, CNTFRQ_EL0)
 * measures the elapsed time.  Prints "cpu <n> ~<mhz> MHz".  Build: clang --target=aarch64-linux-gnu -O2 -nostdlib -static -fuse-ld=lld cpumhz.c -o cpumhz */
typedef unsigned long u64;

static long sys3(long n, long a, long b, long c)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
	return x0;
}

static u64 cnt(void)
{
	u64 v;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static void put(const char *s)
{
	long n = 0;

	while (s[n])
		n++;
	sys3(64, 1, (long)s, n);
}

static void putu(u64 v)
{
	char b[24];
	int i = 23;

	b[i] = 0;
	do {
		b[--i] = '0' + v % 10;
		v /= 10;
	} while (v);
	put(b + i);
}

void _start(void)
{
	u64 frq, t0, t1, best = 0, i;
	int cpu;
	unsigned int cpuid[2] = { 0, 0 };

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	sys3(168, (long)cpuid, 0, 0);	/* getcpu */
	cpu = (int)cpuid[0];
	for (int rep = 0; rep < 5; rep++) {
		u64 x = 0;

		t0 = cnt();
		for (i = 0; i < 20000000; i++) {
			/* 8 dependent adds per iteration */
			__asm__ volatile("add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
					 "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n" : "+r"(x));
		}
		t1 = cnt();
		/* cycles = 8 * 20e6 (+ loop overhead executing in parallel), time = (t1 - t0) / frq s  =>  MHz = cycles / time / 1e6 */
		u64 mhz = (8ul * 20000000ul * frq) / ((t1 - t0) * 1000000ul);

		if (mhz > best)
			best = mhz;
	}
	put("cpu ");
	putu(cpu);
	put(" ~");
	putu(best);
	put(" MHz (timer ");
	putu(frq / 1000000);
	put(" MHz)\n");
	sys3(93, 0, 0, 0);
}
