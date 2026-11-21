/* Mod characters' own sounds (docs/MODDING.md 4.1): engine.wav and special.wav in place of their base car's.
 *
 * The game keeps its samples in SPU RAM (UACORE.VAB from boot, the arena's bank) and has no room for more, so a
 * character's are played from a bank of the port's: PSY-Z patch 22's Psyz_SpuSetVoiceBank, a voice decoding from the
 * host's memory (512 KB laid out as SPU RAM) from its key-on. At each key-on (runtime/hw.c) the decomp says whose sound
 * the voice plays (request 87: portVoiceCar, the car slot + 1, and portVoiceKind, 1 the engine, 2 the special); a car
 * slot on a character with that file gets the character's sample in place of the base's: its registers as the game set
 * them (volume, envelope, reverb, pitch as the engine's revs move it), its pitch scaled so the file plays at its own
 * rate at the pitch the game starts it with: some sounds have it written before the key-on, others just after, so the
 * key-on's pitch until the first one written after it, or until two VBlanks have passed without one. The engine loops;
 * the special loops when the base's sample does.
 *
 * A character's bank is read and encoded at its first key-on (mod_wav.c), kept until the mods change. Sounds are not
 * match state (the rollback hashes no SPU), so a machine without a character's sounds plays the base's.
 * Tests: TM2PC_TEST_MOD_SOUND=<mod index>,<kind>,<sample> plays that mod's sound on every voice keyed on with that
 * sample (the start address register, as TM2PC_SPU_TRACE prints it; * every sound), without the decomp's tags. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime/runtime.h"
#include "mods/mods.h"

#define BANK_BYTES (512u * 1024u) /* (PSYZ_SPU_RAM_SIZE: the decoder's addresses wrap within it) */
enum { BANKS = 16, KIND_ENGINE = 1, KIND_SPECIAL = 2 };
static struct {
    int mod, generation;             /* its index in the mods list, at mods_generation */
    unsigned char *ram;              /* the bank (NULL: none read, or nothing in it) */
    unsigned start[3][2];            /* by kind (1, 2): played once [0] and looped [1], byte offsets; 0 none */
    int rate[3];
    unsigned long used;              /* (the oldest bank goes first when all are taken) */
} banks[BANKS];
static unsigned long bank_clock;

int mod_cars_group_number(int g);  /* vplus/mod_cars.c */
int mod_car_mod(int number);
unsigned short Psyz_SpuRead(unsigned int reg_offset);
unsigned short Psyz_SpuReadExt(unsigned int reg_offset);
unsigned char *Psyz_SpuGetRam(void);

static unsigned short voice_reg(int v, unsigned reg)
{
    return v < 24 ? Psyz_SpuRead((unsigned)v * 16 + reg) : Psyz_SpuReadExt((unsigned)(v - 24) * 16 + reg);
}

/* the mod's sounds encoded into a bank: block 0 silent, then each sound once and looped */
static int bank_load(int b, int mod)
{
    const Mod *m = mods_get(mod);
    banks[b].mod = mod;
    banks[b].generation = mods_generation();
    banks[b].used = ++bank_clock;
    memset(banks[b].start, 0, sizeof banks[b].start);
    if (!banks[b].ram) banks[b].ram = calloc(1, BANK_BYTES);
    if (!banks[b].ram || !m) return 0;
    memset(banks[b].ram, 0, BANK_BYTES);
    unsigned at = 16;
    static const char *const files[3] = {NULL, "engine.wav", "special.wav"};
    static const int seconds[3] = {0, MOD_ENGINE_SECONDS, MOD_SPECIAL_SECONDS};
    for (int k = KIND_ENGINE; k <= KIND_SPECIAL; k++) {
        char path[700], why[80];
        ModWav w;
        snprintf(path, sizeof path, "%s%s", m->dir, files[k]);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fclose(f);
        if (!mod_wav_read(path, &w, seconds[k], why, sizeof why)) {
            fprintf(stderr, "mods: %s: %s %s (its base's sound instead)\n", m->name, files[k], why);
            continue;
        }
        unsigned n = (unsigned)mod_adpcm_size(w.frames);
        if (at + 2 * n > BANK_BYTES) { mod_wav_free(&w); continue; }
        for (int loop = 0; loop < 2; loop++) {
            mod_adpcm_encode(w.pcm, w.frames, loop, banks[b].ram + at);
            banks[b].start[k][loop] = at;
            at += n;
        }
        banks[b].rate[k] = w.rate;
        fprintf(stderr, "mods: %s: %s, %.2f s at %d Hz\n", m->name, files[k], (double)w.frames / w.rate, w.rate);
        mod_wav_free(&w);
    }
    const char *dump = getenv("TM2PC_MOD_SOUND_DUMP"); /* tests: the bank as encoded */
    FILE *f = dump ? fopen(dump, "wb") : NULL;
    if (f) { fwrite(banks[b].ram, 1, at, f); fclose(f); }
    return 1;
}
static int bank_of(int mod)
{
    int oldest = 0;
    for (int b = 0; b < BANKS; b++) {
        if (banks[b].used && banks[b].mod == mod && banks[b].generation == mods_generation()) {
            banks[b].used = ++bank_clock;
            return b;
        }
        if (banks[b].used < banks[oldest].used) oldest = b;
    }
    return bank_load(oldest, mod) ? oldest : -1;
}

