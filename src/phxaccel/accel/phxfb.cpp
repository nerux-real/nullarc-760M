// PhxFB: the 780M's display as an IOFramebuffer that PhxAccel (NMXAccel) composites for WindowServer.
// Like NullMoth's NMIntelFB (phase 1), it takes over the mode the firmware (GOP) already lit, at boot in place of the
// firmware framebuffer (IONDRV), and writes nothing to the display engine: the scan-out surface is the boot console's
// (IOPlatformExpert::getConsoleInfo), the accelerator's display pipe copies each finished frame into a write-combined CPU
// mapping of it, and a timer at the panel's refresh (measured on the display engine, measureRefresh) stands in for the VBL
// interrupt WindowServer paces an accelerated display on.
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IODisplay.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <pexpert/pexpert.h>
#include <kern/thread_call.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IORangeAllocator.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <libkern/OSAtomic.h>
#include <sys/sysctl.h>
#include "phxbl.h"
#include "nvrm_vram_abi.h"
#include "nvrm_gpuva_abi.h"

void phxAgdcInstall();   // phxagdc.cpp: the AppleGraphicsDeviceControl that reports this panel (debug.phxagdc)
void phxAgdcRemove();
void phxHoldInit(IOService *fb, unsigned bootHoldSec);   // phxagdc.cpp: debug.phxfb_hold keeps this framebuffer (and so the GPU) busy

#define PFBLOG(fmt, ...) IOLog("PhxFB: " fmt "\n", ##__VA_ARGS__)
// experiment switches without a rebuild: `sudo nvram phxfb-x=<n>` (read at start, kept). bit 0: panel reported as external.
static uint32_t gPhxX = 0;
static volatile SInt32 gTrace = 0;
// the per-call trace is a debugging aid: off unless NVRAM phxfb-x has bit 1 set (nvram phxfb-x=2), it costs kernel log
// traffic on every WindowServer poll
#define PTR(fmt, ...) do { if ((gPhxX & 2) && OSIncrementAtomic(&gTrace) < 600) IOLog("PhxFB: [t] " fmt "\n", ##__VA_ARGS__); } while (0)

static int gAlias = 1;
static bool gAliasSysctl;
SYSCTL_INT(_debug, OID_AUTO, phxfb_alias, CTLFLAG_RW | CTLFLAG_LOCKED, &gAlias, 0,
           "1 = frames are copied into the scan-out through its system-RAM address (the carve-out), 0 = through the GPU's BAR");

// The VBL tick. XNU's timer coalescing moves a kernel deadline by up to an eighth of its delay, at most 1 ms, so a
// 120 Hz tick could land anywhere in the first millisecond after the vblank it stands for. The system-critical urgency
// that is never moved is not exported to kexts (thread_call_enter_delayed_with_leeway), so the tick is armed 1.5 ms
// early and re-armed for its deadline from there, each stage with an eighth of the previous slop, until less than
// 50 us remain (debug.phxfb_vbl_staged, 0 = one plain deadline). The lateness of each tick is summed for the
// measurements, and nvrm-accel.cpp times its presents against the last tick (gPhxVblLastAbs, gPhxFrameAbs).
static int gVblStaged = 1;
static SInt64 gVblN, gVblLateUsSum, gVblLateUsMax;
uint64_t gPhxVblLastAbs, gPhxFrameAbs;
volatile uint64_t gPhxVblSeq;   // ticks so far, counted before the handlers run: the display pipe completes on the next one
SYSCTL_INT(_debug, OID_AUTO, phxfb_vbl_staged, CTLFLAG_RW | CTLFLAG_LOCKED, &gVblStaged, 0,
           "1 = the VBL tick is armed early and re-armed until its deadline (little coalescing slop), 0 = one plain deadline");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_vbl_n, CTLFLAG_RD | CTLFLAG_LOCKED, &gVblN, "VBL ticks fired");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_vbl_late_us_sum, CTLFLAG_RW | CTLFLAG_LOCKED, &gVblLateUsSum, "microseconds the VBL ticks fired past their deadlines, summed");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_vbl_late_us_max, CTLFLAG_RW | CTLFLAG_LOCKED, &gVblLateUsMax, "the latest VBL tick, microseconds past its deadline (writable: 0 restarts it)");
static struct sysctl_oid *const kFbOids[] = { &sysctl__debug_phxfb_alias, &sysctl__debug_phxfb_vbl_staged, &sysctl__debug_phxfb_vbl_n,
                                              &sysctl__debug_phxfb_vbl_late_us_sum, &sysctl__debug_phxfb_vbl_late_us_max };

// The panel's backlight is the display block's PWM (DCN 3.1.4 PWRSEQ0, segment 2 base 0x34c0): ACPI's _BCM only asks the
// GPU driver through ATIF (AFN7), and the firmware leaves the PWM on at full duty (period 4000, 12 bits). It is set as
// Linux's panel_cntl does without DMUB: lock the group, write the active count, unlock, wait for the pending update.
// tools/phxctl/phxbl does the same from user space. -phxbloff leaves the panel without a brightness control.
enum : uint32_t { kBlCntl = 0x34c0 + 0x2f19, kBlPeriod = 0x34c0 + 0x2f1b, kBlLock = 0x34c0 + 0x2f1c };
// The firmware runs both PWM generators, PWRSEQ0 and PWRSEQ1 (+0x6c), at full duty, and PWRSEQ0 alone left the panel's
// brightness as it was (2026-10-25: eDP DPCD 0x721 says the panel follows its PWM pin): both get the level.
static constexpr uint32_t kBlSeqs[] = { 0, 0x2f85 - 0x2f19 };
static constexpr uint32_t kBlMax = 1024;   // the brightness parameter's range
class PhxAMDFB;
static IOService *phxBacklightCreate(PhxAMDFB *fb);

// The class name carries "AMD": CoreDisplay tells the GPU vendor from the framebuffer's class name (Intel, AMD, NVDA,
// AppleParavirt), and for any other name it turns IOPresentment off, which left the panel on the software compositor
// with the Metal compositor up for the accelerator (GetGPUVendorForFramebufferService -> 1, 2026-10-20).
static bool takeOneShot(const char *name);   // below: NVRAM one-shot switches
static bool nvramHas(const char *name);

