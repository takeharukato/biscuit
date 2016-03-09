// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "runtime.h"
#include "defs_GOOS_GOARCH.h"
#include "os_GOOS.h"
#include "signal_unix.h"
#include "stack.h"
#include "textflag.h"

extern SigTab runtime·sigtab[];

static Sigset sigset_none;
static Sigset sigset_all = { ~(uint32)0, ~(uint32)0 };

// Linux futex.
//
//	futexsleep(uint32 *addr, uint32 val)
//	futexwakeup(uint32 *addr)
//
// Futexsleep atomically checks if *addr == val and if so, sleeps on addr.
// Futexwakeup wakes up ·threads sleeping on addr.
// Futexsleep is allowed to wake up spuriously.

enum
{
	FUTEX_WAIT = 0,
	FUTEX_WAKE = 1,
};

// Atomically,
//	if(*addr == val) sleep
// Might be woken up spuriously; that's allowed.
// Don't sleep longer than ns; ns < 0 means forever.
#pragma textflag NOSPLIT
void
runtime·futexsleep(uint32 *addr, uint32 val, int64 ns)
{
	Timespec ts;

	// Some Linux kernels have a bug where futex of
	// FUTEX_WAIT returns an internal error code
	// as an errno.  Libpthread ignores the return value
	// here, and so can we: as it says a few lines up,
	// spurious wakeups are allowed.

	if(ns < 0) {
		runtime·futex(addr, FUTEX_WAIT, val, nil, nil, 0);
		return;
	}
	// NOTE: tv_nsec is int64 on amd64, so this assumes a little-endian system.
	ts.tv_nsec = 0;
	ts.tv_sec = runtime·timediv(ns, 1000000000LL, (int32*)&ts.tv_nsec);
	runtime·futex(addr, FUTEX_WAIT, val, &ts, nil, 0);
}

static void badfutexwakeup(void);

// If any procs are sleeping on addr, wake up at most cnt.
#pragma textflag NOSPLIT
void
runtime·futexwakeup(uint32 *addr, uint32 cnt)
{
	int64 ret;
	void (*fn)(void);

	ret = runtime·futex(addr, FUTEX_WAKE, cnt, nil, nil, 0);
	if(ret >= 0)
		return;

	// I don't know that futex wakeup can return
	// EAGAIN or EINTR, but if it does, it would be
	// safe to loop and call futex again.
	g->m->ptrarg[0] = addr;
	g->m->scalararg[0] = (int32)ret; // truncated but fine
	fn = badfutexwakeup;
	if(g == g->m->gsignal)
		fn();
	else
		runtime·onM(&fn);
	*(int32*)0x1006 = 0x1006;
}

static void
badfutexwakeup(void)
{
	void *addr;
	int64 ret;
	
	addr = g->m->ptrarg[0];
	ret = (int32)g->m->scalararg[0];
	runtime·printf("futexwakeup addr=%p returned %D\n", addr, ret);
}

extern runtime·sched_getaffinity(uintptr pid, uintptr len, uintptr *buf);
static int32
getproccount(void)
{
	uintptr buf[16], t;
	int32 r, cnt, i;

	cnt = 0;
	r = runtime·sched_getaffinity(0, sizeof(buf), buf);
	if(r > 0)
	for(i = 0; i < r/sizeof(buf[0]); i++) {
		t = buf[i];
		t = t - ((t >> 1) & 0x5555555555555555ULL);
		t = (t & 0x3333333333333333ULL) + ((t >> 2) & 0x3333333333333333ULL);
		cnt += (int32)((((t + (t >> 4)) & 0xF0F0F0F0F0F0F0FULL) * 0x101010101010101ULL) >> 56);
	}

	return cnt ? cnt : 1;
}

// Clone, the Linux rfork.
enum
{
	CLONE_VM = 0x100,
	CLONE_FS = 0x200,
	CLONE_FILES = 0x400,
	CLONE_SIGHAND = 0x800,
	CLONE_PTRACE = 0x2000,
	CLONE_VFORK = 0x4000,
	CLONE_PARENT = 0x8000,
	CLONE_THREAD = 0x10000,
	CLONE_NEWNS = 0x20000,
	CLONE_SYSVSEM = 0x40000,
	CLONE_SETTLS = 0x80000,
	CLONE_PARENT_SETTID = 0x100000,
	CLONE_CHILD_CLEARTID = 0x200000,
	CLONE_UNTRACED = 0x800000,
	CLONE_CHILD_SETTID = 0x1000000,
	CLONE_STOPPED = 0x2000000,
	CLONE_NEWUTS = 0x4000000,
	CLONE_NEWIPC = 0x8000000,
};

void
runtime·newosproc(M *mp, void *stk)
{
	int32 ret;
	int32 flags;
	Sigset oset;

	/*
	 * note: strace gets confused if we use CLONE_PTRACE here.
	 */
	flags = CLONE_VM	/* share memory */
		| CLONE_FS	/* share cwd, etc */
		| CLONE_FILES	/* share fd table */
		| CLONE_SIGHAND	/* share sig handler table */
		| CLONE_THREAD	/* revisit - okay for now */
		;

	mp->tls[0] = mp->id;	// so 386 asm can find it
	if(0){
		runtime·printf("newosproc stk=%p m=%p g=%p clone=%p id=%d/%d ostk=%p\n",
			stk, mp, mp->g0, runtime·clone, mp->id, (int32)mp->tls[0], &mp);
	}

	// Disable signals during clone, so that the new thread starts
	// with signals disabled.  It will enable them in minit.
	runtime·rtsigprocmask(SIG_SETMASK, &sigset_all, &oset, sizeof oset);
	ret = runtime·clone(flags, stk, mp, mp->g0, runtime·mstart);
	runtime·rtsigprocmask(SIG_SETMASK, &oset, nil, sizeof oset);

	if(ret < 0) {
		runtime·printf("runtime: failed to create new OS thread (have %d already; errno=%d)\n", runtime·mcount(), -ret);
		runtime·throw("runtime.newosproc");
	}
}

