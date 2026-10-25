// phxbl.h: the internal eDP panel's backlight, driven from PhxFB (included by phxfb.cpp only).
//
// macOS has no backlight driver for this GPU. Windows/Linux set the brightness of an AMD eDP panel one of two ways, and
// which one this panel uses is read, not assumed:
//   - DPCD over the AUX channel (eDP 1.4 "backlight adjustment by AUX": brightness at DPCD 0x722/0x723, control mode in
//     0x721, enable in 0x720). The usual way for OLED panels, which have no PWM pin. The transaction sequence is the one
//     of amdgpu's dce_aux.c on the DP_AUX block of DCN 3.1.4 (register offsets from dcn_3_1_4_offset.h).
//   - the display engine's own PWM (PWRSEQ0/1 BL_PWM_*), when the panel is a PWM-pin one.
// The ACPI side (DSDT LCD._BCM -> ATIF function 7 "set backlight level") only tells the GPU driver the level; the driver
// has to apply it, so nothing in the firmware does it for us.
//
// Everything here is bounded polling on MMIO; a channel whose AUX block is not enabled is never touched, and the only
// writes are the brightness bytes (and, when the panel is not yet in DPCD mode and has no usable PWM, the mode byte).
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/graphics/IODisplay.h>
#include <libkern/OSAtomic.h>

#define BLLOG(fmt, ...) IOLog("PhxFB: BL: " fmt "\n", ##__VA_ARGS__)