static void seenAttr(const char *kind, uint32_t idx, uint32_t attr, uintptr_t value);
class PhxAMDFB : public IOFramebuffer {
    OSDeclareDefaultStructors(PhxAMDFB)
    IOPCIDevice *fPCI = nullptr;
    IOMemoryMap *fSurfMap = nullptr;   // write-combined CPU view of the scan-out surface through the BAR
    // The same surface through its system-RAM address: the 780M's VRAM is a carve-out of system memory, and amdgpu maps
    // it there on APUs rather than through the BAR. Through the BAR a 2560x1600 frame took ~110 ms to copy (~7 fps, the
    // whole desktop lagged, 2026-10-20). This view comes out write-back although it asks for write-combined (frames
    // copied with plain stores stayed in the CPU cache: stripes), so the display pipe writes it with non-temporal stores.
    IOMemoryMap *fAliasMap = nullptr;
    IOMemoryMap *fMmio = nullptr;      // BAR5, for the backlight PWM
    IOService *fBacklight = nullptr;   // the brightness parameter handler, named "backlight"
    unsigned fAliasTries = 0;
    volatile UInt32 fAliasBusy = 0;
    struct Irq { IOFBInterruptProc proc = nullptr; OSObject *target = nullptr; void *ref = nullptr; };
    Irq fVbl[4], fConnect;   // IOFramebuffer itself and, when it asks, IOAccelDisplayPipe each register a VBL handler
    thread_call_t fVblTimer = nullptr;
    thread_call_t fConnCall = nullptr;   // tells IOFramebuffer the connection changed, so it probes it (EDID, display connect)
    volatile int fConnShots = 0;
    uint64_t fVblNext = 0;
    volatile bool fStopping = false;
    bool fBeside = false;   // another framebuffer (the firmware's IONDRV one) already drives this GPU: not the console
    IOPhysicalAddress fBase = 0;
    uint32_t fWidth = 0, fHeight = 0, fRowBytes = 0;
    static constexpr uint64_t kFrameNs = 16666667ull;   // 60 Hz, when the refresh cannot be measured
    uint64_t fFrameNs = kFrameNs;
    uint32_t fRefreshMilliHz = 60000, fHTotal = 0, fVTotal = 0;
    static constexpr unsigned kBootHoldSec = 120;   // the boot daemon releases it sooner (debug.phxfb_hold=0)
    // The accelerator is NullMoth's, written against an NVIDIA RM: it backs each IOAccel surface with "VRAM" from the
    // framebuffer (nvAllocVram) and a GPU address from the RM (nvGpuVaAlloc/Map). On the 780M nothing on the GPU reads
    // those surfaces -- the display pipe copies finished frames by CPU, and RADV keeps its own GPU addresses in
    // PhxBringup -- so VRAM here is physically contiguous system memory (the 780M has no other kind) and a GPU address
    // is a number from a private range that never enters a page table. Without them every surface_flush WindowServer
    // makes failed ("failed to alloc GART space"), 30 times a second, and no frame reached the panel (2026-10-20).
    static constexpr uint64_t kVaBase = 0x100000000000ull, kVaSize = 1ull << 40, kVramCap = 1024ull << 20;
    IORangeAllocator *fVa = nullptr;
    volatile SInt64 fVramBytes = 0;
    volatile SInt32 fHandles = 0;
    unsigned fRmLogged = 0;
    // VRAM comes from one physically contiguous pool taken at start, while memory is still unfragmented: allocated one
    // by one, a 16 MB surface found no contiguous run hours into a session, and when WindowServer re-allocated its frame
    // on display wake the panel kept the frame from before the display slept (2026-10-21). An allocation is a
    // sub-descriptor of the pool (its handle); past the pool each one is contiguous memory of its own, as before.
    static constexpr uint64_t kPoolTry[3] = { 512ull << 20, 256ull << 20, 128ull << 20 };
    IOBufferMemoryDescriptor *fPool = nullptr;
    IORangeAllocator *fPoolRange = nullptr;
    uint8_t *fPoolKva = nullptr;
    uint64_t fPoolPhys = 0, fPoolBytes = 0;

