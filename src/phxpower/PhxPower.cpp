//
//  PhxPower.cpp — CPU power management for AMD Phoenix / Hawk Point (Zen 4) on macOS.
//
//  Why: with OpenCore's DummyPowerManagement there is no CPU power management at all. XNU idles every core with a bare
//  HLT (C1 only) and never touches AMD's CPPC request register, so the cores sit at high clocks and the package never
//  reaches a deep sleep state. Measured on the Lenovo IdeaPad Slim 5 16AHP10 (Ryzen 5 8645HS): about 43 W at an idle
//  desktop under macOS against 5-8 W under Linux at the same panel brightness.
//
//  What it does (each part is a boot-arg level, `phxpow=N`):
//    1 (default)  CPPC: writes MSR C001_02B3 on every logical CPU, energy-performance preference = balance_power, min =
//                 lowest perf, max = highest perf, desired = 0 (hardware autonomous). Linux's amd-pstate-epp does the
//                 same. No kernel hooks, nothing else changes.
//    2            + registers a minimal pmDispatch table so XNU's machine_idle() calls MachineIdle() instead of HLT,
//                 and idles with HLT (C1) or the ACPI I/O-port C2 state (CStateBase+1).
//    3            + the ACPI I/O-port C3 state (CStateBase+2, core power gating / CC6). Linux's cpuidle uses exactly
//                 these ports on this machine (_CST: FFH C1, SystemIO 0x414 C2 18 us, SystemIO 0x415 C3 350 us).
//
//  Idle state choice: per CPU, predicted idle = how long the previous idle lasted (drops at once when an idle is
//  shorter than predicted, rises slowly), C3 only when predicted >= debug.phxpower_c3us, C2 only when >= c2us.
//
//  Boot-args: phxpow=0..3, phxpow-epp=0..255 (0 = performance, 255 = power; default 191), phxpow-min / phxpow-max
//  (CPPC perf units, 0 = hardware default), phxpow-any (do not insist on a Family 19h model 70h-7Fh CPU),
//  -phxpoweroff (disable everything). Live: sysctl debug.phxpower_maxc (0/1 = HLT only, 2, 3) selects the deepest state
//  allowed; debug.phxpower_stats and debug.phxpower_cppc show what happened.
//
//  If the machine hangs at boot with phxpow=2/3, boot with -phxpoweroff or phxpow=1.
//

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/OSAtomic.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <i386/proc_reg.h>
extern "C" {
#include <i386/pmCPU.h>   // MacKernelSDK has no C++ guards: without extern "C" pmKextRegister is mangled and OpenCore cannot resolve it
}
#include <kern/thread_call.h>
#include <kern/clock.h>

extern "C" int cpu_number(void);
extern "C" void mp_rendezvous_no_intrs(void (*action_func)(void *), void *arg);

#define PHXPOWER_BUILD 1u

static constexpr uint32_t kMsrCStateBase   = 0xC0010073;
static constexpr uint32_t kMsrCppcCap1     = 0xC00102B0;
static constexpr uint32_t kMsrCppcEnable   = 0xC00102B1;
static constexpr uint32_t kMsrCppcReq      = 0xC00102B3;
static constexpr unsigned kMaxCpu          = 64;      // power of two

static int gLevel = 1;
static int gMaxC = 1;                // live: deepest state allowed (<=1: HLT only)
static int gC2us = 80, gC3us = 1000; // live: predicted-idle thresholds
static uint32_t gEpp = 191, gMinOv = 0, gMaxOv = 0;
static uint16_t gPortC2, gPortC3;
static bool gHooked;
static pmDispatch_t gDisp;
static pmCallBacks_t gCb;

struct alignas(64) CpuStat {
    uint64_t pred;                   // predicted idle, ns
    uint64_t n1, n2, n3;             // idles entered per state
    uint64_t t1, t2, t3;             // ns spent per state (includes the interrupt that woke it)
};
static CpuStat gStat[kMaxCpu];

struct CppcRec { uint64_t cap1, oldReq, newReq; uint32_t ok; };
static CppcRec gRec[kMaxCpu];