int64 runtime·hackmode;

void
runtime·osinit(void)
{
	if (runtime·hackmode) {
		// XXX duur
		runtime·ncpu = 1;
	} else {
		runtime·ncpu = getproccount();
	}
}

// Random bytes initialized at startup.  These come
// from the ELF AT_RANDOM auxiliary vector (vdso_linux_amd64.c).
byte*	runtime·startup_random_data;
uint32	runtime·startup_random_data_len;

#pragma textflag NOSPLIT
void
runtime·get_random_data(byte **rnd, int32 *rnd_len)
{
	if(runtime·startup_random_data != nil) {
		*rnd = runtime·startup_random_data;
		*rnd_len = runtime·startup_random_data_len;
	} else {
		#pragma dataflag NOPTR
		static byte urandom_data[HashRandomBytes];
		int32 fd;
		fd = runtime·open("/dev/urandom", 0 /* O_RDONLY */, 0);
		if(runtime·read(fd, urandom_data, HashRandomBytes) == HashRandomBytes) {
			*rnd = urandom_data;
			*rnd_len = HashRandomBytes;
		} else {
			*rnd = nil;
			*rnd_len = 0;
		}
		runtime·close(fd);
	}
}

void
runtime·goenvs(void)
{
	runtime·goenvs_unix();
}

// Called to initialize a new m (including the bootstrap m).
// Called on the parent thread (main thread in case of bootstrap), can allocate memory.
void
runtime·mpreinit(M *mp)
{
	mp->gsignal = runtime·malg(32*1024);	// OS X wants >=8K, Linux >=2K
	mp->gsignal->m = mp;
}

// Called to initialize a new m (including the bootstrap m).
// Called on the new thread, can not allocate memory.
void
runtime·minit(void)
{
	// Initialize signal handling.
	runtime·signalstack((byte*)g->m->gsignal->stack.lo, 32*1024);
	runtime·rtsigprocmask(SIG_SETMASK, &sigset_none, nil, sizeof(Sigset));
}

// Called from dropm to undo the effect of an minit.
void
runtime·unminit(void)
{
	runtime·signalstack(nil, 0);
}

uintptr
runtime·memlimit(void)
{
	Rlimit rl;
	extern byte runtime·text[], runtime·end[];
	uintptr used;

	if(runtime·getrlimit(RLIMIT_AS, &rl) != 0)
		return 0;
	if(rl.rlim_cur >= 0x7fffffff)
		return 0;

	// Estimate our VM footprint excluding the heap.
	// Not an exact science: use size of binary plus
	// some room for thread stacks.
	used = runtime·end - runtime·text + (64<<20);
	if(used >= rl.rlim_cur)
		return 0;

	// If there's not at least 16 MB left, we're probably
	// not going to be able to do much.  Treat as no limit.
	rl.rlim_cur -= used;
	if(rl.rlim_cur < (16<<20))
		return 0;

	return rl.rlim_cur - used;
}

#ifdef GOARCH_386
#define sa_handler k_sa_handler
#endif

/*
 * This assembler routine takes the args from registers, puts them on the stack,
 * and calls sighandler().
 */
extern void runtime·sigtramp(void);
extern void runtime·sigreturn(void);	// calls rt_sigreturn, only used with SA_RESTORER

void
runtime·setsig(int32 i, GoSighandler *fn, bool restart)
{
	SigactionT sa;

	runtime·memclr((byte*)&sa, sizeof sa);
	sa.sa_flags = SA_ONSTACK | SA_SIGINFO | SA_RESTORER;
	if(restart)
		sa.sa_flags |= SA_RESTART;
	sa.sa_mask = ~0ULL;
	// Although Linux manpage says "sa_restorer element is obsolete and
	// should not be used". x86_64 kernel requires it. Only use it on
	// x86.
#ifdef GOARCH_386
	sa.sa_restorer = (void*)runtime·sigreturn;
#endif
#ifdef GOARCH_amd64
	sa.sa_restorer = (void*)runtime·sigreturn;
#endif
	if(fn == runtime·sighandler)
		fn = (void*)runtime·sigtramp;
	sa.sa_handler = fn;
	if(runtime·rt_sigaction(i, &sa, nil, sizeof(sa.sa_mask)) != 0)
		runtime·throw("rt_sigaction failure");
}

GoSighandler*
runtime·getsig(int32 i)
{
	SigactionT sa;

	runtime·memclr((byte*)&sa, sizeof sa);
	if(runtime·rt_sigaction(i, nil, &sa, sizeof(sa.sa_mask)) != 0)
		runtime·throw("rt_sigaction read failure");
	if((void*)sa.sa_handler == runtime·sigtramp)
		return runtime·sighandler;
	return (void*)sa.sa_handler;
}

void
runtime·signalstack(byte *p, int32 n)
{
	SigaltstackT st;

	st.ss_sp = p;
	st.ss_size = n;
	st.ss_flags = 0;
	if(p == nil)
		st.ss_flags = SS_DISABLE;
	runtime·sigaltstack(&st, nil);
}

void
runtime·unblocksignals(void)
{
	runtime·rtsigprocmask(SIG_SETMASK, &sigset_none, nil, sizeof sigset_none);
}

#pragma textflag NOSPLIT
int8*
runtime·signame(int32 sig)
{
	return runtime·sigtab[sig].name;
}

