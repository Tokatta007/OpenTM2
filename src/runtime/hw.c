/* Hardware registers (0x1F80xxxx) that the recompiled SDK code touches:
 * interrupt controller, DMA, root counters, GPU ports, SPU (PSY-Z). Addresses and
 * bit layouts: psx-spx. The CD-ROM and MDEC ports are not modeled; libcd
 * and the movie player are replaced at the API level (hle.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include <psyz/spu.h>
#include <psyz/audio.h>

static uint32_t dma_madr[8], dma_bcr[8], dma_chcr[8], dpcr = 0x07654321u, dicr;
static uint32_t timer_mode[3], timer_target[3], timer_count[3];

static void dicr_update(void)
{
    uint32_t flags = dicr & 0x7F000000u;
    int irq = (dicr & 0x8000u) || ((dicr & 0x800000u) && (flags >> 24) & ((dicr >> 16) & 0x7F));
    dicr = (dicr & 0x7FFFFFFFu) | (irq ? 0x80000000u : 0);
}

static void dma_done(int ch)
{
    dma_chcr[ch] &= ~((1u << 24) | (1u << 28));
    if (dicr & (1u << (16 + ch))) {
        uint32_t before = dicr & 0x80000000u;
        dicr |= 1u << (24 + ch);
        dicr_update();
        if (!before && (dicr & 0x80000000u))
            rt_raise_irq(IRQ_DMA);
    }
}

static void dma_otc(void)
{
    uint32_t a = dma_madr[6] & (GUEST_RAM_BYTES - 4), n = dma_bcr[6] & 0xFFFF;
    if (!n) n = 0x10000;
    for (uint32_t i = 0; i < n; i++, a -= 4)
        mem_w32(&M.cpu, a, i == n - 1 ? 0xFFFFFFu : ((a - 4) & 0xFFFFFFu));
}

/* SPU DMA (channel 4): block transfers to or from sound RAM at the SPU's
 * transfer address, which advances as on hardware. */
static void dma_spu(void)
{
    uint32_t words = (dma_bcr[4] & 0xFFFF) * (dma_bcr[4] >> 16), a = dma_madr[4] & (GUEST_RAM_BYTES - 4);
    uint32_t bytes = words * 4;
    if (a + bytes > GUEST_RAM_BYTES) bytes = GUEST_RAM_BYTES - a;
    if (getenv("TM2PC_SPU_TRACE") && (dma_chcr[4] & 1))
        fprintf(stderr, "spu %u: upload %u bytes at sample %04X\n",(unsigned)M.vblanks, (unsigned)bytes,
                Psyz_SpuGetTransferAddr() / 8);
    Psyz_AudioLock();
    if (dma_chcr[4] & 1) {
        Psyz_SpuFifoWriteBulk(M.ram + a, bytes);
    } else {
        uint32_t t = Psyz_SpuGetTransferAddr();
        Psyz_SpuMemRead(t, M.ram + a, bytes);
        Psyz_SpuSetTransferAddr((t + bytes) & (PSYZ_SPU_RAM_SIZE - 1));
    }
    Psyz_AudioUnlock();
}

static void dma_start(int ch)
{
    switch (ch) {
    case 2: gpu_dma(dma_madr[2], dma_bcr[2], dma_chcr[2]); break;
    case 4: dma_spu(); break;
    case 6: dma_otc(); break;
    default: rt_unexpected("dma channel", (uint32_t)ch, dma_chcr[ch]); break;
    }
    dma_done(ch);
}

/* SPU registers (0x1F801C00..0x1F801FFF): PSY-Z's register-level SPU,
 * which the audio thread also runs, so access takes the audio lock. */