static inline void cpuidx(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

// ---- CPPC ------------------------------------------------------------------------------------------------------------

static void cppcAction(void *) {
    const unsigned c = (unsigned)cpu_number();
    if (c >= kMaxCpu) return;
    const uint64_t cap = rdmsr64(kMsrCppcCap1);
    const uint32_t hi = (uint32_t)(cap >> 24) & 0xff, lo = (uint32_t)cap & 0xff;
    gRec[c].cap1 = cap;
    if (!hi || !lo || hi < lo) { gRec[c].ok = 0; return; }
    uint32_t mx = gMaxOv ? (gMaxOv < hi ? gMaxOv : hi) : hi;
    uint32_t mn = gMinOv ? (gMinOv > lo ? gMinOv : lo) : lo;
    if (mx < lo) mx = lo;
    if (mn > mx) mn = mx;
    gRec[c].oldReq = rdmsr64(kMsrCppcReq);
    if (!(rdmsr64(kMsrCppcEnable) & 1)) wrmsr64(kMsrCppcEnable, 1);
    wrmsr64(kMsrCppcReq, ((uint64_t)(gEpp & 0xff) << 24) | ((uint64_t)mn << 8) | mx);
    gRec[c].newReq = rdmsr64(kMsrCppcReq);
    gRec[c].ok = 1;
}

// ---- idle ------------------------------------------------------------------------------------------------------------

// Called by XNU's machine_idle() with interrupts disabled; must return with interrupts disabled.
// "sti" followed directly by HLT / IN is atomic (one-instruction interrupt shadow), so a wakeup cannot be lost.
static uint64_t phxMachineIdle(uint64_t) {
    CpuStat *s = &gStat[(unsigned)cpu_number() & (kMaxCpu - 1)];
    const int maxc = gMaxC;
    const uint64_t pred = s->pred;
    const uint64_t t0 = mach_absolute_time();
    int st = 1;
    if (maxc >= 3 && gPortC3 && pred >= (uint64_t)gC3us * 1000ull) {
        uint8_t v;
        st = 3;
        __asm__ volatile("sti; inb %%dx, %%al; cli" : "=a"(v) : "d"(gPortC3) : "memory");
    } else if (maxc >= 2 && gPortC2 && pred >= (uint64_t)gC2us * 1000ull) {
        uint8_t v;
        st = 2;
        __asm__ volatile("sti; inb %%dx, %%al; cli" : "=a"(v) : "d"(gPortC2) : "memory");
    } else {
        __asm__ volatile("sti; hlt; cli" ::: "memory");
    }
    uint64_t dt = mach_absolute_time() - t0;
    if (st == 1) { s->n1++; s->t1 += dt; } else if (st == 2) { s->n2++; s->t2 += dt; } else { s->n3++; s->t3 += dt; }
    if (dt > 50000000ull) dt = 50000000ull;
    s->pred = dt < pred ? dt : (pred * 3 + dt) / 4;
    return 0;
}

// ---- sysctl ------------------------------------------------------------------------------------------------------------

SYSCTL_INT(_debug, OID_AUTO, phxpower_maxc, CTLFLAG_RW | CTLFLAG_LOCKED, &gMaxC, 0,
           "deepest idle state allowed: <=1 HLT only, 2 +C2, 3 +C3 (only has effect when phxpow>=2)");
SYSCTL_INT(_debug, OID_AUTO, phxpower_c2us, CTLFLAG_RW | CTLFLAG_LOCKED, &gC2us, 0, "min predicted idle for C2, us");
SYSCTL_INT(_debug, OID_AUTO, phxpower_c3us, CTLFLAG_RW | CTLFLAG_LOCKED, &gC3us, 0, "min predicted idle for C3, us");
SYSCTL_INT(_debug, OID_AUTO, phxpower_level, CTLFLAG_RD | CTLFLAG_LOCKED, &gLevel, 0, "phxpow level in effect");

static int phxStatsHandler SYSCTL_HANDLER_ARGS {
    char buf[640];
    uint64_t n1 = 0, n2 = 0, n3 = 0, t1 = 0, t2 = 0, t3 = 0;
    for (unsigned i = 0; i < kMaxCpu; i++) {
        n1 += gStat[i].n1; n2 += gStat[i].n2; n3 += gStat[i].n3;
        t1 += gStat[i].t1; t2 += gStat[i].t2; t3 += gStat[i].t3;
    }
    snprintf(buf, sizeof buf, "build=%u level=%d hooked=%d maxc=%d ports=%x/%x  C1/HLT n=%llu t=%llums  C2 n=%llu t=%llums  C3 n=%llu t=%llums",
             PHXPOWER_BUILD, gLevel, gHooked ? 1 : 0, gMaxC, gPortC2, gPortC3,
             n1, t1 / 1000000ull, n2, t2 / 1000000ull, n3, t3 / 1000000ull);
    return SYSCTL_OUT(req, buf, strlen(buf) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, phxpower_stats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, phxStatsHandler, "A",
            "idle state counts and residency");

static int phxCppcHandler SYSCTL_HANDLER_ARGS {
    char buf[1536];
    size_t o = 0;
    for (unsigned i = 0; i < kMaxCpu && o + 100 < sizeof buf; i++) {
        if (!gRec[i].cap1) continue;
        o += snprintf(buf + o, sizeof buf - o, "cpu%u cap1=%llx old=%llx new=%llx ok=%u\n", i, gRec[i].cap1, gRec[i].oldReq,
                      gRec[i].newReq, gRec[i].ok);
    }
    if (!o) buf[0] = 0;
    return SYSCTL_OUT(req, buf, strlen(buf) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, phxpower_cppc, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, phxCppcHandler, "A",
            "CPPC request written per CPU");


// ---- NVRAM: level override and the "did the last boot survive" marker ---------------------------------------------------
// NVRAM `phxpow` (a number as text) overrides the boot-arg, so the level can be changed from macOS with
// `sudo nvram phxpow=3` and undone with `sudo nvram phxpow=1`. When a boot with level >= 2 starts it sets `phxpow-trying`;
// it is removed after kHealthySeconds of uptime or at an orderly shutdown. If the marker is still there at the next boot
// the last one hung or panicked, so this boot falls back to level 1 and writes `phxpow=1` to NVRAM.

static constexpr uint32_t kHealthySeconds = 1200;
static thread_call_t gHealthCall;

static bool nvramRead(const char *name, uint32_t *out, bool *present) {
    *present = false;
    IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!opts) return false;
    OSObject *o = opts->copyProperty(name);
    char buf[16] = {0};
    if (OSData *d = OSDynamicCast(OSData, o)) { unsigned n = d->getLength() < 15 ? d->getLength() : 15; memcpy(buf, d->getBytesNoCopy(), n); }
    else if (OSString *st = OSDynamicCast(OSString, o)) strlcpy(buf, st->getCStringNoCopy(), sizeof buf);
    else if (OSNumber *nn = OSDynamicCast(OSNumber, o)) snprintf(buf, sizeof buf, "%u", nn->unsigned32BitValue());
    *present = o != nullptr;
    OSSafeReleaseNULL(o);
    opts->release();
    uint32_t v = 0;
    const char *c = buf;
    bool digits = false;
    for (; *c >= '0' && *c <= '9'; c++) { v = v * 10 + (uint32_t)(*c - '0'); digits = true; }
    if (digits) *out = v;
    return digits;
}

static void nvramSync(IORegistryEntry *opts) {
    if (OSString *why = OSString::withCString("phxpower")) { opts->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", why); why->release(); }
}

static void nvramPut(const char *name, const char *value) {
    IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!opts) return;
    if (OSString *str = OSString::withCString(value)) { opts->setProperty(name, str); str->release(); }
    nvramSync(opts);
    opts->release();
}

static void nvramDrop(const char *name) {
    IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!opts) return;
    OSObject *o = opts->copyProperty(name);
    if (o) { o->release(); opts->removeProperty(name); nvramSync(opts); }
    opts->release();
}

static void healthyCall(thread_call_param_t, thread_call_param_t) {
    nvramDrop("phxpow-trying");
    IOLog("PhxPower: %u s of uptime at level %d, marked this boot healthy\n", kHealthySeconds, gLevel);
}

// ---- service ------------------------------------------------------------------------------------------------------------

class PhxPower : public IOService {
    OSDeclareDefaultStructors(PhxPower)
public:
    bool start(IOService *provider) override;
    void systemWillShutdown(IOOptionBits specifier) override;
};
OSDefineMetaClassAndStructors(PhxPower, IOService)

void PhxPower::systemWillShutdown(IOOptionBits specifier) {
    nvramDrop("phxpow-trying");   // an orderly restart is not a hang
    IOService::systemWillShutdown(specifier);
}

bool PhxPower::start(IOService *provider) {
    if (!IOService::start(provider)) return false;

    char dummy[8];
    if (PE_parse_boot_argn("-phxpoweroff", dummy, sizeof dummy)) {
        IOLog("PhxPower: -phxpoweroff, doing nothing\n");
        return false;
    }
    uint32_t v;
    if (PE_parse_boot_argn("phxpow", &v, sizeof v)) gLevel = (int)(v > 3 ? 3 : v);
    bool present;
    if (nvramRead("phxpow", &v, &present)) gLevel = (int)(v > 3 ? 3 : v);   // NVRAM wins over the boot-arg
    if (PE_parse_boot_argn("phxpow-epp", &v, sizeof v)) gEpp = v & 0xff;
    if (PE_parse_boot_argn("phxpow-min", &v, sizeof v)) gMinOv = v & 0xff;
    if (PE_parse_boot_argn("phxpow-max", &v, sizeof v)) gMaxOv = v & 0xff;

    sysctl_register_oid(&sysctl__debug_phxpower_maxc);
    sysctl_register_oid(&sysctl__debug_phxpower_c2us);
    sysctl_register_oid(&sysctl__debug_phxpower_c3us);
    sysctl_register_oid(&sysctl__debug_phxpower_level);
    sysctl_register_oid(&sysctl__debug_phxpower_stats);
    sysctl_register_oid(&sysctl__debug_phxpower_cppc);

    // Hardware gate: AMD Family 19h (Zen 4), model 70h-7Fh (Phoenix / Hawk Point), CPPC advertised.
    uint32_t a, b, c, d;
    cpuidx(0, 0, &a, &b, &c, &d);
    const bool amd = b == 0x68747541 && d == 0x69746e65 && c == 0x444d4163;   // "AuthenticAMD"
    cpuidx(1, 0, &a, &b, &c, &d);
    const uint32_t fam = ((a >> 8) & 0xf) + ((a >> 20) & 0xff);
    const uint32_t mod = ((a >> 4) & 0xf) | (((a >> 16) & 0xf) << 4);
    cpuidx(0x80000008, 0, &a, &b, &c, &d);
    const bool cppc = (b >> 27) & 1;
    const bool any = PE_parse_boot_argn("phxpow-any", dummy, sizeof dummy);
    IOLog("PhxPower: build %u level %d, AMD=%d family %x model %x CPPC=%d\n", PHXPOWER_BUILD, gLevel, amd, fam, mod, cppc);
    if (!amd || !cppc || (!any && !(fam == 0x19 && mod >= 0x70 && mod <= 0x7f))) {
        IOLog("PhxPower: not a Phoenix/Hawk Point CPU with CPPC, leaving the CPU alone (phxpow-any overrides)\n");
        gLevel = 0;
        return true;
    }

    if (gLevel >= 1) {
        mp_rendezvous_no_intrs(cppcAction, nullptr);
        unsigned okc = 0;
        for (unsigned i = 0; i < kMaxCpu; i++) {
            if (!gRec[i].cap1) continue;
            if (gRec[i].ok) okc++;
            IOLog("PhxPower: cpu%u CPPC cap1 %llx request %llx -> %llx%s\n", i, gRec[i].cap1, gRec[i].oldReq, gRec[i].newReq,
                  gRec[i].ok ? "" : " (skipped)");
        }
        IOLog("PhxPower: CPPC set on %u CPUs (epp %u)\n", okc, gEpp);
    }

    if (gLevel >= 2) {
        uint32_t dv;
        bool tried;
        nvramRead("phxpow-trying", &dv, &tried);
        if (tried) {
            IOLog("PhxPower: the last boot with level >= 2 never finished (marker phxpow-trying), falling back to level 1 and setting NVRAM phxpow=1\n");
            nvramDrop("phxpow-trying");
            nvramPut("phxpow", "1");
            gLevel = 1;
        } else {
            nvramPut("phxpow-trying", "1");
            gHealthCall = thread_call_allocate(healthyCall, nullptr);
            if (gHealthCall) {
                uint64_t dl;
                clock_interval_to_deadline(kHealthySeconds, NSEC_PER_SEC, &dl);
                thread_call_enter_delayed(gHealthCall, dl);
            }
        }
    }

    if (gLevel >= 2) {
        const uint64_t cs = rdmsr64(kMsrCStateBase);
        const uint32_t base = (uint32_t)cs & 0xffff;
        IOLog("PhxPower: CStateBaseAddr MSR %llx -> I/O base %x\n", cs, base);
        if (base == 0 || base > 0xfff0) {
            IOLog("PhxPower: no usable C-state I/O base, idle stays HLT\n");
        } else {
            gPortC2 = (uint16_t)(base + 1);
            gPortC3 = (uint16_t)(base + 2);
            gMaxC = gLevel >= 3 ? 3 : 2;
            gDisp.MachineIdle = phxMachineIdle;
            pmKextRegister(PM_DISPATCH_VERSION, &gDisp, &gCb);
            gHooked = true;
            if (gCb.initComplete) gCb.initComplete();
            IOLog("PhxPower: idle hook installed, C2 port %x C3 port %x, deepest state C%d\n", gPortC2, gPortC3, gMaxC);
        }
    }
    registerService();
    return true;
}