// src/runtime/asm_amd64.s
void ·cli(void);
void ·finit(void);
void ·fxsave(uint64 *);
void ·htpause(void);
struct cpu_t* ·Gscpu(void);
int64 inb(int32);
uint64 ·lap_id(void);
void ·lcr0(uint64);
void lcr3(uint64);
void ·lcr4(uint64);
void outb(int64, int64);
int64 ·Pushcli(void);
void ·Popcli(int64);
uint64 ·rcr0(void);
uint64 rcr2(void);
uint64 rcr3(void);
uint64 ·rcr4(void);
uint64 ·Rdmsr(uint64);
uint64 ·rflags(void);
uint64 rrsp(void);
void ·sti(void);
void tlbflush(void);
void _trapret(uint64 *);
void wlap(uint32, uint32);
void ·Wrmsr(uint64, uint64);
void ·mktrap(uint64);
void ·fs_null(void);
void ·gs_null(void);

uint64 ·Rdtsc(void);

// src/runtime/sys_linux_amd64.s
void fakesig(int32, Siginfo *, void *);
void intsigret(void);

// src/runtime/os_linux.go
void runtime·cls(void);
void runtime·perfgather(uint64 *);
void runtime·perfmask(void);
void runtime·putch(int8);
void runtime·putcha(int8, int8);
void runtime·shadow_clear(void);

// src/runtime/proc.c
struct spinlock_t {
	volatile uint32 v;
};

void ·splock(struct spinlock_t *);
void ·spunlock(struct spinlock_t *);

// this file
void ·lap_eoi(void);
void runtime·deray(uint64);

void runtime·stackcheck(void);
void ·invlpg(void *);

extern struct spinlock_t *·pmsglock;

void ·_pnum(uint64 n);
void ·pnum(uint64 n);

#pragma textflag NOSPLIT
void
·_pmsg(int8 *msg)
{
	runtime·putch(' ');
	if (msg)
		while (*msg)
			runtime·putch(*msg++);
}

#pragma textflag NOSPLIT
void
pmsg(int8 *msg)
{
	int64 fl = ·Pushcli();
	·splock(·pmsglock);
	·_pmsg(msg);
	·spunlock(·pmsglock);
	·Popcli(fl);
}

uint32 ·Halt;

void runtime·pancake(void *msg, int64 addr);

#define assert(x, y, z)        do { if (!(x)) runtime·pancake(y, z); } while (0)

#pragma textflag NOSPLIT
static void
bw(uint8 *d, uint64 data, uint64 off)
{
	*d = (data >> off*8) & 0xff;
}

#define MAXCPUS 32

#define	CODE_SEG        1

// physical address of current pmap, given to us by bootloader
extern uint64 ·p_kpmap;

int8 gostr[] = "go";

#pragma textflag NOSPLIT
void
exam(uint64 cr0)
{
	USED(cr0);
	//pmsg(" first free ");
	//·pnum(first_free);

	pmsg("inspect cr0");

	if (cr0 & (1UL << 30))
		pmsg("CD set ");
	if (cr0 & (1UL << 29))
		pmsg("NW set ");
	if (cr0 & (1UL << 16))
		pmsg("WP set ");
	if (cr0 & (1UL << 5))
		pmsg("NE set ");
	if (cr0 & (1UL << 3))
		pmsg("TS set ");
	if (cr0 & (1UL << 2))
		pmsg("EM set ");
	if (cr0 & (1UL << 1))
		pmsg("MP set ");
}

#define PGSIZE          (1ULL << 12)
#define PGOFFMASK       (PGSIZE - 1)
#define PGMASK          (~PGOFFMASK)

#define ROUNDDOWN(x, y) ((x) & ~((y) - 1))
#define ROUNDUP(x, y)   (((x) + ((y) - 1)) & ~((y) - 1))

#define PML4X(x)        (((uint64)(x) >> 39) & 0x1ff)
#define PDPTX(x)        (((uint64)(x) >> 30) & 0x1ff)
#define PDX(x)          (((uint64)(x) >> 21) & 0x1ff)
#define PTX(x)          (((uint64)(x) >> 12) & 0x1ff)

#define PTE_W           (1ULL << 1)
#define PTE_U           (1ULL << 2)
#define PTE_P           (1ULL << 0)
#define PTE_PCD         (1ULL << 4)

#define PTE_ADDR(x)     ((x) & ~0x3ff)

// slot for recursive mapping
#define	VREC    0x42ULL
#define	VTEMP   0x43ULL
// vdirect is 44
#define	VREC2   0x45ULL

#define	VUMAX   0x42ULL		// highest runtime mapping

#define CADDR(m, p, d, t) ((uint64 *)(m << 39 | p << 30 | d << 21 | t << 12))

uint64 * ·pgdir_walk(void *va, uint8 create);

#pragma textflag NOSPLIT
void
stack_dump(uint64 rsp)
{
	uint64 *pte = ·pgdir_walk((void *)rsp, 0);
	·_pmsg("STACK DUMP\n");
	if (pte && *pte & PTE_P) {
		int32 i, pc = 0;
		uint64 *p = (uint64 *)rsp;
		for (i = 0; i < 70; i++) {
			pte = ·pgdir_walk(p, 0);
			if (pte && *pte & PTE_P) {
				·_pnum(*p++);
				if (((pc++ + 1) % 4) == 0)
					·_pmsg("\n");
			}
		}
	} else {
		pmsg("bad stack");
		·_pnum(rsp);
		if (pte) {
			·_pmsg("pte:");
			·_pnum(*pte);
		} else
			·_pmsg("null pte");
	}
}

#define TRAP_NMI	2
#define TRAP_PGFAULT	14
#define TRAP_SYSCALL	64
#define TRAP_TIMER	32
#define TRAP_DISK	(32 + 14)
#define TRAP_SPUR	48
#define TRAP_YIELD	49
#define TRAP_TLBSHOOT	70
#define TRAP_SIGRET	71
#define TRAP_PERFMASK	72

#define IRQ_BASE	32
#define IS_IRQ(x)	(x > IRQ_BASE && x <= IRQ_BASE + 15)
#define IS_CPUEX(x)	(x < IRQ_BASE)