    void poolInit() {
        for (uint64_t bytes : kPoolTry) {
            IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                kIODirectionInOut | kIOMemoryPhysicallyContiguous, bytes, 0x0000FFFFFFFFF000ull);
            if (b && b->prepare() != kIOReturnSuccess) OSSafeReleaseNULL(b);
            if (!b) continue;
            fPoolRange = IORangeAllocator::withRange(bytes, PAGE_SIZE, 0, IORangeAllocator::kLocking);
            if (!fPoolRange) { b->complete(); b->release(); return; }
            fPool = b; fPoolKva = (uint8_t *)b->getBytesNoCopy(); fPoolBytes = bytes;
            fPoolPhys = b->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
            PFBLOG("VRAM pool: %llu MB at phys 0x%llx", bytes >> 20, fPoolPhys);
            return;
        }
        PFBLOG("VRAM pool: no contiguous run of %llu MB or less, every allocation on its own", kPoolTry[2] >> 20);
    }
    void poolFree() {
        OSSafeReleaseNULL(fPoolRange);
        if (fPool) { fPool->complete(); OSSafeReleaseNULL(fPool); }
        fPoolKva = nullptr; fPoolPhys = fPoolBytes = 0;
    }
    bool poolAlloc(NVRMVramRequest *r, uint64_t bytes) {
        IORangeScalar off = 0;
        if (!fPoolRange || !fPoolRange->allocate(bytes, &off, PAGE_SIZE)) return false;
        IOSubMemoryDescriptor *s = IOSubMemoryDescriptor::withSubRange(fPool, off, bytes, kIODirectionInOut);
        if (!s) { fPoolRange->deallocate(off, bytes); return false; }
        bzero(fPoolKva + off, bytes);
        r->handle = s; r->kva = fPoolKva + off; r->actualSize = bytes; r->allocationCookie = 0;
        r->phys = fPoolPhys + off;
        return true;
    }

    IOReturn vramAlloc(NVRMVramRequest *r) {
        if (!r || r->version != NVRM_VRAM_ABI_VERSION || !r->size) return kIOReturnBadArgument;
        const uint64_t bytes = round_page_64(r->size);
        if ((uint64_t)OSAddAtomic64((SInt64)bytes, &fVramBytes) + bytes > kVramCap) {
            OSAddAtomic64(-(SInt64)bytes, &fVramBytes);
            if (fRmLogged++ < 16) PFBLOG("VRAM: %llu bytes refused, %lld already out (cap %llu)", bytes, fVramBytes, kVramCap);
            return kIOReturnNoMemory;
        }
        if (poolAlloc(r, bytes)) {
            r->mappedTotal = (uint64_t)fVramBytes;
            if (fRmLogged++ < 16) PFBLOG("VRAM: %llu bytes at phys 0x%llx from the pool (%lld out)", bytes, r->phys, fVramBytes);
            return kIOReturnSuccess;
        }
        IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous, bytes, 0x0000FFFFFFFFF000ull);
        if (b && b->prepare() != kIOReturnSuccess) OSSafeReleaseNULL(b);
        if (!b) {
            OSAddAtomic64(-(SInt64)bytes, &fVramBytes);
            if (fRmLogged++ < 16) PFBLOG("VRAM: no contiguous %llu bytes", bytes);
            return kIOReturnNoMemory;
        }
        bzero(b->getBytesNoCopy(), bytes);
        r->handle = b; r->kva = b->getBytesNoCopy(); r->actualSize = bytes; r->allocationCookie = 0;
        r->phys = b->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
        r->mappedTotal = (uint64_t)fVramBytes;
        if (fRmLogged++ < 16) PFBLOG("VRAM: %llu bytes at phys 0x%llx (%lld out)", bytes, r->phys, fVramBytes);
        return kIOReturnSuccess;
    }
    IOReturn vramFree(NVRMVramRequest *r) {
        if (!r || r->version != NVRM_VRAM_ABI_VERSION) return kIOReturnBadArgument;
        if (IOSubMemoryDescriptor *s = OSDynamicCast(IOSubMemoryDescriptor, (OSObject *)r->handle)) {
            const uint64_t off = (uint64_t)((uint8_t *)r->kva - fPoolKva), len = s->getLength();
            if (!fPoolRange || (uint8_t *)r->kva < fPoolKva || off + len > fPoolBytes) return kIOReturnBadArgument;
            OSAddAtomic64(-(SInt64)len, &fVramBytes);
            fPoolRange->deallocate(off, len);
            s->release();
            return kIOReturnSuccess;
        }
        IOBufferMemoryDescriptor *b = OSDynamicCast(IOBufferMemoryDescriptor, (OSObject *)r->handle);
        if (!b) return kIOReturnBadArgument;
        OSAddAtomic64(-(SInt64)b->getLength(), &fVramBytes);
        b->complete(); b->release();
        return kIOReturnSuccess;
    }
    IOReturn rmCall(const OSSymbol *fn, void *p1) {
        if (fn->isEqualTo(NVRM_GPUVA_FN_HANDLES)) {
            NVRMRmHandles *h = (NVRMRmHandles *)p1;
            if (!h || h->version != NVRM_GPUVA_ABI_VERSION) return kIOReturnBadArgument;
            h->kapiDevice = this; h->hClient = 0x50480001; h->hDevice = 0x50480002; h->hSubDevice = 0x50480003;
            return kIOReturnSuccess;
        }
        NVRMGpuVaRequest *g = (NVRMGpuVaRequest *)p1;
        if (!g || g->version != NVRM_GPUVA_ABI_VERSION) return kIOReturnBadArgument;
        if (fn->isEqualTo(NVRM_GPUVA_FN_ALLOC)) {
            const uint64_t len = round_page_64(g->size);
            uint64_t align = g->align > PAGE_SIZE ? g->align : PAGE_SIZE;
            if (align & (align - 1)) align = PAGE_SIZE;
            IORangeScalar off = 0;
            if (!fVa || !len || !fVa->allocate(len, &off, align)) { g->status = 1; return kIOReturnNoSpace; }
            g->gpuva = kVaBase + off; g->hVirt = (unsigned)OSIncrementAtomic(&fHandles) + 1; g->status = 0;
            return kIOReturnSuccess;
        }
        if (fn->isEqualTo(NVRM_GPUVA_FN_MAP)) {   // nothing to map: no GPU work reaches these surfaces
            g->hMemory = (unsigned)OSIncrementAtomic(&fHandles) + 1; g->status = 0;
            return kIOReturnSuccess;
        }
        if (fn->isEqualTo(NVRM_GPUVA_FN_FREE)) {
            if (!(g->flags & NVRM_GPUVA_FLAG_UNMAP_ONLY) && fVa && g->gpuva >= kVaBase && g->gpuva - kVaBase < kVaSize)
                fVa->deallocate(g->gpuva - kVaBase, round_page_64(g->size));
            g->status = 0;
            return kIOReturnSuccess;
        }
        return kIOReturnUnsupported;
    }

    static void connFire(thread_call_param_t p0, thread_call_param_t) {
        PhxAMDFB *me = (PhxAMDFB *)p0;
        if (me->fStopping) return;
        const IOFBInterruptProc proc = me->fConnect.proc;
        if (proc) { PFBLOG("connect interrupt -> %s", me->fConnect.target ? me->fConnect.target->getMetaClass()->getClassName() : "?"); proc(me->fConnect.target, me->fConnect.ref); }
        if (--me->fConnShots > 0 && me->fConnCall) { uint64_t dl; clock_interval_to_deadline(4000, kMillisecondScale, &dl); thread_call_enter_delayed(me->fConnCall, dl); }
    }
    static void vblFire(thread_call_param_t p0, thread_call_param_t) {
        PhxAMDFB *me = (PhxAMDFB *)p0;
        if (me->fStopping) return;
        const uint64_t at = mach_absolute_time(), due = me->fVblNext;
        uint64_t near; nanoseconds_to_absolutetime(50000, &near);
        if (gVblStaged && due > at + near) { thread_call_enter_delayed(me->fVblTimer, due); return; }   // an early stage
        if (due && at > due) {
            uint64_t ns; absolutetime_to_nanoseconds(at - due, &ns);
            const SInt64 us = (SInt64)(ns / 1000);
            OSAddAtomic64(us, &gVblLateUsSum);
            if (us > gVblLateUsMax) gVblLateUsMax = us;
        }
        OSAddAtomic64(1, &gVblN);
        gPhxVblLastAbs = due ? due : at;   // the vblank this tick stands for
        __atomic_add_fetch(&gPhxVblSeq, 1, __ATOMIC_RELEASE);
        for (const Irq &q : me->fVbl) {
            const IOFBInterruptProc proc = __atomic_load_n(&q.proc, __ATOMIC_ACQUIRE);
            if (proc) proc(q.target, q.ref);
        }
        uint64_t period; nanoseconds_to_absolutetime(me->fFrameNs, &period);
        const uint64_t now = mach_absolute_time();
        me->fVblNext = (me->fVblNext && me->fVblNext + period > now) ? me->fVblNext + period : now + period;
        uint64_t lead; nanoseconds_to_absolutetime(1500000, &lead);
        thread_call_enter_delayed(me->fVblTimer, gVblStaged ? me->fVblNext - lead : me->fVblNext);
    }

    // The firmware lit the panel at its native refresh, which the VBL timer and the mode must report: they said 60 Hz while
    // the panel scanned at 120 (OTG frame counter: 392 frames in 3.28 s), and WindowServer composited at 60 fps (2026-10-20).
    // Read-only: OTG0's H/V totals (DCN 3.1.4: segment 2 base 0x34c0 + 0x1b2a / 0x1b2f) and its frame counter (+0x1b4c)
    // over ~200 ms, snapped to a standard rate when within 2 %.
    //
    // 2026-10-25, 83HW (Lenovo 16" 2880x1800 OLED, EDID LEN8BAD): the firmware lights it at 60 Hz with the line rate of
    // its 120 Hz mode (3080 total x 3840 total at ~710 MHz; EDID range limits 30-120 Hz, 230 kHz line rate): the panel
    // stretches the vertical front porch to lower the refresh. 120 Hz is then the same pixel clock and the same lines
    // with V_TOTAL halved (3840 -> 1920). `nvram phxfb-120=1` (one boot per arming) does that, only when the OTG's
    // blanking registers agree with that reading, and keeps it only if the frame counter then shows ~120 Hz.
    static constexpr uint32_t kOtg = 0x34c0;
    uint32_t sampleRefresh(volatile uint32_t *r, uint32_t *frames, uint64_t *ns) {
        const uint32_t c0 = r[kOtg + 0x1b4c] & 0xffffff; const uint64_t t0 = mach_absolute_time();
        IOSleep(200);
        const uint32_t c1 = r[kOtg + 0x1b4c] & 0xffffff; const uint64_t t1 = mach_absolute_time();
        absolutetime_to_nanoseconds(t1 - t0, ns);
        *frames = (c1 - c0) & 0xffffff;
        return *ns ? (uint32_t)(((uint64_t)*frames * 1000000000000ull) / *ns) : 0;
    }
    void dumpOtg(volatile uint32_t *r, const char *why) {
        for (uint32_t i = 0x1b2a; i < 0x1b92; i += 6)
            PFBLOG("OTG0 %s [%#x..]: %08x %08x %08x %08x %08x %08x", why, i, r[kOtg + i], r[kOtg + i + 1], r[kOtg + i + 2],
                   r[kOtg + i + 3], r[kOtg + i + 4], r[kOtg + i + 5]);
    }
    void tryHighRefresh(volatile uint32_t *r, uint32_t mhz) {
        const uint32_t ht = r[kOtg + 0x1b2a] + 1, vt = r[kOtg + 0x1b2f] + 1;
        // measured on 83HW: V_BLANK_START_END (0x1b36) = 0x00280730: blank starts at line 1840 (BLANK_START, bits 14:0) and ends
        // at line 40 (bits 30:16), so the active lines are [40, 1840) = 1800, V_SYNC_A (0x1b37) 0..16, V_TOTAL 3840: the
        // front porch (1840..3840) is what the firmware stretched for 60 Hz. Halving V_TOTAL only trims that porch.
        const uint32_t vbse = r[kOtg + 0x1b36], bs = vbse & 0x7fff, be = (vbse >> 16) & 0x7fff;
        if (mhz < 55000 || mhz > 65000) { PFBLOG("120 Hz: panel is at %u mHz, not the 60 Hz this expects: leaving it", mhz); return; }
        const uint32_t nvt = (vt + 1) / 2;   // 3840 -> 1920 (exactly half: 120.06 Hz at this line rate; from the measured 59.7 it came out 1911, 119.6 Hz)
        if (ht < 2000 || ht > 4500 || vt < 3000 || vt > 4500) { PFBLOG("120 Hz: totals %ux%u are not what this expects: leaving it", ht, vt); return; }
        if (bs <= be || bs - be != fHeight || bs + 8 > nvt) {
            PFBLOG("120 Hz: blank start/end %u/%u do not fit %u active lines in %u total: leaving it", bs, be, (unsigned)fHeight, nvt); return;
        }
        const uint32_t oldv = r[kOtg + 0x1b2f];
        // write just after a frame boundary, so the new total lands in the blanking, not in the active lines
        const uint32_t f0 = r[kOtg + 0x1b4c] & 0xffffff;
        for (int k = 0; k < 4000000 && (r[kOtg + 0x1b4c] & 0xffffff) == f0; k++) { }
        r[kOtg + 0x1b2f] = nvt - 1;
        IOSleep(150);
        uint32_t frames; uint64_t ns; const uint32_t m2 = sampleRefresh(r, &frames, &ns);
        PFBLOG("120 Hz: V_TOTAL %u -> %u, now %u.%03u Hz (%u frames in %llu us), V_TOTAL reads back %u", vt, nvt, m2 / 1000, m2 % 1000,
               frames, ns / 1000, r[kOtg + 0x1b2f] + 1);
        if (m2 < 110000 || m2 > 130000) {
            r[kOtg + 0x1b2f] = oldv; IOSleep(100);
            PFBLOG("120 Hz: no effect or out of range: V_TOTAL put back to %u", r[kOtg + 0x1b2f] + 1);
        }
        dumpOtg(r, "after");
    }
    void measureRefresh() {
        IOMemoryMap *mm = fPCI ? fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5) : nullptr;
        if (!mm || mm->getLength() < 0x5100u * 4) { OSSafeReleaseNULL(mm); PFBLOG("refresh: no MMIO view, 60 Hz assumed"); return; }
        volatile uint32_t *r = (volatile uint32_t *)mm->getVirtualAddress();
        dumpOtg(r, "boot");
        uint32_t frames; uint64_t ns;
        uint32_t mhz = sampleRefresh(r, &frames, &ns);
        // on by default since 2026-10-25 (83HW: 119.6 Hz measured, looks smoother). Off with the boot-arg -phxfb60 or
        // `sudo nvram phxfb-60=1`; the old one-shot `phxfb-120` is still consumed so a leftover does not linger.
        char a60[8]; takeOneShot("phxfb-120");
        const bool want120 = !PE_parse_boot_argn("-phxfb60", a60, sizeof a60) && !nvramHas("phxfb-60");
        if (!want120) PFBLOG("120 Hz: disabled (-phxfb60 / NVRAM phxfb-60): staying on the firmware's refresh");
        if (want120 && (r[kOtg + 0x1b41] & 1) && frames >= 4) {
            tryHighRefresh(r, mhz);
            mhz = sampleRefresh(r, &frames, &ns);
        }
        const uint32_t ctl = r[kOtg + 0x1b41], ht = r[kOtg + 0x1b2a] + 1, vt = r[kOtg + 0x1b2f] + 1;
        mm->release();
        if (!(ctl & 1) || !ns || frames < 4 || ht < 100 || vt < 100) { PFBLOG("refresh: OTG0 not running (ctl 0x%x, %u frames), 60 Hz assumed", ctl, frames); return; }
        static const uint32_t kStd[] = { 48000, 50000, 60000, 75000, 90000, 100000, 120000, 144000, 165000, 240000 };
        for (uint32_t k : kStd) if (mhz > k * 49 / 50 && mhz < k * 51 / 50) { mhz = k; break; }
        if (mhz < 24000 || mhz > 360000) { PFBLOG("refresh: %u mHz measured, out of range - 60 Hz assumed", mhz); return; }
        fRefreshMilliHz = mhz; fHTotal = ht; fVTotal = vt;
        fFrameNs = 1000000000000ull / mhz;
        PFBLOG("refresh: OTG0 %ux%u totals, %u frames in %llu us -> %u.%03u Hz", ht, vt, frames, ns / 1000, mhz / 1000, mhz % 1000);
    }

    // backlight (phxbl.h): probed at start, driven by PhxBacklight (an IODisplayParameterHandler under this framebuffer)
    IOMemoryMap *fBlMap = nullptr;
    phxbl::Panel fBl;
    void blInit() {
        fBlMap = fPCI ? fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5) : nullptr;
        if (!fBlMap || fBlMap->getLength() < 0x19200u) { BLLOG("no usable MMIO view: no backlight"); OSSafeReleaseNULL(fBlMap); return; }
        fBl.probe((volatile uint32_t *)fBlMap->getVirtualAddress(), (uint32_t)fBlMap->getLength());
    }