static uint64_t spu_keyon_at[48]; /* each voice's last key-on (VBlanks) */
static uint32_t spu_read(uint32_t off)
{
    static int ext_test = -1; /* (TM2PC_SPU_EXT_TEST, below: a voice's volumes read back are its twin's) */
    if (ext_test < 0) ext_test = getenv("TM2PC_SPU_EXT_TEST") != NULL;
    Psyz_AudioLock();
    uint32_t v = ext_test && off < 0x180 && (off & 15) < 4 ? Psyz_SpuReadExt(off) : Psyz_SpuRead(off);
    Psyz_AudioUnlock();
    static int trace = -1; /* (TM2PC_SPU_TRACE=3: a voice's envelope read as 0 within 12 VBlanks of its key-on) */
    if (trace < 0) trace = getenv("TM2PC_SPU_TRACE") ? atoi(getenv("TM2PC_SPU_TRACE")) : 0;
    if (trace == 3 && off < 0x180 && (off & 15) == 0xC && !v && M.vblanks - spu_keyon_at[off >> 4] < 12)
        fprintf(stderr, "spu %u: voice %u envelope 0, %u VBlanks after its key-on (ra %08X)\n",(unsigned)M.vblanks,
                (unsigned)(off >> 4), (unsigned)(M.vblanks - spu_keyon_at[off >> 4]), M.cpu.r[31]);
    return v;
}

extern int spu_port_keyon;
extern int spu_thin_menu;
extern int spu_keyon_record;
extern uint64_t spu_keyed;
int feats_in_battle(void); /* profile/feat_hooks.c: a match is running (the menus keep their double boom) */
static uint32_t spu_thin_keyon(uint32_t off, uint32_t v, int ext);
static uint16_t spu_volume[48][2]; /* each voice's volume as last written, left and right (the SPU reads back its current one) */
static uint32_t spu_thin_volume(int voice, int side, uint32_t v);
/* A key-on's voices: the port's bank each plays from (vplus/mod_sounds.c: a mod character's own sounds; NULL the SPU's
 * RAM), found before the audio lock is taken (a first use reads and encodes the mod's files), set with it held */