// HZ timer interrupts/sec
#define HZ		100
static uint32 lapic_quantum;
// picoseconds per CPU cycle
uint64 ·Pspercycle;

struct thread_t {
// ======== don't forget to update the go definition too! ======
#define TFREGS       17
#define TFHW         7
#define TFSIZE       ((TFREGS + TFHW)*8)
	// general context
	uint64 tf[TFREGS + TFHW];
#define FXSIZE       512
#define FXREGS       (FXSIZE/8)
	// MMX/SSE state; must be 16 byte aligned or fx{save,rstor} will
	// generate #GP
	//uint64 _pad1;
	uint64 fx[FXREGS];
	// these both are pointers to go allocated memory, but they are only
	// non-nil during user program execution thus the GC will always find
	// tf and fxbuf via the G's syscallsp.
	struct user_t {
		uint64 tf;
		uint64 fxbuf;
	} user;
	// we could put this on the signal stack instead
	uint64 sigtf[TFREGS + TFHW];
	// don't care whether sigfx is 16 byte aligned since we never call
	// fx{save,rstor} on it directly.
	uint64 sigfx[FXREGS];
	uint64 sigstatus;
	uint64 sigsleepfor;
#define TF_RSP       (TFREGS + 5)
#define TF_RIP       (TFREGS + 2)
#define TF_CS        (TFREGS + 3)
#define TF_RFLAGS    (TFREGS + 4)
	#define		TF_FL_IF	(1 << 9)
#define TF_SS        (TFREGS + 6)
#define TF_TRAPNO    TFREGS
#define TF_RAX       16
#define TF_RBX       15
#define TF_RCX       14
#define TF_RDX       13
#define TF_RDI       12
#define TF_RSI       11
#define TF_RBP       10
#define TF_FSBASE    1
#define TF_SYSRSP    0

	int64 status;
#define ST_INVALID   0
#define ST_RUNNABLE  1
#define ST_RUNNING   2
#define ST_WAITING   3	// waiting for a trap to be serviced
#define ST_SLEEPING  4
#define ST_WILLSLEEP 5
	int32 doingsig;
	// stack for signals, provided by the runtime via sigaltstack.
	// sigtramp switches m->g to the signal g so that stack checks
	// pass and thus we don't try to grow the stack.
	uint64 sigstack;
	struct prof_t {
		uint64 enabled;
		uint64 totaltime;
		uint64 stampstart;
	} prof;

	uint64 sleepfor;
	uint64 sleepret;
#define ETIMEDOUT   110
	uint64 futaddr;
	uint64 p_pmap;
	//uint64 _pad2;
};

struct cpu_t {
// ======== don't forget to update the go definition too! ======
	// XXX missing go type info
	//struct thread_t *mythread;

	// if you add fields before rsp, asm in ·mktrap() needs to be updated

	// a pointer to this cpu_t
	uint64 this;
	uint64 mythread;
	uint64 rsp;
	uint64 num;
	// these are used only by Go code
	void *pmap;
	Slice pms;
	//uint64 pid;
};

#define NTHREADS        64
//static struct thread_t ·threads[NTHREADS];
extern struct thread_t ·threads[NTHREADS];
// index is lapic id
extern struct cpu_t ·cpus[MAXCPUS];

#define curcpu               (·cpus[·lap_id()])
#define curthread            ((struct thread_t *)(curcpu.mythread))
#define setcurthread(x)      (curcpu.mythread = (uint64)x)

extern struct spinlock_t *·threadlock;
extern struct spinlock_t *·futexlock;

static uint64 _gimmealign;
extern uint64 ·fxinit[512/8];

void ·fpuinit(int8);

// newtrap is a function pointer to a user provided trap handler. alltraps
// jumps to newtrap if it is non-zero.
uint64 newtrap;

static uint16 cpuattrs[MAXCPUS];
#pragma textflag NOSPLIT
void
cpuprint(uint8 n, int32 row)
{
	uint16 *p = (uint16*)0xb8000;
	uint64 num = ·Gscpu()->num;
	p += num + row*80;
	uint16 attr = cpuattrs[num];
	cpuattrs[num] += 0x100;
	*p = attr | n;
}

#pragma textflag NOSPLIT
void
·Cprint(byte n, int64 row)
{
	cpuprint((uint8)n, (int32)row);
}

#pragma textflag NOSPLIT
static void
cpupnum(uint64 rip)
{
	uint64 i;
	for (i = 0; i < 16; i++) {
		uint8 c = (rip >> i*4) & 0xf;
		if (c < 0xa)
			c += '0';
		else
			c = 'a' + (c - 0xa);
		cpuprint(c, i);
	}
}

void ·sched_halt(void);

uint64 ·hack_nanotime(void);

void ·sched_run(struct thread_t *t);

void ·yieldy(void);

#pragma textflag NOSPLIT
static uint64 *
pte_mapped(void *va)
{
	uint64 *pte = ·pgdir_walk(va, 0);
	if (!pte || (*pte & PTE_P) == 0)
		return nil;
	return pte;
}

#pragma textflag NOSPLIT
static void
assert_mapped(void *va, int64 size, int8 *msg)
{
	byte *p = (byte *)ROUNDDOWN((uint64)va, PGSIZE);
	byte *end = (byte *)ROUNDUP((uint64)va + size, PGSIZE);
	for (; p < end; p += PGSIZE)
		if (pte_mapped(p) == nil)
			runtime·pancake(msg, (uint64)va);
}

void ·wakeup(void);

extern uint64 ·tlbshoot_pmap;
extern uint64 ·tlbshoot_wait;

void ·tlb_shootdown(void);