public:
    bool blSet(uint32_t level) { return fBlMap ? fBl.set(level) : false; }
    void blPower(bool on) { if (fBlMap && !(gPhxX & 16)) fBl.blank(!on); }   // phxfb-x bit 16: leave the backlight alone
    uint32_t blLevel() const { return fBl.level; }
    static constexpr uint32_t kBlLevels = phxbl::Panel::kLevels;
    // the backlight as 0..kBlMax of full; phxbl.h drives the panel (PWM generators, or the OLED's AUX source-backlight)
    bool backlightRead(uint32_t *level) {
        if (!fBlMap || !level || fBl.mode == phxbl::Panel::None) return false;
        *level = (uint32_t)((uint64_t)fBl.level * kBlMax / kBlLevels);
        return true;
    }
    bool backlightWrite(uint32_t level) {
        if (!fBlMap || fBl.mode == phxbl::Panel::None) return false;
        if (level > kBlMax) level = kBlMax;
        return fBl.set((uint32_t)((uint64_t)level * kBlLevels / kBlMax));
    }
    // Display sleep left the backlight lit over a black or torn frame (2026-10-25). Besides brightness 0, a connection
    // powered down goes dark and gets its level back on power-up; a brightness set while dark waits.
    bool fBlDark = false;
    uint32_t fBlSaved = kBlMax;
    bool backlightSet(uint32_t level) {
        if (fBlDark) { fBlSaved = level > kBlMax ? kBlMax : level; return true; }
        return backlightWrite(level);
    }
    void displayPower(bool on) {
        if (!fBacklight || on != fBlDark) return;
        if (!on && !(backlightRead(&fBlSaved) && backlightWrite(0))) return;
        fBlDark = !on;
        if (on) backlightWrite(fBlSaved);
        fBacklight->setProperty("PhxDark", fBlDark);
        PFBLOG("backlight: display power %s, level %u", on ? "on" : "off", fBlSaved);
    }
    bool backlightDark() const { return fBlDark; }
    // which calls reach the panel, newest first, as the registry property PhxTrace (the brightness slider moved and no
    // brightness parameter arrived, 2026-10-25); the log does not reach the unified log
    char fTrace[16][48] = {}; unsigned fTraceN = 0;
    void trace(const char *what, uint32_t a, uint64_t v) {
        char line[48]; snprintf(line, sizeof line, "%s %c%c%c%c=0x%llx", what, (char)(a >> 24), (char)(a >> 16), (char)(a >> 8), (char)a,
                                (unsigned long long)v);
        if (fTraceN && !strcmp(line, fTrace[(fTraceN - 1) % 16])) return;   // repeats of the last call
        strlcpy(fTrace[fTraceN++ % 16], line, sizeof fTrace[0]);
        char all[16 * 48 + 16] = ""; for (unsigned i = 0; i < 16 && i < fTraceN; i++) { strlcat(all, fTrace[(fTraceN - 1 - i) % 16], sizeof all); strlcat(all, "; ", sizeof all); }
        setProperty("PhxTrace", all);
    }
    // the built-in panel is a TFT: AppleBacklightDisplay then drives it (brightness, dimming), as on a MacBook
    IOReturn getAppleSense(IOIndex idx, UInt32 *senseType, UInt32 *primary, UInt32 *extended, UInt32 *displayType) override {
        if (idx != 0 || !fBacklight) return kIOReturnUnsupported;
        if (senseType) *senseType = 0;
        if (primary) *primary = 0;
        if (extended) *extended = 0;
        if (displayType) *displayType = 2;   // kPanelTFTConnect (IOMacOSVideo.h, the NDRV headers)
        return kIOReturnSuccess;
    }
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn enableController() override { return kIOReturnSuccess; }
    bool isConsoleDevice() override { return !fBeside; }
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override {
        if (aperture != kIOFBSystemAperture) return nullptr;
        return IODeviceMemory::withRange(fBase, (IOPhysicalLength)fRowBytes * fHeight);
    }
    IODeviceMemory *getVRAMRange() override { return getApertureRange(kIOFBSystemAperture); }
    const char *getPixelFormats() override { return IO32BitDirectPixels "\0"; }
    IOItemCount getDisplayModeCount() override { PTR("getDisplayModeCount"); return 1; }
    IOReturn getDisplayModes(IODisplayModeID *modes) override { PTR("getDisplayModes"); modes[0] = 1; return kIOReturnSuccess; }
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override {
        // WindowServer failed "obtain mode info" with the single id 1 on 26.7: answer for any id and log which it asked for
        static int n; if (n++ < 20) PFBLOG("getInformationForDisplayMode(%#x)", (unsigned)mode);
        if (!info) return kIOReturnBadArgument;
        bzero(info, sizeof(*info));
        info->maxDepthIndex = 0; info->nominalWidth = fWidth; info->nominalHeight = fHeight;
        info->refreshRate = (IOFixed1616)(((uint64_t)fRefreshMilliHz << 16) / 1000); info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag | kFramebufferSupportsGammaCorrection;
        // 2026-10-25: the mode the panel runs is its native, built-in, accelerated one and may be used for HiDPI; a framebuffer
        // that does not say so gets no scaled ("looks like") modes from CoreDisplay however the display override lists them.
        // `nvram phxfb-x=4` brings the old flags back.
        if (!(gPhxX & 4)) info->flags |= kDisplayModeNativeFlag | kDisplayModeBuiltInFlag | kDisplayModeValidForHiResFlag |
                                         kDisplayModeAcceleratorBackedFlag | kDisplayModeValidForMirroringFlag;
        return kIOReturnSuccess;
    }
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi) override {
        static int n; if (n++ < 20) PFBLOG("getPixelInformation(mode %#x, depth %d, aperture %d)", (unsigned)mode, (int)depth, (int)aperture);
        if (aperture != kIOFBSystemAperture || !pi) return kIOReturnUnsupportedMode;
        bzero(pi, sizeof(*pi));
        pi->bytesPerRow = fRowBytes; pi->bitsPerPixel = 32; pi->pixelType = kIORGBDirectPixels;
        pi->componentCount = 3; pi->bitsPerComponent = 8;
        pi->componentMasks[0] = 0x00FF0000; pi->componentMasks[1] = 0x0000FF00; pi->componentMasks[2] = 0x000000FF;
        strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
        pi->activeWidth = fWidth; pi->activeHeight = fHeight;
        return kIOReturnSuccess;
    }
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { PTR("getCurrentDisplayMode"); *mode = 1; *depth = 0; return kIOReturnSuccess; }
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { return getCurrentDisplayMode(mode, depth); }
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override { PFBLOG("setDisplayMode(%#x, %d)", (unsigned)mode, (int)depth); return kIOReturnSuccess; }
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override {
        static int n; if (n++ < 20) PFBLOG("getTimingInfoForDisplayMode(%#x)", (unsigned)mode);
        if (!info) return kIOReturnUnsupportedMode;
        bzero(info, sizeof(*info));
        info->appleTimingID = kIOTimingIDInvalid;   // no detailed timing: the panel keeps the firmware's
        return kIOReturnSuccess;
    }
    // WindowServer ended with display id 0 on 26.7: CoreDisplay names a display from its EDID, and PhxFB published none (the
    // firmware lit the panel; there is no DDC on an eDP panel). One synthetic 128-byte EDID 1.4 block describes the panel
    // at the mode PhxFB reports (CVT reduced blanking), vendor LEN, name "Built-in".
    bool hasDDCConnect(IOIndex idx) override { return idx == 0; }
    IOReturn getDDCBlock(IOIndex idx, UInt32 bn, IOSelect type, IOOptionBits, UInt8 *data, IOByteCount *len) override {
        PFBLOG("getDDCBlock(conn %d, block %u, type %u, len %llu)", (int)idx, (unsigned)bn, (unsigned)type, len ? (unsigned long long)*len : 0ull);
        if (idx != 0 || !data || !len || *len < 128) return kIOReturnBadArgument;
        if (bn != 1) return kIOReturnNoDevice;   // one block, no extensions
        uint8_t *e = data; bzero(e, 128);
        static const uint8_t hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
        memcpy(e, hdr, 8);
        e[8] = 0x30; e[9] = 0xAE;                 // manufacturer "LEN"
        e[10] = 0x01; e[11] = 0x00;               // product code
        e[12] = 1;                                // serial
        e[16] = 1; e[17] = 34;                    // week 1, year 2024
        e[18] = 1; e[19] = 4;                     // EDID 1.4
        e[20] = 0xA5;                             // digital, 8 bpc, DisplayPort
        e[21] = 34; e[22] = 22;                   // 34 x 22 cm (aspect 16:10)
        e[23] = 0x78; e[24] = 0x0A;               // gamma 2.2, RGB, preferred timing in first descriptor
        static const uint8_t chroma[10] = { 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54 };   // sRGB
        memcpy(e + 25, chroma, 10);
        for (int k = 0; k < 8; k++) { e[38 + 2 * k] = 0x01; e[39 + 2 * k] = 0x01; }   // standard timings: unused
        const uint32_t w = fWidth ? fWidth : 2880, h = fHeight ? fHeight : 1800;
        const uint32_t hb = 160, vb = 46, mhz = fRefreshMilliHz ? fRefreshMilliHz : 60000;
        const uint32_t pclk = (uint32_t)((uint64_t)(w + hb) * (h + vb) * mhz / 1000 / 10000);   // 10 kHz units
        uint8_t *d = e + 54;
        d[0] = pclk & 0xff; d[1] = (pclk >> 8) & 0xff;
        d[2] = w & 0xff; d[3] = hb & 0xff; d[4] = (uint8_t)(((w >> 8) << 4) | (hb >> 8));
        d[5] = h & 0xff; d[6] = vb & 0xff; d[7] = (uint8_t)(((h >> 8) << 4) | (vb >> 8));
        d[8] = 48; d[9] = 32; d[10] = (3 << 4) | 6; d[11] = 0;
        d[12] = 0x58; d[13] = 0xD7; d[14] = 0x10;   // 344 x 215 mm
        d[17] = 0x1A;
        uint8_t *n = e + 72; n[3] = 0xFC; memcpy(n + 5, "Built-in\n    ", 13);
        uint8_t *z = e + 90; z[3] = 0x10;
        uint8_t sum = 0; for (int k = 0; k < 127; k++) sum += e[k];
        e[127] = (uint8_t)(256 - sum);
        *len = 128;
        return kIOReturnSuccess;
    }
    IOItemCount getConnectionCount() override { return 1; }
    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref, void **iref) override {
        if (type == kIOFBVBLInterruptType) {
            for (Irq &q : fVbl) if (!q.proc) {
                q.target = target; q.ref = ref; __atomic_store_n(&q.proc, proc, __ATOMIC_RELEASE);
                PFBLOG("VBL handler %u: %s", (unsigned)(&q - fVbl), target ? target->getMetaClass()->getClassName() : "(no target)");
                if (iref) *iref = &q;
                return kIOReturnSuccess;
            }
            PFBLOG("VBL handler refused: all %u taken", (unsigned)(sizeof fVbl / sizeof fVbl[0]));
            return kIOReturnNoResources;
        }
        if (type == kIOFBConnectInterruptType) {
            fConnect = { proc, target, ref }; if (iref) *iref = &fConnect;
            PFBLOG("connect handler registered: %s", target ? target->getMetaClass()->getClassName() : "(none)");
            // The connection never "changed" for a framebuffer the firmware lit, so IOFramebuffer never probed the panel
            // (no getDDCBlock call, no display connect, display id 0 in WindowServer): raise it ourselves, twice.
            if (fConnCall) { fConnShots = 2; uint64_t dl; clock_interval_to_deadline(500, kMillisecondScale, &dl); thread_call_enter_delayed(fConnCall, dl); }
            return kIOReturnSuccess;
        }
        return kIOReturnUnsupported;
    }
    IOReturn unregisterInterrupt(void *iref) override {
        for (Irq &q : fVbl) if (iref == &q) __atomic_store_n(&q.proc, (IOFBInterruptProc)nullptr, __ATOMIC_RELEASE);
        return kIOReturnSuccess;
    }
    IOReturn setInterruptState(void *, UInt32) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *, bool) override { return kIOReturnSuccess; }
    IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
    // power and connection answers as NullMoth's NMIntelFB/NVRMFB give them: acknowledge at once, never queue on the
    // controller thread (a base-class power change waited 45 s for an acknowledge and froze WindowServer restarts)
    IOReturn setPowerState(unsigned long ordinal, IOService *) override {
        if (ordinal) blPower(true);
        handleEvent(ordinal ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff);
        if (!ordinal) blPower(false);
        return kIOPMAckImplied;
    }
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override {
        trace("attr", attribute, value);
        seenAttr("setAttribute", 0, attribute, value);
        PTR("setAttribute('%c%c%c%c', %#lx)", (char)(attribute >> 24), (char)(attribute >> 16), (char)(attribute >> 8), (char)attribute, (unsigned long)value);
        if (attribute == kIOWindowServerActiveAttribute) return kIOReturnSuccess;
        if (attribute == kIOPowerAttribute) {
            displayPower(value != 0);
            if (value) { blPower(true); IOReturn r = IOFramebuffer::setAttribute(attribute, value); handleEvent(kIOFBNotifyDidPowerOn);
                         return r == kIOReturnUnsupported ? kIOReturnSuccess : r; }
            handleEvent(kIOFBNotifyWillPowerOff);
            blPower(false);
            IOReturn r = IOFramebuffer::setAttribute(attribute, value);
            return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
        }
        return IOFramebuffer::setAttribute(attribute, value);
    }
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override {
        seenAttr("getAttribute", 0, attribute, 0);
        PTR("getAttribute('%c%c%c%c')", (char)(attribute >> 24), (char)(attribute >> 16), (char)(attribute >> 8), (char)attribute);
        if (attribute == kIOWindowServerActiveAttribute) { if (value) *value = 1; return kIOReturnSuccess; }
        return IOFramebuffer::getAttribute(attribute, value);
    }
    IOReturn setAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t value) override {
        trace("conn", attribute, value);
        seenAttr("setAttributeForConnection", (uint32_t)idx, attribute, value);
        PTR("setAttributeForConnection(%d, '%c%c%c%c', %#lx)", (int)idx, (char)(attribute >> 24), (char)(attribute >> 16), (char)(attribute >> 8), (char)attribute, (unsigned long)value);
        switch (attribute) {
        case kConnectionPower:
            displayPower(value != 0);
            return kIOReturnSuccess;
        case kConnectionColorModesSupported: case kConnectionColorDepthsSupported:
        case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported: case kConnectionColorMode:
        case kConnectionDisplayFlags: case kConnectionFlags:
            return kIOReturnSuccess;
        default: return IOFramebuffer::setAttributeForConnection(idx, attribute, value);
        }
    }
    IOReturn getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value) override {
        seenAttr("getAttributeForConnection", (uint32_t)idx, attr, 0);
        PTR("getAttributeForConnection(%d, '%c%c%c%c')", (int)idx, (char)(attr >> 24), (char)(attr >> 16), (char)(attr >> 8), (char)attr);
        if (idx != 0) return kIOReturnBadArgument;
        // AppleDisplay asks kConnectionSupportsHLDDCSense with a NULL value before it reads the EDID: answer it first
        if (attr == kConnectionSupportsHLDDCSense) return kIOReturnSuccess;
        if (!value) return kIOReturnBadArgument;
        switch (attr) {
        case kConnectionEnable: case kConnectionCheckEnable: *value = 1; return kIOReturnSuccess;
        case kConnectionFlags: *value = (gPhxX & 1) ? 0 : kIOConnectionBuiltIn; return kIOReturnSuccess;
        case kConnectionColorModesSupported: case kConnectionColorMode: *value = 0x00000001; return kIOReturnSuccess;
        case kConnectionColorDepthsSupported: case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported:
            *value = 0x00000002; return kIOReturnSuccess;
        default: return IOFramebuffer::getAttributeForConnection(idx, attr, value);
        }
    }
    IOReturn connectFlags(IOIndex c, IODisplayModeID m, IOOptionBits *flags) override {
        PTR("connectFlags(%d, %#x)", (int)c, (unsigned)m);
        if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
        return kIOReturnSuccess;
    }
    // The accelerator's display pipe asks for the scan-out: its CPU mapping (frames are copied in) and its physical
    // address in the GPU's VRAM aperture. With no address the pipe got no framebuffer resource, and IOAcceleratorFamily
    // marks a display pipe active only once one exists (init_framebuffer_resource sets +0x298): every WindowServer
    // transaction ended kIOReturnNotReady and no frame reached the panel (2026-10-20).
    // The scan-out through the carve-out, once PhxBringup has published where the carve-out sits (phx-fb-phys) and only
    // if that view holds the pixels the BAR shows. NULL (copy through the BAR) until then.
    IOMemoryMap *aliasMap() {
        if (fAliasMap || fAliasTries >= 3) return fAliasMap;
        if (!OSCompareAndSwap(0, 1, &fAliasBusy)) return nullptr;
        if (!fAliasMap && fAliasTries < 3) {
            fAliasTries++;
            uint64_t fbPhys = 0, barOff = ~0ull;
            if (OSIterator *it = fPCI->getChildIterator(gIOServicePlane)) {
                while (OSObject *o = it->getNextObject()) {
                    IORegistryEntry *e = OSDynamicCast(IORegistryEntry, o);
                    OSNumber *n = e ? OSDynamicCast(OSNumber, e->getProperty("phx-fb-phys")) : nullptr;
                    if (n) { fbPhys = n->unsigned64BitValue(); break; }
                }
                it->release();
            }
            for (uint32_t i = 0; i < fPCI->getDeviceMemoryCount(); i++) {
                IODeviceMemory *dm = fPCI->getDeviceMemoryWithIndex(i);
                if (dm && fBase >= dm->getPhysicalAddress() && fBase - dm->getPhysicalAddress() < dm->getLength()) {
                    barOff = fBase - dm->getPhysicalAddress();
                    break;
                }
            }
            const uint64_t phys = fbPhys + barOff, bytes = (uint64_t)fRowBytes * fHeight;
            IOMemoryMap *m = nullptr;
            if (fbPhys && barOff != ~0ull)
                if (IODeviceMemory *dm = IODeviceMemory::withRange(phys, bytes)) { m = dm->map(kIOMapWriteCombineCache); dm->release(); }
            if (m) {
                const volatile uint32_t *a = (const volatile uint32_t *)m->getVirtualAddress();
                const volatile uint32_t *b = (const volatile uint32_t *)fSurfMap->getVirtualAddress();
                const uint64_t words = bytes / 4, step = words / 4096 ? words / 4096 : 1;
                unsigned same = 0, nz = 0, n = 0;
                for (uint64_t w = 0; w < words; w += step, n++) { const uint32_t x = a[w], y = b[w]; same += x == y; nz += y != 0; }
                PFBLOG("alias: scan-out at BAR offset 0x%llx = system RAM 0x%llx: %u of %u samples match the BAR (%u non-zero)",
                       barOff, phys, same, n, nz);
                if (same == n && nz) fAliasMap = m; else m->release();
            } else {
                PFBLOG("alias: carve-out 0x%llx, scan-out BAR offset 0x%llx: no system-RAM view, copying through the BAR", fbPhys, barOff);
            }
        }
        fAliasBusy = 0;
        return fAliasMap;
    }
    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override {
        if (fn && fn->isEqualTo(NVRM_VRAM_FN_SCANOUT)) {
            NVRMVramRequest *r = (NVRMVramRequest *)p1;
            if (!r || r->version != NVRM_VRAM_ABI_VERSION) return kIOReturnBadArgument;
            if (!fSurfMap) return kIOReturnNotReady;
            IOMemoryMap *alias = gAlias ? aliasMap() : nullptr;
            r->kva = (void *)(alias ? alias : fSurfMap)->getVirtualAddress();
            r->phys = fBase; r->actualSize = round_page_64((uint64_t)fRowBytes * fHeight);
            r->width = fWidth; r->height = fHeight; r->pitch = fRowBytes;
            return kIOReturnSuccess;
        }
        if (fn && fn->isEqualTo(NVRM_VRAM_FN_ALLOC)) return vramAlloc((NVRMVramRequest *)p1);
        if (fn && fn->isEqualTo(NVRM_VRAM_FN_FREE)) return vramFree((NVRMVramRequest *)p1);
        if (fn && (fn->isEqualTo(NVRM_GPUVA_FN_HANDLES) || fn->isEqualTo(NVRM_GPUVA_FN_ALLOC) || fn->isEqualTo(NVRM_GPUVA_FN_MAP) ||
                   fn->isEqualTo(NVRM_GPUVA_FN_FREE)))
            return rmCall(fn, p1);
        return IOFramebuffer::callPlatformFunction(fn, wait, p1, p2, p3, p4);
    }
};