void mod_sound_keyon(int voice, const unsigned char **ram, unsigned *start, int *pitch_scale);
typedef struct { int n, voice[16], scale[16]; const unsigned char *ram[16]; unsigned start[16]; } SpuBanks;
static void spu_banks_find(SpuBanks *k, uint32_t off, uint32_t v, int ext)
{
    k->n = 0;
    if ((off != 0x188 && off != 0x18A) || !(v & 0xFFFF)) return;
    int base = (ext ? 24 : 0) + (off == 0x18A ? 16 : 0);
    for (int b = 0; b < 16; b++)
        if ((v & (1u << b)) && base + b < 48) {
            k->voice[k->n] = base + b;
            mod_sound_keyon(base + b, &k->ram[k->n], &k->start[k->n], &k->scale[k->n]);
            k->n++;
        }
}
static void spu_banks_set(const SpuBanks *k) /* (the audio lock held) */
{
    for (int i = 0; i < k->n; i++) Psyz_SpuSetVoiceBank(k->voice[i], k->ram[i], k->start[i], k->scale[i]);
}
int mod_sound_pitch(int voice, unsigned pitch); /* vplus/mod_sounds.c: a bank's voice's first pitch, its scale (0 none) */
static void spu_banks_pitch(uint32_t off, uint32_t v, int ext) /* (the audio lock held) a voice's pitch written */
{
    if (off >= 0x180 || (off & 15) != 4) return;
    int voice = (ext ? 24 : 0) + (int)(off >> 4), scale = mod_sound_pitch(voice, v & 0xFFFF);
    if (scale) Psyz_SpuSetVoicePitchScale(voice, scale);
}
/* tests: TM2PC_SPU_SAMPLE=HEX[,HEX..] (up to 8): the volume and pitch writes of the voices playing those samples */
static int spu_sample_traced(int sample)
{
    static int n = -1, list[8];
    if (n < 0) {
        n = 0;
        const char *e = getenv("TM2PC_SPU_SAMPLE");
        while (e && *e && n < 8) {
            char *end;
            list[n++] = (int)strtol(e, &end, 16);
            e = *end == ',' ? end + 1 : NULL;
        }
    }
    for (int i = 0; i < n; i++) if (list[i] == sample) return 1;
    return 0;
}
static void spu_write(uint32_t off, uint32_t v)
{
    if (off < 0x180 && (off & 15) < 4) v = spu_thin_volume((int)(off >> 4), (int)(off >> 1) & 1, v);
    /* TM2PC_SPU_TRACE=1: log key on/off with each voice's sample address */
    static int trace = -1;
    if (trace < 0) trace = getenv("TM2PC_SPU_TRACE") ? atoi(getenv("TM2PC_SPU_TRACE")) : 0;
    if (off < 0x180 && (off & 15) < 6 && spu_sample_traced((int)Psyz_SpuRead((off & ~15u) + 6)))
        fprintf(stderr, "spu %u: voice %u (sample %04X) %s %04X ra %08X\n", (unsigned)M.vblanks, (unsigned)(off >> 4),
                (unsigned)Psyz_SpuRead((off & ~15u) + 6), (off & 15) == 4 ? "pitch" : (off & 2) ? "volume right" : "volume left",
                (unsigned)(v & 0xFFFF), M.cpu.r[31]);
    if (trace == 2 && off < 0x180 && (off >> 4) >= 20 && (off >> 4) < 24 && (off & 15) < 4)
        fprintf(stderr, "spu %u: voice %u volume %s %04X\n",(unsigned)M.vblanks, (unsigned)(off >> 4),
                (off & 2) ? "right" : "left", (unsigned)(v & 0xFFFF));
    if (trace && off >= 0x188 && off <= 0x18E && (v & 0xFFFF)) {
        int base = (off & 2) ? 16 : 0;
        for (int b = 0; b < 16; b++)
            if (v & (1u << b))
                fprintf(stderr, "spu %u: key %s voice %d sample %04X vol %04X %04X pitch %04X ra %08X\n", (unsigned)M.vblanks,
                        off < 0x18C ? "on" : "off", base + b, Psyz_SpuRead((uint32_t)(base + b) * 16 + 6),
                        Psyz_SpuRead((uint32_t)(base + b) * 16 + 0), Psyz_SpuRead((uint32_t)(base + b) * 16 + 2),
                        Psyz_SpuRead((uint32_t)(base + b) * 16 + 4), M.cpu.r[31]);
        if (trace == 3) fprintf(stderr, "spu %u: sp %08X okof %08X %08X\n", (unsigned)M.vblanks, M.cpu.r[29],
                                mem_r32(&M.cpu, 0x801FE8D8u), mem_r32(&M.cpu, 0x801FE8E0u));
    }
    v = spu_thin_keyon(off, v, 0);
    if (spu_keyon_record && (off == 0x188 || off == 0x18A)) spu_keyed |= (uint64_t)(v & 0xFFFF) << (off == 0x18A ? 16 : 0);
    /* tests: TM2PC_SPU_EXT_TEST=1, each voice n played by the extra voice 24 + n instead (its registers, keys and
     * reverb bit mirrored there, its own volume 0): the output the same as without (--wav), through the extra voices */
    static int ext_test = -1;
    if (ext_test < 0) ext_test = getenv("TM2PC_SPU_EXT_TEST") != NULL;
    int mirror = ext_test && (off < 0x180 || (off >= 0x188 && off <= 0x18E) || off == 0x198 || off == 0x19A);
    SpuBanks banks;
    spu_banks_find(&banks, off, v, 0);
    Psyz_AudioLock();
    spu_banks_set(&banks);
    spu_banks_pitch(off, v, 0);
    if (mirror && off < 0x180) Psyz_SpuWriteExt(off, (unsigned short)v);
    Psyz_SpuWrite(off, (unsigned short)(mirror && off < 0x180 && (off & 15) < 4 ? 0 : v));
    if (mirror && off >= 0x188) Psyz_SpuWriteExt(off, (unsigned short)v);
    Psyz_AudioUnlock();
}