#pragma textflag NOSPLIT
void
sigret(struct thread_t *t)
{
	assert(t->status == ST_RUNNING, "uh oh2", 0);

	// restore pre-signal context
	runtime·memmove(t->tf, t->sigtf, TFSIZE);
	runtime·memmove(t->fx, t->sigfx, FXSIZE);

	·splock(·threadlock);
	assert(t->sigstatus == ST_RUNNABLE || t->sigstatus == ST_SLEEPING,
	    "oh nyet", t->sigstatus);

	// allow new signals
	t->doingsig = 0;

	uint64 sf = t->sigsleepfor;
	uint64 st = t->sigstatus;
	t->sigsleepfor = t->sigstatus = 0;

	if (st == ST_WAITING) {
		t->sleepfor = sf;
		t->status = ST_WAITING;
		·yieldy();
	} else {
		// t->status is already ST_RUNNING
		·spunlock(·threadlock);
		·sched_run(t);
	}
}

// if sigsim() is used to deliver signals other than SIGPROF, you will need to
// construct siginfo_t and more of context.

// sigsim is executed by the runtime thread directly (ie not in interrupt
// context) on the signal stack. mksig() is used in interrupt context to setup
// and dispatch a signal context. we use an interrupt to restore pre-signal
// context because an interrupt switches to the interrupt stack so we can
// easily mark the task as signal-able again and restore old context (a task
// must be marked as signal-able only after the signal stack is no longer
// used).

// we could probably not use an interrupt and instead switch to pre-signal
// stack, then mark task as signal-able, and finally restore pre-signal
// context. the function implementing this should be not marked no-split
// though.
#pragma textflag NOSPLIT
void
sigsim(int32 signo, Siginfo *si, void *ctx)
{
	// SIGPROF handler doesn't use siginfo_t...
	USED(si);
	fakesig(signo, nil, ctx);
	//intsigret();
	·mktrap(TRAP_SIGRET);
}

// caller must hold threadlock
#pragma textflag NOSPLIT
void
mksig(struct thread_t *t, int32 signo)
{
	assert(t->sigstack != 0, "no sig stack", t->sigstack);
	// save old context for sigret
	// XXX
	if ((t->tf[TF_RFLAGS] & TF_FL_IF) == 0) {
		assert(t->status == ST_WILLSLEEP, "how the fuke", t->status);
		t->tf[TF_RFLAGS] |= TF_FL_IF;
	}
	runtime·memmove(t->sigtf, t->tf, TFSIZE);
	runtime·memmove(t->sigfx, t->fx, FXSIZE);
	t->sigsleepfor = t->sleepfor;
	t->sigstatus = t->status;
	t->status = ST_RUNNABLE;
	t->doingsig = 1;

	// these are defined by linux since we lie to the go build system that
	// we are running on linux...
	struct ucontext_t {
		uint64 uc_flags;
		uint64 uc_link;
		struct uc_stack_t {
			void *sp;
			int32 flags;
			uint64 size;
		} uc_stack;
		struct mcontext_t {
			//ulong	greg[23];
			uint64 r8;
			uint64 r9;
			uint64 r10;
			uint64 r11;
			uint64 r12;
			uint64 r13;
			uint64 r14;
			uint64 r15;
			uint64 rdi;
			uint64 rsi;
			uint64 rbp;
			uint64 rbx;
			uint64 rdx;
			uint64 rax;
			uint64 rcx;
			uint64 rsp;
			uint64 rip;
			uint64 eflags;
			uint16 cs;
			uint16 gs;
			uint16 fs;
			uint16 __pad0;
			uint64 err;
			uint64 trapno;
			uint64 oldmask;
			uint64 cr2;
			uint64	fpptr;
			uint64	res[8];
		} uc_mcontext;
		uint64 uc_sigmask;
	};

	uint64 *rsp = (uint64 *)t->sigstack;
	rsp -= sizeof(struct ucontext_t);
	struct ucontext_t *ctxt = (struct ucontext_t *)rsp;

	// the profiler only uses rip and rsp of the context...
	runtime·memclr((byte *)ctxt, sizeof(struct ucontext_t));
	ctxt->uc_mcontext.rip = t->tf[TF_RIP];
	ctxt->uc_mcontext.rsp = t->tf[TF_RSP];

	// simulate call to sigsim with args
	*--rsp = (uint64)ctxt;
	// nil siginfo_t
	*--rsp = 0;
	*--rsp = (uint64)signo;
	// bad return addr; shouldn't be reached
	*--rsp = 0;

	t->tf[TF_RSP] = (uint64)rsp;
	t->tf[TF_RIP] = (uint64)sigsim;
}

#pragma textflag NOSPLIT
static void
timetick(struct thread_t *t)
{
	uint64 elapsed = ·hack_nanotime() - t->prof.stampstart;
	t->prof.stampstart = 0;
	t->prof.totaltime += elapsed;
}

// caller must hold threadlock
#pragma textflag NOSPLIT
static void
proftick(void)
{
	const uint64 profns = 10000000;
	static uint64 lastprof;
	uint64 n = ·hack_nanotime();

	if (n - lastprof < profns)
		return;
	lastprof = n;

	int32 i;
	for (i = 0; i < NTHREADS; i++) {
		// if profiling the kernel, do fake SIGPROF if we aren't
		// already
		struct thread_t *t = &·threads[i];
		if (!t->prof.enabled || t->doingsig)
			continue;
		// don't touch running ·threads
		if (t->status != ST_RUNNABLE)
			continue;
		const int32 SIGPROF = 27;
		mksig(t, SIGPROF);
	}
}

#pragma textflag NOSPLIT
void
kernel_fault(uint64 *tf)
{
	uint64 trapno = tf[TF_TRAPNO];
	·_pmsg("trap frame at");
	·_pnum((uint64)tf);
	·_pmsg("trapno");
	·_pnum(trapno);
	uint64 rip = tf[TF_RIP];
	·_pmsg("rip");
	·_pnum(rip);
	if (trapno == TRAP_PGFAULT) {
		uint64 rcr2(void);
		uint64 cr2 = rcr2();
		·_pmsg("cr2");
		·_pnum(cr2);
	}
	uint64 rsp = tf[TF_RSP];
	stack_dump(rsp);
	runtime·pancake("kernel fault", trapno);
}