OSDefineMetaClassAndStructors(PhxAMDFB, IOFramebuffer)

// log each distinct framebuffer attribute selector the first time it is seen (backlight hunt, 2026-10-25): which selector does the
// brightness slider use, if not the IODisplay parameter handler
static void seenAttr(const char *kind, uint32_t idx, uint32_t attr, uintptr_t value)
{
    static uint32_t seen[96]; static int n = 0;
    for (int i = 0; i < n; i++) if (seen[i] == (attr ^ (idx << 28) ^ (kind[0] << 24))) return;
    if (n >= 96) return;
    seen[n++] = attr ^ (idx << 28) ^ (kind[0] << 24);
    auto pc = [](uint32_t c) { return (c >= 32 && c < 127) ? (char)c : '.'; };
    BLLOG("first %s(%u, '%c%c%c%c' %#x, %#lx)", kind, idx, pc(attr >> 24), pc((attr >> 16) & 255), pc((attr >> 8) & 255), pc(attr & 255), attr, (unsigned long)value);
}
// The panel's brightness as a display parameter. IOGraphics looks for parameter handlers under the framebuffer, and
// takes a built-in TFT panel for a backlit one (AppleBacklightDisplay) only while a service named "backlight" exists.
class PhxBacklight : public IODisplayParameterHandler {
    OSDeclareDefaultStructors(PhxBacklight)
    PhxAMDFB *fFB = nullptr;
    uint32_t fLevel = kBlMax;
public:
    static PhxBacklight *withFramebuffer(PhxAMDFB *fb) {
        PhxBacklight *b = new PhxBacklight;
        if (b && !b->init()) OSSafeReleaseNULL(b);
        if (b) { b->fFB = fb; fb->backlightRead(&b->fLevel); }
        return b;
    }
    bool setDisplay(IODisplay *display) override {
        if (!display) return true;
        OSObject *p = display->copyProperty(gIODisplayParametersKey);
        OSDictionary *cur = OSDynamicCast(OSDictionary, p);
        OSDictionary *params = cur ? OSDictionary::withDictionary(cur) : OSDictionary::withCapacity(2);
        OSSafeReleaseNULL(p);
        if (!params) return false;
        fFB->backlightRead(&fLevel);
        IODisplay::addParameter(params, gIODisplayBrightnessKey, 0, kBlMax);
        IODisplay::setParameter(params, gIODisplayBrightnessKey, (SInt32)fLevel);
        // CoreBrightness sets the linear ones, as an Intel MacBook's framebuffer offers them: with "brightness" alone
        // the slider moved and no parameter ever reached the panel (2026-10-25)
        const OSSymbol *linear[] = { gIODisplayLinearBrightnessKey, gIODisplayUsableLinearBrightnessKey };
        for (const OSSymbol *k : linear) { IODisplay::addParameter(params, k, 0, kBlMax); IODisplay::setParameter(params, k, (SInt32)fLevel); }
        display->setProperty(gIODisplayParametersKey, params);
        params->release();
        PFBLOG("backlight: brightness parameter on %s, %u of %u", display->getName(), fLevel, kBlMax);
        return true;
    }
    // the last parameter set, in the registry: PhxAMDFB's log does not reach the unified log
    uint32_t fSets = 0;
    bool doIntegerSet(OSDictionary *, const OSSymbol *paramName, UInt32 value) override {
        setProperty("PhxLastParam", const_cast<OSSymbol *>(paramName)); setProperty("PhxLastValue", value, 32); setProperty("PhxSets", ++fSets, 32);
        fFB->trace(paramName->getCStringNoCopy(), 0x20202020, value);
        setProperty("PhxDark", fFB->backlightDark());
        if (paramName != gIODisplayBrightnessKey && paramName != gIODisplayLinearBrightnessKey
            && paramName != gIODisplayUsableLinearBrightnessKey) return false;
        if (!fFB->backlightSet(value)) return false;
        fLevel = value > kBlMax ? kBlMax : value;
        return true;
    }
    bool doDataSet(const OSSymbol *, OSData *) override { return false; }
    IOReturn setProperties(OSObject *properties) override {
        OSDictionary *d = OSDynamicCast(OSDictionary, properties);
        if (OSCollectionIterator *it = d ? OSCollectionIterator::withCollection(d) : nullptr) {
            while (const OSSymbol *k = OSDynamicCast(OSSymbol, it->getNextObject())) fFB->trace(k->getCStringNoCopy(), 0x2a2a2a2a, 0);
            it->release();
        }
        return kIOReturnUnsupported;
    }
    bool doUpdate() override { return true; }
};
OSDefineMetaClassAndStructors(PhxBacklight, IODisplayParameterHandler)