/* Every voice keyed off (both register blocks): the results' silence after a battle (the user's: the lava burn and the
 * engine looped on under the scoreboard); the music is the CD input's, not a voice */
void spu_all_keys_off(void)
{
    Psyz_AudioLock();
    Psyz_SpuWrite(0x18C, 0xFFFF);
    Psyz_SpuWrite(0x18E, 0x00FF);
    Psyz_SpuWriteExt(0x18C, 0xFFFF);
    Psyz_SpuWriteExt(0x18E, 0x00FF);
    Psyz_AudioUnlock();
}

/* In a battle, stacked sounds are thinned (the port's own sounds, spu_port_keyon, and the menus are left alone). One
 * sound is a sample started at one place: the same sample at the same volume on both sides (within a quarter, at
 * least 0x400: the camera moved). The game keys a voice on first and sets its volume a frame later, so a voice is
 * judged at its first volume (both sides written after its key-on): a copy is muted (its volume written 0) until its
 * next key-on. A car's explosion (sample 0x1264, address / 8) is started on several voices a few frames apart (2,
 * then up to 6 at a death): one plays, a copy within 12 VBlanks is muted. Any sound started on more than two voices
 * at once (Axel's special: three sounds on four voices each) is as loud as all of them together: two play (a sound's
 * two sides), the rest are muted. Another car's (another place: other volumes) is its own sound and plays (16 cars:
 * several firing, hit or blown up in the same frames; the user's playtest of 2026-10-28, sounds missing). One record
 * for both blocks. TM2PC_THIN_LOG=1: each copy muted. */
static uint64_t spu_muted;       /* (bit v: voice v, 0..47) a copy: its volume 0 until its next key-on */
static uint64_t spu_judge;       /* keyed on in a battle, its first volume not seen yet */
static uint8_t spu_judge_sides[48]; /* (bit 0 left, 1 right written since the key-on) */
static uint16_t spu_judge_sample[48];
static int spu_same_volume(uint16_t a, uint16_t b)
{
    if ((a | b) & 0x8000) return a == b; /* (a sweep: only the same one) */
    int d = (int)a - (int)b, m = a > b ? a : b;
    return (d < 0 ? -d : d) <= (m / 4 > 0x400 ? m / 4 : 0x400);
}
static uint32_t spu_thin_keyon(uint32_t off, uint32_t v, int ext) /* a key-on word: its voices to be judged */
{
    if ((off == 0x188 || off == 0x18A) && (v & 0xFFFF)) {
        int base = (ext ? 24 : 0) + (off == 0x18A ? 16 : 0), judge = !spu_port_keyon && (feats_in_battle() || spu_thin_menu);
        for (int b = 0; b < 16; b++) {
            if (!(v & (1u << b)) || base + b >= 48) continue;
            uint64_t bit = 1ull << (base + b);
            spu_keyon_at[base + b] = M.vblanks;
            spu_muted &= ~bit;
            spu_judge = judge ? spu_judge | bit : spu_judge & ~bit;
            spu_judge_sides[base + b] = 0;
            uint32_t reg = (uint32_t)(base + b - (ext ? 24 : 0)) * 16 + 6;
            spu_judge_sample[base + b] = (uint16_t)(ext ? Psyz_SpuReadExt(reg) : Psyz_SpuRead(reg));
        }
    }
    return v;
}
static uint32_t spu_thin_volume(int voice, int side, uint32_t v) /* a voice's volume written: what the SPU gets */
{
    if (voice < 0 || voice >= 48) return v;
    uint64_t bit = 1ull << voice;
    spu_volume[voice][side] = (uint16_t)v;
    if (spu_judge & bit) {
        spu_judge_sides[voice] |= (uint8_t)(1 << side);
        if (spu_judge_sides[voice] == 3) { /* both sides: judged */
            spu_judge &= ~bit;
            static struct { uint16_t sample, l, r; uint64_t at; int n; } recent[32], boom[8];
            uint16_t sample = spu_judge_sample[voice], l = spu_volume[voice][0], r = spu_volume[voice][1];
            int copy = 0;
            if (sample == 0x1264) {
                int k = -1, oldest = 0;
                for (int i = 0; i < 8; i++) {
                    if (M.vblanks < boom[i].at + 12 && spu_same_volume(boom[i].l, l) && spu_same_volume(boom[i].r, r)) { k = i; break; }
                    if (boom[i].at < boom[oldest].at) oldest = i;
                }
                if (k >= 0) copy = 1;
                else { boom[oldest].at = M.vblanks; boom[oldest].l = l; boom[oldest].r = r; }
            }
            if (!copy) {
                int slot = -1, oldest = 0;
                for (int i = 0; i < 32; i++) {
                    if (recent[i].sample == sample && M.vblanks <= recent[i].at + 2 && spu_same_volume(recent[i].l, l) &&
                        spu_same_volume(recent[i].r, r)) { slot = i; break; }
                    if (recent[i].at < recent[oldest].at) oldest = i;
                }
                if (slot < 0) {
                    slot = oldest;
                    recent[slot].sample = sample; recent[slot].l = l; recent[slot].r = r;
                    recent[slot].at = M.vblanks; recent[slot].n = 0;
                }
                copy = ++recent[slot].n > 2;
            }
            if (copy) spu_muted |= bit;
            static int log = -1;
            if (log < 0) log = getenv("TM2PC_THIN_LOG") ? atoi(getenv("TM2PC_THIN_LOG")) : 0;
            if (log && (copy || log >= 2))
                fprintf(stderr, "spu thin: v%llu voice %d sample %04X vol %04X %04X %s\n", (unsigned long long)M.vblanks, voice,
                        sample, l, r, copy ? "muted" : "plays");
        }
    }
    return spu_muted & bit ? 0 : v;
}