// XXX
// may want to only ·wakeup() on most timer ints since now there is more
// overhead for timer ints during user time.
#pragma textflag NOSPLIT
void
trap(uint64 *tf)
{
	uint64 trapno = tf[TF_TRAPNO];

	if (trapno == TRAP_NMI) {
		runtime·perfgather(tf);
		runtime·perfmask();
		_trapret(tf);
	}

	lcr3(·p_kpmap);

	// CPU exceptions in kernel mode are fatal errors
	if (trapno < TRAP_TIMER && (tf[TF_CS] & 3) == 0)
		kernel_fault(tf);

	if (·Gscpu() != &curcpu) {
		·pnum((uint64)·Gscpu());
		·pnum((uint64)&curcpu);
		runtime·pancake("gs is wrong", 0);
	}

	struct thread_t *ct = curthread;

	assert((·rflags() & TF_FL_IF) == 0, "ints enabled in trap", 0);

	if (·Halt)
		while (1);

	// clear shadow pointers to user pmap
	runtime·shadow_clear();

	// don't add code before FPU context saving unless you've thought very
	// carefully! it is easy to accidentally and silently corrupt FPU state
	// (ie calling runtime·memmove) before it is saved below.

	// save FPU state immediately before we clobber it
	if (ct) {
		// if in user mode, save to user buffers and make it look like
		// Userrun returned.
		if (ct->user.tf) {
			uint64 *ufx = (uint64 *)ct->user.fxbuf;
			uint64 *utf = (uint64 *)ct->user.tf;
			·fxsave(ufx);
			runtime·memmove(utf, tf, TFSIZE);
			// runtime/asm_amd64.s
			void _userint(void);
			ct->tf[TF_RIP] = (uint64)_userint;
			ct->tf[TF_RSP] = utf[TF_SYSRSP];
			ct->tf[TF_RAX] = trapno;
			ct->tf[TF_RBX] = rcr2();
			// XXXPANIC
			if (trapno == TRAP_YIELD || trapno == TRAP_SIGRET)
				runtime·pancake("nyet", trapno);
			// if we are unlucky enough for a timer int to come in
			// before we execute the first instruction of the new
			// rip, make sure the state we just saved isn't
			// clobbered
			ct->user.tf = 0;
			ct->user.fxbuf = 0;
		} else {
			·fxsave(ct->fx);
			runtime·memmove(ct->tf, tf, TFSIZE);
		}
		timetick(ct);
	}

	int32 yielding = 0;
	// these interrupts are handled specially by the runtime
	if (trapno == TRAP_YIELD) {
		trapno = TRAP_TIMER;
		tf[TF_TRAPNO] = TRAP_TIMER;
		yielding = 1;
	}

	void (*ntrap)(uint64 *, int64);
	ntrap = (void (*)(uint64 *, int64))newtrap;

	if (trapno == TRAP_TLBSHOOT) {
		// does not return
		·tlb_shootdown();
	} else if (trapno == TRAP_TIMER) {
		·splock(·threadlock);
		if (ct) {
			if (ct->status == ST_WILLSLEEP) {
				ct->status = ST_SLEEPING;
				// XXX set IF, unlock
				ct->tf[TF_RFLAGS] |= TF_FL_IF;
				·spunlock(·futexlock);
			} else
				ct->status = ST_RUNNABLE;
		}
		if (!yielding) {
			·lap_eoi();
			if (curcpu.num == 0) {
				·wakeup();
				proftick();
			}
		}
		// ·yieldy doesn't return
		·yieldy();
	} else if (IS_IRQ(trapno)) {
		if (ntrap) {
			// catch kernel faults that occur while trying to
			// handle user traps
			ntrap(tf, 0);
		} else
			runtime·pancake("IRQ without ntrap", trapno);
		if (ct)
			·sched_run(ct);
		else
			·sched_halt();
	} else if (IS_CPUEX(trapno)) {
		// we vet out kernel mode CPU exceptions above; must be from
		// user program. thus return from Userrun() to kernel.
		·sched_run(ct);
	} else if (trapno == TRAP_SIGRET) {
		// does not return
		sigret(ct);
	} else if (trapno == TRAP_PERFMASK) {
		·lap_eoi();
		runtime·perfmask();
		if (ct)
			·sched_run(ct);
		else
			·sched_halt();
	} else {
		runtime·pancake("unexpected int", trapno);
	}
	// not reached
	runtime·pancake("no returning", 0);
}

static uint64 lapaddr;

uint64 ·tss_init(int32 myid);

#pragma textflag NOSPLIT
uint32
rlap(uint32 reg)
{
	if (!lapaddr)
		runtime·pancake("lapaddr null?", lapaddr);
	volatile uint32 *p = (uint32 *)lapaddr;
	return p[reg];
}

#pragma textflag NOSPLIT
void
wlap(uint32 reg, uint32 val)
{
	if (!lapaddr)
		runtime·pancake("lapaddr null?", lapaddr);
	volatile uint32 *p = (uint32 *)lapaddr;
	p[reg] = val;
}

#pragma textflag NOSPLIT
uint64
·lap_id(void)
{
	assert((·rflags() & TF_FL_IF) == 0, "ints enabled for lapid", 0);

	if (!lapaddr)
		runtime·pancake("lapaddr null (id)", lapaddr);
	volatile uint32 *p = (uint32 *)lapaddr;

#define IDREG       (0x20/4)
	return p[IDREG] >> 24;
}

#pragma textflag NOSPLIT
void
·lap_eoi(void)
{
	assert(lapaddr, "lapaddr null?", lapaddr);

#define EOIREG      (0xb0/4)
	wlap(EOIREG, 0);
}

#pragma textflag NOSPLIT
uint64
ticks_get(void)
{
#define CCREG       (0x390/4)
	return lapic_quantum - rlap(CCREG);
}