static IOService *phxBacklightCreate(PhxAMDFB *fb)
{
    PhxBacklight *b = PhxBacklight::withFramebuffer(fb);
    if (!b) return nullptr;
    b->setName("backlight");
    if (!b->attach(fb)) { b->release(); return nullptr; }
    b->registerService();
    return b;
}

// One boot per arming: `nvram <name>=1` lets the next start use the feature, and start removes the variable and
// commits NVRAM at once, so a boot that panics (or shows a black panel) after this point comes back without hands.
static bool takeOneShot(const char *name)
{
    IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!opts) return false;
    OSObject *o = opts->copyProperty(name);
    const bool armed = o != nullptr;
    bool stuck = false;
    OSSafeReleaseNULL(o);
    if (armed) {
        opts->removeProperty(name);
        // the forced commit: the throttled one (IONVRAM-SYNCNOW) left the variable in flash once, and the boot after a
        // panic took the panel again (2026-10-20)
        if (OSString *why = OSString::withCString("phxfb")) { opts->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", why); why->release(); }
        o = opts->copyProperty(name);
        stuck = o != nullptr;
        PFBLOG("NVRAM %s taken (%s)", name, stuck ? "STILL SET: removal refused, ignoring it" : "removed and committed");
        OSSafeReleaseNULL(o);
    }
    opts->release();
    return armed && !stuck;
}
static bool nvramHas(const char *name)
{
    IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!opts) return false;
    OSObject *o = opts->copyProperty(name);
    const bool has = o != nullptr;
    OSSafeReleaseNULL(o); opts->release();
    return has;
}
static bool takeBootArming() { return takeOneShot("phxfb-boot"); }