/* The port's 24 voices more (24..47; PSY-Z's PSYZ_SPU_EXT_VOICES, psyz-17-spu-voices.patch): their registers at
 * 0x1F802C00..0x1F802DFF, laid out as the SPU's first 0x200 bytes (voice 24's at +0, key on +0x188/+0x18A, key off
 * +0x18C/+0x18E, reverb on +0x198/+0x19A, ENDX +0x19C/+0x19E; bit n: voice 24+n). The PS1's expansion region 2,
 * unused by the game: a build of the decomp's sound library with more voices writes them there (DECOMP_REQUESTS.md). */
#define SPU_EXT 0x1F802C00u
static uint32_t spu_ext_read(uint32_t off)
{
    Psyz_AudioLock();
    uint32_t v = Psyz_SpuReadExt(off);
    Psyz_AudioUnlock();
    return v;
}
static void spu_ext_write(uint32_t off, uint32_t v)
{
    if (off < 0x180 && (off & 15) < 4) v = spu_thin_volume(24 + (int)(off >> 4), (int)(off >> 1) & 1, v);
    v = spu_thin_keyon(off, v, 1);
    if (spu_keyon_record && (off == 0x188 || off == 0x18A)) spu_keyed |= (uint64_t)(v & 0xFFFF) << (off == 0x18A ? 40 : 24);
    static int trace = -1; /* (TM2PC_SPU_TRACE=1: block 2's key on/off too) */
    if (trace < 0) trace = getenv("TM2PC_SPU_TRACE") ? atoi(getenv("TM2PC_SPU_TRACE")) : 0;
    if (off < 0x180 && (off & 15) < 6 && spu_sample_traced((int)Psyz_SpuReadExt((off & ~15u) + 6)))
        fprintf(stderr, "spu %u: voice %u (sample %04X) %s %04X ra %08X\n", (unsigned)M.vblanks, 24 + (unsigned)(off >> 4),
                (unsigned)Psyz_SpuReadExt((off & ~15u) + 6), (off & 15) == 4 ? "pitch" : (off & 2) ? "volume right" : "volume left",
                (unsigned)(v & 0xFFFF), M.cpu.r[31]);
    if (trace && off >= 0x188 && off <= 0x18E && (v & 0xFFFF))
        for (int b = 0; b < 16; b++)
            if (v & (1u << b)) {
                int w = ((off & 2) ? 16 : 0) + b;
                fprintf(stderr, "spu %u: key %s voice %d sample %04X\n", (unsigned)M.vblanks, off < 0x18C ? "on" : "off", 24 + w,
                        Psyz_SpuReadExt((uint32_t)w * 16 + 6));
            }
    SpuBanks banks;
    spu_banks_find(&banks, off, v, 1);
    Psyz_AudioLock();
    spu_banks_set(&banks);
    spu_banks_pitch(off, v, 1);
    Psyz_SpuWriteExt(off, (unsigned short)v);
    Psyz_AudioUnlock();
}