/* the game's sample at a voice's start address loops (its last block jumps back) */
static int sample_loops(unsigned start)
{
    const unsigned char *ram = Psyz_SpuGetRam();
    if (!ram) return 0;
    for (unsigned a = (start << 3) & (BANK_BYTES - 16), n = 0; n < BANK_BYTES / 16; n++, a = (a + 16) & (BANK_BYTES - 16))
        if (ram[a + 1] & 1) return (ram[a + 1] & 2) != 0;
    return 0;
}

/* whose sound voice v is keyed on for: the mod (its index in the mods list) and kind, else 0 */
static int voice_owner(int v, int *mod)
{
    const char *t = getenv("TM2PC_TEST_MOD_SOUND");
    if (t) {
        int m, k;
        unsigned sample;
        if (sscanf(t, "%d,%d,%x", &m, &k, &sample) == 3 && voice_reg(v, 6) == sample) { *mod = m; return k; }
        if (sscanf(t, "%d,%d,*", &m, &k) == 2 && strchr(t, '*')) { *mod = m; return k; } /* (every sound) */
        return 0;
    }
    static uint32_t car = 1, kind, groups;
    if (car == 1) { car = port_sym("portVoiceCar"); kind = port_sym("portVoiceKind"); groups = port_sym("portCarGroups"); }
    if (!car || !kind || !groups || v < 0 || v >= 48) return 0;
    int slot = mem_r8(&M.cpu, car + (uint32_t)v) - 1, k = mem_r8(&M.cpu, kind + (uint32_t)v);
    if (slot < 0 || (k != KIND_ENGINE && k != KIND_SPECIAL)) return 0;
    int group = (int)mem_r32(&M.cpu, groups + 4u * (uint32_t)slot); /* (portCarGroups.slot_group) */
    int m = mod_car_mod(mod_cars_group_number(group));
    const Mod *md = m >= 0 ? mods_get(m) : NULL;
    if (!md || md->kind != MOD_CHARACTER || !mod_on(md)) return 0;
    *mod = m;
    return k;
}

static int voice_rate[48];  /* a voice playing from a bank: its file's rate */
static uint64_t voice_until[48]; /* the VBlank its starting pitch is taken by (0: taken) */
static int scale_for(int rate, unsigned pitch) /* the file at its own rate at that pitch: rate / 44100 x 4096 / pitch */
{
    long long scale = (long long)rate * 4096 * 4096 / (44100LL * (pitch ? pitch : 4096));
    return scale < 1 ? 1 : scale > 16 * 4096 ? 16 * 4096 : (int)scale;
}

/* runtime/hw.c, a voice's pitch written: its bank's scale from now (0: none to change) */
int mod_sound_pitch(int v, unsigned pitch)
{
    if (v < 0 || v >= 48 || M.vblanks >= voice_until[v] || !(pitch & 0x3FFF)) return 0;
    voice_until[v] = 0;
    return scale_for(voice_rate[v], pitch & 0x3FFF);
}

/* runtime/hw.c, before voice v's key-on is written: the bank it plays from (NULL: SPU RAM), its start, pitch scale */
void mod_sound_keyon(int v, const unsigned char **ram, unsigned *start, int *pitch_scale)
{
    *ram = NULL;
    *start = 0;
    *pitch_scale = 4096;
    if (v >= 0 && v < 48) voice_until[v] = 0;
    int mod = -1, k = voice_owner(v, &mod);
    if (!k) return;
    int b = bank_of(mod);
    if (b < 0) return;
    int loop = k == KIND_ENGINE || sample_loops(voice_reg(v, 6));
    if (!banks[b].start[k][loop]) return;
    *ram = banks[b].ram;
    *start = banks[b].start[k][loop];
    *pitch_scale = scale_for(banks[b].rate[k], voice_reg(v, 4) & 0x3FFF);
    voice_rate[v] = banks[b].rate[k];
    voice_until[v] = M.vblanks + 2;
    if (getenv("TM2PC_SOUND_TRACE"))
        fprintf(stderr, "mod sound: voice %d plays %s's %s%s\n", v, mods_get(mod)->name,
                k == KIND_ENGINE ? "engine" : "special", loop ? " (looped)" : "");
}