namespace phxbl {

static constexpr uint32_t kDcn = 0x34c0;                              // DCN segment 2 base (dwords) in BAR5
static constexpr uint32_t kAuxBase = 0x1f50, kAuxStride = 0x1c, kAuxChannels = 5;
static constexpr uint32_t kPwm0 = 0x2f19, kPwm1 = 0x2f85;              // PWRSEQn_BL_PWM_CNTL (+1 CNTL2, +2 PERIOD_CNTL, +3 GRP1_REG_LOCK)
static constexpr uint32_t kPwrSeq0 = 0x2f14, kPwrSeq1 = 0x2f80;        // PWRSEQn_PANEL_PWRSEQ_CNTL (+1 STATE)

// eDP DPCD
static constexpr uint32_t kDpEdpRev = 0x700, kDpEdpCap1 = 0x701, kDpEdpAdjCap = 0x702;
static constexpr uint32_t kDpDispCtl = 0x720, kDpBlMode = 0x721, kDpBlMsb = 0x722, kDpBlLsb = 0x723, kDpPwmBits = 0x724;

struct Mmio {
    volatile uint32_t *r = nullptr;
    uint32_t dwords = 0;
    bool ok(uint32_t off) const { return r && kDcn + off < dwords; }
    uint32_t rd(uint32_t off) const { return r[kDcn + off]; }
    void wr(uint32_t off, uint32_t v) const { r[kDcn + off] = v; }
};

// One AUX transaction (native DPCD read/write, up to 16 bytes). Returns the byte count (a write returns len on ACK) or <0:
// -1 bad args/out of BAR, -2 AUX block not enabled, -3 owned by the DMCU, -4 could not take it, -5 no reply, -6 NACK/DEFER,
// -7 timeout or reply error.
static int auxXfer(const Mmio &m, uint32_t ch, bool write, uint32_t addr, uint8_t *buf, uint32_t len)
{
    enum { CTL = 0, SWC = 1, ARB = 2, INT = 3, STA = 4, DAT = 6 };
    if (ch >= kAuxChannels || len < 1 || len > 16) return -1;
    const uint32_t b = kAuxBase + ch * kAuxStride;
    if (!m.ok(b + DAT)) return -1;
    if (!(m.rd(b + CTL) & 1)) return -2;
    if (((m.rd(b + ARB) >> 2) & 3) == 2) return -3;
    m.wr(b + ARB, m.rd(b + ARB) | 0x10000);                               // SW_USE_AUX_REG_REQ
    int ret = -4;
    if (((m.rd(b + ARB) >> 2) & 3) == 1) {                                 // SW can access
        m.wr(b + INT, m.rd(b + INT) | 0x2);                               // SW_DONE_ACK
        for (int i = 0; i < 200 && (m.rd(b + STA) & 1); i++) IODelay(10);
        const uint32_t nbytes = 4 + (write ? len : 0);                    // 3 header bytes + length byte (+ payload)
        m.wr(b + SWC, (m.rd(b + SWC) & ~(0xF0u | 0x1F0000u)) | (nbytes << 16));
        uint32_t v = m.rd(b + DAT) & ~(0xFF00u | 0x1F0000u | 1u | 0x80000000u);   // INDEX 0, write side, ...
        v |= 0x80000000u | ((((write ? 0x80u : 0x90u)) | ((addr >> 16) & 0xF)) << 8);   // ... autoincrement disabled for byte 0
        m.wr(b + DAT, v);
        v &= ~0x80000000u;                                                // autoincrement on for the rest
        v = (v & ~0xFF00u) | (((addr >> 8) & 0xFF) << 8);  m.wr(b + DAT, v);
        v = (v & ~0xFF00u) | ((addr & 0xFF) << 8);         m.wr(b + DAT, v);
        v = (v & ~0xFF00u) | (((len - 1) & 0xFF) << 8);    m.wr(b + DAT, v);
        if (write) for (uint32_t i = 0; i < len; i++) { v = (v & ~0xFF00u) | ((uint32_t)buf[i] << 8); m.wr(b + DAT, v); }
        m.wr(b + SWC, m.rd(b + SWC) | 1);                                 // AUX_SW_GO
        uint32_t st = 0;
        for (int i = 0; i < 3000; i++) { st = m.rd(b + STA); if (st & 1) break; IODelay(10); }
        if (!(st & 1) || (st & (0x70u | 0x80u | 0x200u))) ret = -7;        // not done / RX timeout / HPD low
        else {
            const uint32_t cnt = (st >> 24) & 0x1F;
            if (!cnt) ret = -5;
            else {
                // read side, one byte at a time with an explicit index and no autoincrement: a stuck pointer returned the reply
                // byte for every position on the first try (all zeros, 2026-10-25), so the pointer is never left to the hardware
                auto rdAt = [&](uint32_t k) -> uint32_t { m.wr(b + DAT, 1u | (k << 16) | 0x80000000u); return (m.rd(b + DAT) >> 8) & 0xFF; };
                const uint32_t reply = rdAt(0);                            // reply command in the high nibble: 0 = ACK
                if ((reply >> 4) != 0) ret = -6;
                else if (write) ret = (int)len;
                else {
                    const uint32_t n = cnt - 1 < len ? cnt - 1 : len;
                    for (uint32_t i = 0; i < n; i++) buf[i] = (uint8_t)rdAt(1 + i);
                    ret = (int)n;
                }
            }
        }
    }
    m.wr(b + ARB, (m.rd(b + ARB) | 0x20000u) & ~0x10000u);               // SW_DONE_USING_AUX_REG, drop the request
    return ret;
}

struct Panel {
    enum Mode { None = 0, Dpcd = 1, Pwm = 2 };
    Mmio m;
    IOLock *lock = nullptr;
    Mode mode = None;
    int ch = -1;                    // AUX channel of the eDP sink
    int pwm = -1;                   // 0/1: PWRSEQ block with an enabled PWM
    bool two = false;               // 16-bit DPCD brightness
    bool needMode = false;          // panel not in DPCD mode: switch it on the first set
    uint8_t dpcd[0x10];             // 0x700..0x70f
    uint8_t ctl[8];                 // 0x720..0x727
    uint32_t pwmPeriod = 0;         // full-scale duty value (period << fractional bits)
    uint32_t pwmFrac = 0;
    uint32_t hwMax = 0xFFFF;        // largest hardware value (DPCD 16-bit, or PWM period)
    uint32_t level = 0;             // last level set, 0..kLevels
    static constexpr uint32_t kLevels = 1024;
    uint32_t setCount = 0;