int spu_port_keyon; /* the port's own sounds are being started (not thinned) */
int spu_thin_menu; /* (unused: car select's specials are scaled, not thinned) */
int spu_keyon_record; /* set: the voices keyed on are noted in spu_keyed (car select's specials) */
uint64_t spu_keyed; /* (bit v: voice v, 0..47) */

void hw_timer_counts(uint32_t *get, const uint32_t *set) /* the kill cam's snapshots: the counters 0-1 the game reads */
{
    for (int i = 0; i < 3; i++) { if (get) get[i] = timer_count[i]; if (set) timer_count[i] = set[i]; }
}
static uint32_t timer_read(int n, uint32_t reg)
{
    if (reg == 0) {
        if (n == 2) /* game clock: position within the current period */
            return (uint32_t)(M.clock_acc * ((timer_target[2] & 0xFFFF) ? (timer_target[2] & 0xFFFF) : 0xFFFF));
        return timer_count[n]++ & 0xFFFF;
    }
    return reg == 4 ? timer_mode[n] : timer_target[n];
}

uint32_t hw_read(uint32_t addr, int size)
{
    uint32_t p = addr & 0x1FFFFFFFu, v = 0;
    if (p == 0x1F801070) v = M.i_stat;
    else if (p == 0x1F801074) v = M.i_mask;
    else if (p >= 0x1F801080 && p < 0x1F8010F0) {
        int ch = (p - 0x1F801080) >> 4;
        uint32_t reg = p & 0xF;
        v = reg == 0 ? dma_madr[ch] : reg == 4 ? dma_bcr[ch] : dma_chcr[ch];
    } else if (p == 0x1F8010F0) v = dpcr;
    else if (p == 0x1F8010F4) v = dicr;
    else if (p >= 0x1F801100 && p < 0x1F801130) v = timer_read((p - 0x1F801100) >> 4, p & 0xF);
    else if (p == 0x1F801810) v = gpu_read_data();
    else if (p == 0x1F801814) v = gpu_read_status();
    else if (p >= 0x1F801C00 && p < 0x1F802000) v = spu_read(p - 0x1F801C00);
    else if (p >= SPU_EXT && p < SPU_EXT + 0x200)
        v = size == 4 ? spu_ext_read(p - SPU_EXT) | spu_ext_read(p - SPU_EXT + 2) << 16 : spu_ext_read(p - SPU_EXT);
    else if ((p >= 0x1F801000 && p < 0x1F801024) || p == 0x1F801060) v = 0;
    else rt_unexpected("io read", addr, (uint32_t)size);
    rt_deliver_pending(); /* the game is polling: let pending interrupts happen */
    return v;
}