/* tm2pc --test-mod-sound <character folder> <out.wav> (tests): the folder's sounds encoded into a bank and played by
 * the SPU's voice 0 as the game would key them (the special once at pitch 0x0800, the engine looped at 0x1000, at
 * full volume), 3 seconds of the SPU's output written to out.wav: each file at its own rate, the engine twice over */
int mod_sound_selftest(const char *folder, const char *out)
{
    void Psyz_SpuInit(void);
    void Psyz_SpuWrite(unsigned int reg_offset, unsigned short value);
    void Psyz_SpuSetVoiceBank(int voice, const unsigned char *ram, unsigned int start, int pitch_scale);
    void Psyz_SpuPullSamples(short *out, int num_frames);
    static Mod m;
    snprintf(m.dir, sizeof m.dir, "%s%s", folder, folder[0] && folder[strlen(folder) - 1] != '/' ? "/" : "");
    snprintf(m.name, sizeof m.name, "test");
    m.kind = MOD_CHARACTER;
    /* (bank_load reads mods_get: this folder as the mods list's only entry is not possible, so its own copy) */
    unsigned char *ram = calloc(1, BANK_BYTES);
    unsigned start[3][2] = {{0}};
    int rate[3] = {0};
    unsigned at = 16;
    static const char *const files[3] = {NULL, "engine.wav", "special.wav"};
    for (int k = KIND_ENGINE; k <= KIND_SPECIAL; k++) {
        char path[700], why[80];
        ModWav w;
        snprintf(path, sizeof path, "%s%s", m.dir, files[k]);
        if (!mod_wav_read(path, &w, k == KIND_ENGINE ? MOD_ENGINE_SECONDS : MOD_SPECIAL_SECONDS, why, sizeof why)) {
            fprintf(stderr, "%s %s\n", files[k], why);
            free(ram);
            return 1;
        }
        unsigned n = (unsigned)mod_adpcm_size(w.frames);
        for (int loop = 0; loop < 2; loop++) {
            mod_adpcm_encode(w.pcm, w.frames, loop, ram + at);
            start[k][loop] = at;
            at += n;
        }
        rate[k] = w.rate;
        mod_wav_free(&w);
    }
    Psyz_SpuInit();
    Psyz_SpuWrite(0x180, 0x3FFF); /* main volume */
    Psyz_SpuWrite(0x182, 0x3FFF);
    Psyz_SpuWrite(0x1AA, 0xC000); /* SPUCNT: on, not muted */
    FILE *f = fopen(out, "wb");
    if (!f) { free(ram); return 1; }
    const int frames = 3 * 44100;
    uint32_t data = (uint32_t)frames * 4, riff = 36 + data, sr = 44100, br = 44100 * 4, fmt = 16;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    static const unsigned pitches[3] = {0, 0x1000, 0x0800};
    for (int pass = 0; pass < 2; pass++) { /* the special for 1 s, then the engine for 2 */
        int k = pass ? KIND_ENGINE : KIND_SPECIAL;
        Psyz_SpuWrite(0x18C, 1); /* key off */
        Psyz_SpuWrite(0x00, 0x3FFF);
        Psyz_SpuWrite(0x02, 0x3FFF);
        Psyz_SpuWrite(0x04, (unsigned short)pitches[k]);
        Psyz_SpuWrite(0x06, 0x1000); /* (some sample of the game's: the bank's plays instead) */
        Psyz_SpuWrite(0x08, 0x000F); /* ADSR: at once to full, held */
        Psyz_SpuWrite(0x0A, 0x1FC0);
        Psyz_SpuSetVoiceBank(0, ram, start[k][k == KIND_ENGINE], scale_for(rate[k], pitches[k]));
        Psyz_SpuWrite(0x188, 1); /* key on */
        short buf[2 * 441];
        for (int i = 0; i < (pass ? 200 : 100); i++) {
            Psyz_SpuPullSamples(buf, 441);
            fwrite(buf, 4, 441, f);
        }
    }
    fclose(f);
    free(ram);
    return 0;
}