    // user level -> hardware value: perceptual (gamma 2.2) curve over 0.3 %..100 % of the hardware range, so the lowest
    // step is dim but never black (an OLED at 0 looks like a dead screen)
    static uint32_t curve(uint32_t lvl, uint32_t hwmax) {
        if (lvl > kLevels) lvl = kLevels;
        // x^2.2 ~ x^2 * x^0.2; use x^2 * (1 + 0.2*(x-1)*...) -- good enough: x^2.2 via x*x*(0.8+0.2x) for x in [0,1]
        const uint64_t x = lvl;                                           // /1024
        const uint64_t x2 = x * x;                                        // /2^20
        const uint64_t f = (x2 * (800 + 200 * x / kLevels)) / 1000;       // x^2*(0.8+0.2x), /2^20
        uint64_t hw = (f * hwmax) >> 20;
        const uint64_t floorV = (uint64_t)hwmax * 3 / 1000;
        if (hw < floorV) hw = floorV;
        if (hw > hwmax) hw = hwmax;
        return (uint32_t)hw;
    }
    static uint32_t uncurve(uint32_t hw, uint32_t hwmax) {                // inverse by search, 1024 steps is cheap
        uint32_t best = 0, bd = 0xFFFFFFFFu;
        for (uint32_t l = 0; l <= kLevels; l++) { const uint32_t c = curve(l, hwmax); const uint32_t d = c > hw ? c - hw : hw - c; if (d < bd) { bd = d; best = l; } }
        return best;
    }

    void probe(volatile uint32_t *bar, uint32_t bytes) {
        m.r = bar; m.dwords = bytes / 4;
        if (!lock) lock = IOLockAlloc();
        bzero(dpcd, sizeof dpcd); bzero(ctl, sizeof ctl);
        // PWM blocks
        for (int p = 0; p < 2; p++) {
            const uint32_t o = p ? kPwm1 : kPwm0, ps = p ? kPwrSeq1 : kPwrSeq0;
            if (!m.ok(o + 3) || !m.ok(ps + 1)) continue;
            const uint32_t cn = m.rd(o), c2 = m.rd(o + 1), pe = m.rd(o + 2), lk = m.rd(o + 3), pc = m.rd(ps), pst = m.rd(ps + 1);
            BLLOG("PWRSEQ%d: BL_PWM_CNTL %08x CNTL2 %08x PERIOD %08x LOCK %08x | PANEL_PWRSEQ_CNTL %08x STATE %08x", p, cn, c2, pe, lk, pc, pst);
            // 83HW: BL_PWM_CNTL c000fa00, PERIOD 000c0fa0: enabled, fractional, period 4000 with a 12-bit integer part, so the duty
            // register holds the duty left-aligned to 16 bits (4000 << 4 = 0xfa00 = 100 %)
            const uint32_t period = pe & 0xFFFF, bitcnt = (pe >> 16) & 0x1F, fracBits = (cn & 0x40000000u) && bitcnt && bitcnt < 16 ? 16 - bitcnt : 0;
            if ((cn & 0x80000000u) && period && pwm < 0) { pwm = p; pwmPeriod = period << fracBits; pwmFrac = fracBits; }
        }
        // AUX channels that are enabled: who answers, and is it an eDP sink with a backlight
        int pick = -1;
        for (uint32_t c = 0; c < kAuxChannels; c++) {
            const uint32_t b = kAuxBase + c * kAuxStride;
            if (!m.ok(b + 6)) continue;
            const uint32_t cl = m.rd(b), arb = m.rd(b + 2);
            uint8_t d[16]; bzero(d, sizeof d);
            const int n = (cl & 1) ? auxXfer(m, c, false, 0x000, d, 4) : -2;
            BLLOG("AUX%u: CONTROL %08x ARB %08x -> read 0x000 = %d [%02x %02x %02x %02x]", c, cl, arb, n, d[0], d[1], d[2], d[3]);
            if (n != 4 || pick >= 0) continue;
            uint8_t e[16]; bzero(e, sizeof e);
            const int ne = auxXfer(m, c, false, kDpEdpRev, e, 4);
            BLLOG("AUX%u: eDP 0x700..703 = %d [%02x %02x %02x %02x]", c, ne, e[0], e[1], e[2], e[3]);
            if (ne == 4 && e[0] <= 5 && (e[1] || e[2])) {
                pick = (int)c; memcpy(dpcd, e, 4);
                uint8_t k[8]; bzero(k, sizeof k);
                const int nk = auxXfer(m, c, false, kDpDispCtl, k, 5);
                memcpy(ctl, k, 5);
                BLLOG("AUX%u: backlight regs 0x720..724 = %d [%02x %02x %02x %02x %02x]", c, nk, k[0], k[1], k[2], k[3], k[4]);
            }
        }
        ch = pick;
        two = ch >= 0 && (dpcd[2] & 0x04);
        const bool auxCap = ch >= 0 && (dpcd[2] & 0x02);
        const uint32_t dmode = ctl[1] & 3;
        if (auxCap && dmode == 2) mode = Dpcd;                            // already brightness-by-DPCD: the usual OLED case
        else if (pwm >= 0)        mode = Pwm;
        else if (auxCap)          { mode = Dpcd; needMode = true; }       // no PWM to fall back on: put the panel into DPCD mode
        if (mode == Dpcd) {
            hwMax = two ? 0xFFFF : 0xFF;
            const uint32_t cur = two ? ((uint32_t)ctl[2] << 8 | ctl[3]) : ctl[2];
            level = needMode ? kLevels : uncurve(cur, hwMax);
        } else if (mode == Pwm) {
            hwMax = pwmPeriod;
            const uint32_t o = pwm ? kPwm1 : kPwm0;
            level = uncurve(m.rd(o) & 0x7FFFF, hwMax);
        } else level = kLevels;
        BLLOG("backlight: %s (AUX%d, adj cap %02x, mode reg %02x, %s, PWM%d full scale %u) initial level %u/%u",
              mode == Dpcd ? (needMode ? "DPCD (panel not in DPCD mode yet)" : "DPCD") : mode == Pwm ? "PWM" : "NONE: slider will do nothing",
              ch, dpcd[2], ctl[1], two ? "16-bit" : "8-bit", pwm, pwmPeriod, level, kLevels);
    }