void hw_write(uint32_t addr, uint32_t v, int size)
{
    uint32_t p = addr & 0x1FFFFFFFu;
    if (p == 0x1F801070) M.i_stat &= v;           /* acknowledge */
    else if (p == 0x1F801074) { M.i_mask = v & 0x7FF; rt_deliver_pending(); }
    else if (p >= 0x1F801080 && p < 0x1F8010F0) {
        int ch = (p - 0x1F801080) >> 4;
        uint32_t reg = p & 0xF;
        if (reg == 0) dma_madr[ch] = v & 0xFFFFFF;
        else if (reg == 4) dma_bcr[ch] = v;
        else if (reg == 8) {
            dma_chcr[ch] = v;
            if (v & (1u << 24))
                dma_start(ch);
        }
    } else if (p == 0x1F8010F0) dpcr = v;
    else if (p == 0x1F8010F4) {
        dicr = (v & 0x00FFFFFFu) | ((dicr & 0x7F000000u) & ~(v & 0x7F000000u));
        dicr_update();
    } else if (p >= 0x1F801100 && p < 0x1F801130) {
        int n = (p - 0x1F801100) >> 4;
        uint32_t reg = p & 0xF;
        if (reg == 0) timer_count[n] = v;
        else if (reg == 4) timer_mode[n] = v;
        else timer_target[n] = v;
    } else if (p == 0x1F801810) gpu_write_gp0(v);
    else if (p == 0x1F801814) gpu_write_gp1(v);
    else if (p >= 0x1F801C00 && p < 0x1F802000) {
        if (size == 4) {
            spu_write(p - 0x1F801C00, v & 0xFFFF);
            spu_write(p - 0x1F801C00 + 2, v >> 16);
        } else
            spu_write(p - 0x1F801C00, v);
    } else if (p >= SPU_EXT && p < SPU_EXT + 0x200) {
        spu_ext_write(p - SPU_EXT, v & 0xFFFF);
        if (size == 4) spu_ext_write(p - SPU_EXT + 2, v >> 16);
    } else if ((p >= 0x1F801000 && p < 0x1F801024) || p == 0x1F801060 || addr == 0xFFFE0130) {
        /* memory control, RAM size, cache control: nothing to do */
    } else
        rt_unexpected("io write", addr, v);
}

/* Start the SPU and, with output, the SDL audio device that pulls from it. */
int hw_audio_init(int output)
{
    Psyz_SpuInit();
    return output ? Psyz_AudioInit() : 1;
}

/* --wav (headless): pull the SPU's output each VBlank into a WAV file, so a
 * run's sound can be checked without listening. */
static FILE *wav;
static uint32_t wav_frames;
static double wav_acc;

static void wav_header(void)
{
    uint32_t data = wav_frames * 4, riff = 36 + data, rate = 44100, byte_rate = rate * 4, fmt = 16;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
    fwrite(&fmt, 4, 1, wav); fwrite(&pcm, 2, 1, wav); fwrite(&ch, 2, 1, wav);
    fwrite(&rate, 4, 1, wav); fwrite(&byte_rate, 4, 1, wav); fwrite(&align, 2, 1, wav);
    fwrite(&bits, 2, 1, wav); fwrite("data", 1, 4, wav); fwrite(&data, 4, 1, wav);
    fseek(wav, 0, SEEK_END);
}

int hw_wav_open(const char *path)
{
    wav = fopen(path, "wb");
    if (wav) wav_header();
    return wav != 0;
}

void hw_wav_vblank(void)
{
    if (!wav) return;
    wav_acc += 44100.0 / NTSC_VBLANK_HZ;
    int n = (int)wav_acc;
    wav_acc -= n;
    short buf[2 * 1024];
    if (n > 1024) n = 1024;
    Psyz_SpuPullSamples(buf, n);
    fwrite(buf, 4, (size_t)n, wav);
    wav_frames += (uint32_t)n;
    if ((wav_frames & 0x3FFF) < (uint32_t)n) { wav_header(); fflush(wav); }
}