static void readExperimentSwitch()
{
    IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!opts) return;
    OSObject *o = opts->copyProperty("phxfb-x");
    char buf[16] = {0};
    if (OSData *d = OSDynamicCast(OSData, o)) { unsigned n = d->getLength() < 15 ? d->getLength() : 15; memcpy(buf, d->getBytesNoCopy(), n); }
    else if (OSString *st = OSDynamicCast(OSString, o)) strlcpy(buf, st->getCStringNoCopy(), sizeof buf);
    else if (OSNumber *nn = OSDynamicCast(OSNumber, o)) snprintf(buf, sizeof buf, "%u", nn->unsigned32BitValue());
    OSSafeReleaseNULL(o); opts->release();
    uint32_t v = 0; for (const char *c = buf; *c >= '0' && *c <= '9'; c++) v = v * 10 + (uint32_t)(*c - '0');
    gPhxX = v; phxbl::gPhxBlNoOled = (v & 32) != 0; PFBLOG("experiment switch phxfb-x = %u", v);
}

bool PhxAMDFB::start(IOService *provider)
{
    char arg[8];
    if (PE_parse_boot_argn("-phxfboff", arg, sizeof arg)) { PFBLOG("-phxfboff: leaving the firmware framebuffer"); return false; }
    readExperimentSwitch();
    if (!takeBootArming()) { PFBLOG("NVRAM phxfb-boot not set: leaving the firmware framebuffer this boot"); return false; }
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI) return false;
    if (OSIterator *it = fPCI->getChildIterator(gIOServicePlane)) {
        while (OSObject *o = it->getNextObject())
            if (o != this && OSDynamicCast(IOFramebuffer, o)) fBeside = true;
        it->release();
    }
    PE_Video v;
    bzero(&v, sizeof(v));
    IOPlatformExpert *pe = getPlatform();
    if (!pe || pe->getConsoleInfo(&v) != kIOReturnSuccess || !v.v_baseAddr || v.v_depth != 32 || !v.v_width || !v.v_height ||
        v.v_rowBytes < v.v_width * 4 || v.v_width > 16384 || v.v_height > 16384) {
        PFBLOG("no usable boot console (base 0x%lx %lux%lu depth %lu row %lu): leaving the firmware framebuffer",
               v.v_baseAddr, v.v_width, v.v_height, v.v_depth, v.v_rowBytes);
        return false;
    }
    fWidth = (uint32_t)v.v_width; fHeight = (uint32_t)v.v_height; fRowBytes = (uint32_t)v.v_rowBytes;
    // once the video console is up, the platform expert hands out the console's kernel mapping, not the physical
    // address. The GOP surface sits in one of the GPU's BARs (VRAM aperture): find the BAR whose start holds the same
    // pixels as the console mapping. (A descriptor over the mapping would need prepare(): getPhysicalSegment on an
    // unwired one panicked, 2026-10-20.)
    uint64_t base = v.v_baseAddr;
    if (base >= 0xFFFF800000000000ull) {
        const volatile uint32_t *con = (const volatile uint32_t *)(uintptr_t)base;
        const uint32_t probeWords = 64u * 1024u / 4u;
        uint64_t found = 0;
        for (uint32_t i = 0; i < fPCI->getDeviceMemoryCount() && !found; i++) {
            IODeviceMemory *dm = fPCI->getDeviceMemoryWithIndex(i);
            if (!dm || dm->getLength() < (IOByteCount)fRowBytes * fHeight) continue;
            IODeviceMemory *sub = IODeviceMemory::withSubRange(dm, 0, 64u * 1024u);
            IOMemoryMap *m = sub ? sub->map(kIOMapInhibitCache) : nullptr;
            OSSafeReleaseNULL(sub);
            if (!m) continue;
            const volatile uint32_t *bar = (const volatile uint32_t *)m->getVirtualAddress();
            uint32_t same = 0, nz = 0;
            for (uint32_t w = 0; w < probeWords; w++) { const uint32_t a = con[w], b = bar[w]; same += a == b; nz += a != 0; }
            PFBLOG("BAR index %u at 0x%llx: %u of %u words match the console (%u non-zero)", i,
                   (unsigned long long)dm->getPhysicalAddress(), same, probeWords, nz);
            if (same == probeWords && nz) found = dm->getPhysicalAddress();
            m->release();
        }
        if (!found) { PFBLOG("the console surface is in none of the GPU's BARs: leaving the firmware framebuffer"); return false; }
        base = found;
    }
    // early in boot the platform expert hands out the physical address with a flag in its low bits (0x820000001): taken
    // as is, every row WindowServer drew landed one byte off and the panel showed swapped colours (2026-10-20)
    base &= ~(uint64_t)PAGE_MASK;
    fBase = (IOPhysicalAddress)base;
    PFBLOG("console: %ux%u row %u depth %lu at physical 0x%llx", fWidth, fHeight, fRowBytes, v.v_depth, (unsigned long long)fBase);
    if (IODeviceMemory *sm = IODeviceMemory::withRange(fBase, (IOPhysicalLength)fRowBytes * fHeight)) {
        fSurfMap = sm->map(kIOMapWriteCombineCache); sm->release();
    }
    if (!fSurfMap) { PFBLOG("the console surface at 0x%llx would not map write-combined", (unsigned long long)fBase); return false; }
    // IOFramebuffer::start makes the framebuffer's controller only from a dependent ID that is already set
    // (IOFBController::copyController(kDepIDCreate)); without one the controller stays NULL and the first power-change
    // callout dereferences it (IOFramebuffer::powerStateWillChangeTo, panic 2026-10-20)
    setProperty("IOFBDependentID", 0x50485846ull << 32 | 1, 64);   // 'PHXF'
    setProperty("IOFBDependentIndex", 0ull, 32);
    // the backlight's handler comes first: IOGraphics checks for a "backlight" service once, when the framebuffer opens
    fMmio = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    if (fMmio && fMmio->getLength() < (kBlLock + kBlSeqs[1] + 1) * 4) OSSafeReleaseNULL(fMmio);
    uint32_t bl = 0;
    if (PE_parse_boot_argn("-phxbloff", arg, sizeof arg)) PFBLOG("backlight: -phxbloff, no brightness control");
    else if (!(gPhxX & 8) && (blInit(), !backlightRead(&bl))) PFBLOG("backlight: no panel control found (no MMIO view, PWM off, no OLED AUX), no brightness control");
    else if (!(fBacklight = phxBacklightCreate(this))) PFBLOG("backlight: the handler would not attach");
    else PFBLOG("backlight: %s at %u of %u", fBl.mode == phxbl::Panel::Oled ? "OLED AUX" : fBl.mode == phxbl::Panel::Pwm ? "PWM" : "DPCD", bl, kBlMax);
    if (!IOFramebuffer::start(provider)) {
        if (fBacklight) { fBacklight->detach(this); OSSafeReleaseNULL(fBacklight); }
        OSSafeReleaseNULL(fMmio); OSSafeReleaseNULL(fSurfMap); return false; }
    fVa = IORangeAllocator::withRange(kVaSize - 1, PAGE_SIZE, 0, IORangeAllocator::kLocking);
    poolInit();
    setProperty("built-in", kOSBooleanTrue);
    setProperty("IOFBGammaWidth", 8ull, 32);
    setProperty("IOFBGammaCount", 256ull, 32);
    setProperty("DisplayGammaChannels", 3ull, 32);
    setProperty("DisplayGammaEntryCount", 256ull, 32);
    setProperty("DisplayGammaEntrySize", 1ull, 32);
    setProperty("PhxFBWidth", fWidth, 32); setProperty("PhxFBHeight", fHeight, 32);   // the AGDC's panel timing reads these
    measureRefresh();
    setProperty("PhxFBRefreshMilliHz", fRefreshMilliHz, 32);
    if (fHTotal) { setProperty("PhxFBHTotal", fHTotal, 32); setProperty("PhxFBVTotal", fVTotal, 32); }   // the AGDC's panel timing
    nanoseconds_to_absolutetime(fFrameNs, &gPhxFrameAbs);
    fVblTimer = thread_call_allocate(vblFire, this);
    fConnCall = thread_call_allocate(connFire, this);
    if (fVblTimer) { uint64_t d; clock_interval_to_deadline((uint32_t)(fFrameNs / 1000), kMicrosecondScale, &d); fVblNext = d; thread_call_enter_delayed(fVblTimer, d); }
    static IOPMPowerState states[2] = {
        { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    registerPowerDriver(this, states, 2);
    phxHoldInit(this, kBootHoldSec);
    phxAgdcInstall();
    if (!gAliasSysctl) { for (auto *o : kFbOids) sysctl_register_oid(o); gAliasSysctl = true; }
    PFBLOG("panel published: %ux%u row %u at 0x%llx (vbl timer %s)", fWidth, fHeight, fRowBytes,
           (unsigned long long)fBase, fVblTimer ? "running" : "MISSING");
    return true;
}

void PhxAMDFB::stop(IOService *provider)
{
    fStopping = true;
    OSSafeReleaseNULL(fBlMap);
    phxAgdcRemove();
    if (fConnCall) { thread_call_cancel(fConnCall); while (!thread_call_free(fConnCall)) { thread_call_cancel(fConnCall); IOSleep(5); } fConnCall = nullptr; }
    if (fVblTimer) { thread_call_cancel(fVblTimer); while (!thread_call_free(fVblTimer)) { thread_call_cancel(fVblTimer); IOSleep(5); } fVblTimer = nullptr; }
    if (gAliasSysctl) { for (auto *o : kFbOids) sysctl_unregister_oid(o); gAliasSysctl = false; }
    gPhxFrameAbs = 0;
    if (fBacklight) { fBacklight->terminate(); OSSafeReleaseNULL(fBacklight); }
    OSSafeReleaseNULL(fMmio);
    OSSafeReleaseNULL(fAliasMap);
    OSSafeReleaseNULL(fSurfMap);
    OSSafeReleaseNULL(fVa);
    poolFree();
    IOFramebuffer::stop(provider);
}