#pragma textflag NOSPLIT
int64
pit_ticks(void)
{
#define CNT0		0x40
#define CNTCTL		0x43
	// counter latch command for counter 0
	int64 cmd = 0;
	outb(CNTCTL, cmd);
	int64 low = inb(CNT0);
	int64 high = inb(CNT0);
	return high << 8 | low;
}

#pragma textflag NOSPLIT
// wait until 8254 resets the counter
void
pit_phasewait(void)
{
	// 8254 timers are 16 bits, thus always smaller than last;
	int64 last = 1 << 16;
	for (;;) {
		int64 cur = pit_ticks();
		if (cur > last)
			return;
		last = cur;
	}
}

#pragma textflag NOSPLIT
void
timer_setup(int32 calibrate)
{
	uint64 la = 0xfee00000ULL;

	// map lapic IO mem
	uint64 *pte = ·pgdir_walk((void *)la, 1);
	*pte = (uint64)la | PTE_W | PTE_P | PTE_PCD;
	lapaddr = la;
#define LVERSION    (0x30/4)
	uint32 lver = rlap(LVERSION);
	if (lver < 0x10)
		runtime·pancake("82489dx not supported", lver);

#define LVTIMER     (0x320/4)
#define DCREG       (0x3e0/4)
#define DIVONE      0xb
#define ICREG       (0x380/4)

#define MASKINT   (1 << 16)

#define LVSPUR     (0xf0/4)
	// enable lapic, set spurious int vector
	wlap(LVSPUR, 1 << 8 | TRAP_SPUR);

	// timer: periodic, int 32
	wlap(LVTIMER, 1 << 17 | TRAP_TIMER);
	// divide by
	wlap(DCREG, DIVONE);

	if (calibrate) {
		// figure out how many lapic ticks there are in a second; first
		// setup 8254 PIT since it has a known clock frequency. openbsd
		// uses a similar technique.
		const uint32 pitfreq = 1193182;
		const uint32 pithz = 100;
		const uint32 div = pitfreq/pithz;
		// rate generator mode, lsb then msb (if square wave mode is
		// used, the PIT uses div/2 for the countdown since div is
		// taken to be the period of the wave)
		outb(CNTCTL, 0x34);
		outb(CNT0, div & 0xff);
		outb(CNT0, div >> 8);

		// start lapic counting
		wlap(ICREG, 0x80000000);
		pit_phasewait();
		uint32 lapstart = rlap(CCREG);
		uint64 cycstart = ·Rdtsc();

		int32 i;
		for (i = 0; i < pithz; i++)
			pit_phasewait();

		uint32 lapend = rlap(CCREG);
		if (lapend > lapstart)
			runtime·pancake("lapic timer wrapped?", lapend);
		uint32 lapelapsed = lapstart - lapend;
		uint64 cycelapsed = ·Rdtsc() - cycstart;
		pmsg("LAPIC Mhz:");
		·pnum(lapelapsed/(1000 * 1000));
		pmsg("\n");
		lapic_quantum = lapelapsed / HZ;

		pmsg("CPU Mhz:");
		extern uint64 runtime·Cpumhz;
		runtime·Cpumhz = cycelapsed/(1000 * 1000);
		·pnum(runtime·Cpumhz);
		pmsg("\n");
		·Pspercycle = (1000000000000ull)/cycelapsed;

		// disable PIT: one-shot, lsb then msb
		outb(CNTCTL, 0x32);
		outb(CNT0, div & 0xff);
		outb(CNT0, div >> 8);
	}

	// initial count; the LAPIC's frequency is not the same as the CPU's
	// frequency
	wlap(ICREG, lapic_quantum);

#define LVCMCI      (0x2f0/4)
#define LVINT0      (0x350/4)
#define LVINT1      (0x360/4)
#define LVERROR     (0x370/4)
#define LVPERF      (0x340/4)
#define LVTHERMAL   (0x330/4)

	// mask cmci, lint[01], error, perf counters, and thermal sensor
	wlap(LVCMCI,    MASKINT);
	// masking LVINT0 somewhow results in a GPfault?
	//wlap(LVINT0,    1 << MASKSHIFT);
	wlap(LVINT1,    MASKINT);
	wlap(LVERROR,   MASKINT);
	wlap(LVPERF,    MASKINT);
	wlap(LVTHERMAL, MASKINT);

#define IA32_APIC_BASE   0x1b
	uint64 reg = ·Rdmsr(IA32_APIC_BASE);
	if (!(reg & (1 << 11)))
		runtime·pancake("lapic disabled?", reg);
	if (reg >> 12 != 0xfee00)
		runtime·pancake("weird base addr?", reg >> 12);

	uint32 lreg = rlap(LVSPUR);
	if (lreg & (1 << 12))
		pmsg("EOI broadcast surpression\n");
	if (lreg & (1 << 9))
		pmsg("focus processor checking\n");
	if (!(lreg & (1 << 8)))
		pmsg("apic disabled\n");
}

#pragma textflag NOSPLIT
void
sysc_setup(uint64 myrsp)
{
	// lowest 2 bits are ignored for sysenter, but used for sysexit
	const uint64 kcode64 = 1 << 3 | 3;
	const uint64 sysenter_cs = 0x174;
	·Wrmsr(sysenter_cs, kcode64);

	const uint64 sysenter_eip = 0x176;
	// asm_amd64.s
	void _sysentry(void);
	·Wrmsr(sysenter_eip, (uint64)_sysentry);

	const uint64 sysenter_esp = 0x175;
	·Wrmsr(sysenter_esp, myrsp);
}