    // level 0..kLevels
    bool set(uint32_t lvl) {
        if (!lock) return false;
        if (lvl > kLevels) lvl = kLevels;
        IOLockLock(lock);
        bool ok = false;
        const uint32_t hw = curve(lvl, hwMax);
        if (mode == Dpcd && ch >= 0) {
            if (needMode) {
                uint8_t mb = (uint8_t)((ctl[1] & ~3u) | 2u), en = (uint8_t)(ctl[0] | 1u);
                const int a = auxXfer(m, (uint32_t)ch, true, kDpBlMode, &mb, 1);
                const int b = auxXfer(m, (uint32_t)ch, true, kDpDispCtl, &en, 1);
                BLLOG("switch to DPCD mode: mode reg %02x -> %d, enable %02x -> %d", mb, a, en, b);
                if (a == 1) { ctl[1] = mb; needMode = false; }
            }
            uint8_t v[2] = { (uint8_t)(two ? hw >> 8 : hw), (uint8_t)hw };
            const int n = auxXfer(m, (uint32_t)ch, true, kDpBlMsb, v, two ? 2 : 1);
            ok = n == (two ? 2 : 1);
            if (setCount < 8 || !ok) BLLOG("set level %u -> DPCD %s%02x%02x: %d", lvl, two ? "" : "(8-bit) ", v[0], v[1], n);
        } else if (mode == Pwm && pwm >= 0) {
            const uint32_t o = pwm ? kPwm1 : kPwm0;
            const uint32_t cn = m.rd(o);
            m.wr(o, (cn & ~0x7FFFFu) | (hw & 0x7FFFF));
            ok = true;
            if (setCount < 8) BLLOG("set level %u -> PWM duty %u of %u (BL_PWM_CNTL %08x -> %08x)", lvl, hw, pwmPeriod, cn, m.rd(o));
        } else if (setCount < 8) BLLOG("set level %u: no backlight path", lvl);
        setCount++;
        if (ok) level = lvl;
        IOLockUnlock(lock);
        return ok;
    }
};

}   // namespace phxbl