#pragma textflag NOSPLIT
void
gs_set(struct cpu_t *mycpu)
{
	// we must set fs/gs, the only segment descriptors in ia32e mode, at
	// least once before we use the MSRs to change their base address. the
	// MSRs write directly to hidden segment descriptor cache, and if we
	// don't explicitly fill the segment descriptor cache, the writes to
	// the MSRs are thrown out (presumably because the caches are thought
	// to be invalid).
	·gs_null();
	const uint64 ia32_gs_base = 0xc0000101;
	·Wrmsr(ia32_gs_base, (uint64)mycpu);
}

#pragma textflag NOSPLIT
void
proc_setup(void)
{
	// fpuinit must be called before pgdir_walk or tss_init since
	// pgdir_walk may call memclr which uses SSE instructions to zero newly
	// allocated pages.
	·fpuinit(1);

	assert(sizeof(·threads[0].tf) == TFSIZE, "weird tf size",
	    sizeof(·threads[0].tf));
	assert(sizeof(·threads[0].fx) == FXSIZE, "weird fx size",
	    sizeof(·threads[0].fx));
	·threads[0].status = ST_RUNNING;
	·threads[0].p_pmap = ·p_kpmap;

	uint64 la = 0xfee00000ULL;
	uint64 *pte = ·pgdir_walk((void *)la, 0);
	if (pte && *pte & PTE_P)
		runtime·pancake("lapic mem mapped?", (uint64)pte);

	int32 i;
	for (i = 0; i < MAXCPUS; i++)
		·cpus[i].this = (uint64)&·cpus[i];

	timer_setup(1);

	// 8259a - mask all irqs. see 2.5.3.6 in piix3 documentation.
	// otherwise an RTC timer interrupt (that turns into a double-fault
	// since the pic has not been programmed yet) comes in immediately
	// after ·sti.
	outb(0x20 + 1, 0xff);
	outb(0xa0 + 1, 0xff);

	uint64 myrsp = ·tss_init(0);
	sysc_setup(myrsp);
	curcpu.num = 0;
	gs_set(&curcpu);
	setcurthread(&·threads[0]);
	//pmsg("sizeof thread_t:");
	//·pnum(sizeof(struct thread_t));
	//pmsg("\n");

	for (i = 0; i < NTHREADS; i++)
		if ((uint64)·threads[i].fx & ((1 << 4) - 1))
			assert(0, "fx not 16 byte aligned", i);
}

#pragma textflag NOSPLIT
void
·Ap_setup(int64 myid)
{
	pmsg("cpu");
	·pnum(myid);
	pmsg("joined\n");
	assert(myid >= 0 && myid < MAXCPUS, "id id large", myid);
	assert(·lap_id() <= MAXCPUS, "lapic id large", myid);
	·fpuinit(0);
	timer_setup(0);
	uint64 myrsp = ·tss_init(myid);
	sysc_setup(myrsp);
	assert(curcpu.num == 0, "slot taken", curcpu.num);

	int64 test = ·Pushcli();
	assert((test & TF_FL_IF) == 0, "wtf!", test);
	·Popcli(test);
	assert((·rflags() & TF_FL_IF) == 0, "wtf!", test);

	curcpu.num = myid;
	·fs_null();
	gs_set(&curcpu);
	setcurthread(0);
}

struct timespec {
	int64 tv_sec;
	int64 tv_nsec;
};

// exported functions
// XXX remove the NOSPLIT once i've figured out why newstack is being called
// for a program whose stack doesn't seem to grow
#pragma textflag NOSPLIT
void
runtime·Cli(void)
{
	runtime·stackcheck();

	·cli();
}

#pragma textflag NOSPLIT
uint64
runtime·Fnaddr(uint64 *fn)
{
	runtime·stackcheck();

	return *fn;
}

#pragma textflag NOSPLIT
uint64
runtime·Fnaddri(uint64 *fn)
{
	return runtime·Fnaddr(fn);
}

#pragma textflag NOSPLIT
uint64*
runtime·Kpmap(void)
{
	runtime·stackcheck();

	return CADDR(VREC, VREC, VREC, VREC);
}

#pragma textflag NOSPLIT
uint64
runtime·Kpmap_p(void)
{
	return ·p_kpmap;
}

#pragma textflag NOSPLIT
void
runtime·Lcr3(uint64 pmap)
{
	lcr3(pmap);
}

#pragma textflag NOSPLIT
uint64
runtime·Rcr3(void)
{
	runtime·stackcheck();

	return rcr3();
}

#pragma textflag NOSPLIT
int64
runtime·Inb(uint64 reg)
{
	runtime·stackcheck();
	int64 ret = inb(reg);
	return ret;
}

#pragma textflag NOSPLIT
void
runtime·Install_traphandler(uint64 *p)
{
	runtime·stackcheck();

	newtrap = *p;
}

#pragma textflag NOSPLIT
void
runtime·Pnum(uint64 m)
{
	if (runtime·hackmode)
		·pnum(m);
}

#pragma textflag NOSPLIT
uint64
runtime·Rcr2(void)
{
	return rcr2();
}

#pragma textflag NOSPLIT
void
runtime·Sti(void)
{
	runtime·stackcheck();

	·sti();
}

#pragma textflag NOSPLIT
uint64
runtime·Vtop(void *va)
{
	runtime·stackcheck();

	uint64 van = (uint64)va;
	uint64 *pte = pte_mapped(va);
	if (!pte)
		return 0;
	uint64 base = PTE_ADDR(*pte);

	return base + (van & PGOFFMASK);
}

#pragma textflag NOSPLIT
void
runtime·Crash(void)
{
	pmsg("CRASH!\n");
	volatile uint32 *wtf = &·Halt;
	*wtf = 1;
	while (1);
}

#pragma textflag NOSPLIT
void
runtime·Pmsga(uint8 *msg, int64 len, int8 a)
{
	runtime·stackcheck();

	int64 fl = ·Pushcli();
	·splock(·pmsglock);
	while (len--) {
		runtime·putcha(*msg++, a);
	}
	·spunlock(·pmsglock);
	·Popcli(fl);
}
