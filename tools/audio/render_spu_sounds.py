"""Render sounds from a dump of the PS1 sound RAM to WAV files, for listening.

    py tools/audio/render_spu_sounds.py SPU_DUMP OUT_DIR

The dump comes from the port: TM2PC_SPU_DUMP=file writes the sound RAM when
the menus first open (the title's sound bank loaded). Written to OUT_DIR:

- locked_*.wav: the menus' "not available" candidates, played as the port
  plays them (sample, pitch, ADSR envelope and volume as key_on sets them).
- sample_NNN_XXXXX.wav: every sample in the RAM (XXXXX its address / 8, as
  the voice registers name it), at 22050 Hz and at the menus' pitch.
- index.html: a page with a player for each.

The decoding and the envelope follow PSY-Z's SPU (third_party/psyz/psyz/src/
psyz/spu_voice.h); linear interpolation stands in for the Gaussian one.
"""
import struct
import sys
from pathlib import Path

RATE = 44100
POS = [0, 60, 115, 98, 122]
NEG = [0, 0, -52, -55, -60]


def clamp16(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def decode_sample(ram, addr, max_blocks=20000):
    """The sample's PCM (at its own rate), from addr (bytes) to its end block."""
    out, h1, h2 = [], 0, 0
    for _ in range(max_blocks):
        blk = ram[addr:addr + 16]
        if len(blk) < 16:
            break
        shift_in = blk[0] & 0xF
        shift = 9 if shift_in > 12 else 12 - shift_in
        f = min((blk[0] >> 4) & 7, 4)
        for i in range(14):
            byte = blk[2 + i]
            for n in (0, 4):
                t = (byte >> n) & 0xF
                t = t - 16 if t & 8 else t
                s = clamp16(t * (1 << shift) + ((h1 * POS[f]) >> 6) + ((h2 * NEG[f]) >> 6))
                out.append(s)
                h2, h1 = h1, s
        if blk[1] & 1:  # end (a repeat is not followed: the sound is heard once)
            break
        addr += 16
    return out


def denominator(rate):
    return 1 if rate < 48 else 1 << ((rate >> 2) - 11)


def num_inc(rate):
    return (7 - (rate & 3)) << (11 - (rate >> 2)) if rate < 48 else 7 - (rate & 3)


def num_dec(rate):
    return (-8 + (rate & 3)) * (1 << (11 - (rate >> 2))) if rate < 48 else -8 + (rate & 3)


def envelope(adsr1, adsr2, ticks, key_off_at=None):
    """The envelope level (0..0x7FFF) at each output tick."""
    vol, state, ctr, out = 0, 'A', 0, []
    for t in range(ticks):
        if t < 6:  # key-on latency
            out.append(0)
            continue
        if key_off_at is not None and t >= key_off_at and state != 'off':
            state = 'R'
        ctr += 1
        if state == 'A':
            rate = (adsr1 >> 8) & 0x7F
            if (adsr1 & 0x8000) and vol >= 0x6000:
                rate += 8
            if ctr % denominator(rate) == 0:
                vol += num_inc(rate)
                if vol >= 0x7FFF:
                    vol, state = 0x7FFF, 'D'
        elif state == 'D':
            rate = ((adsr1 >> 4) & 0xF) * 4
            if ctr % denominator(rate) == 0:
                vol = max(0, vol + ((num_dec(rate) * vol) >> 15))
                if ((vol >> 11) & 0xF) <= (adsr1 & 0xF):
                    state = 'S'
        elif state == 'S':
            rate = (adsr2 >> 6) & 0x7F
            dec, exp = (adsr2 >> 14) & 1, (adsr2 >> 15) & 1
            if not dec:
                if exp and vol >= 0x6000:
                    rate += 8
                if ctr % denominator(rate) == 0:
                    vol = min(0x7FFF, vol + num_inc(rate))
            elif ctr % denominator(rate) == 0:
                vol = max(0, vol + (((num_dec(rate) * vol) >> 15) if exp else num_dec(rate)))
        elif state == 'R':
            rate = (adsr2 & 0x1F) * 4
            if ctr % denominator(rate) == 0:
                vol = max(0, vol + (((num_dec(rate) * vol) >> 15) if (adsr2 >> 5) & 1 else num_dec(rate)))
                if vol == 0:
                    state = 'off'
        out.append(vol)
    return out


def play(ram, start, pitch, adsr1=0x80FF, adsr2=0x5FC0, vol=0x3FFF, seconds=2.0, delay=0):
    """One voice: the sample at start (address / 8) resampled by pitch (0x1000 = 44.1 kHz)."""
    pcm = decode_sample(ram, start * 8)
    step = (pitch & 0x3FFF) / 0x1000
    n = int(min(seconds * RATE, len(pcm) / step + 64))
    env = envelope(adsr1, adsr2, n)
    gain = ((vol & 0x7FFF) << 1) / 32768
    out = [0.0] * delay
    pos = 0.0
    for t in range(n):
        i = int(pos)
        if i + 1 >= len(pcm):
            break
        frac = pos - i
        s = pcm[i] * (1 - frac) + pcm[i + 1] * frac
        out.append(s * env[t] / 32768 * gain)
        pos += step
    return out


def mix(*voices):
    n = max(len(v) for v in voices)
    return [sum(v[i] for v in voices if i < len(v)) for i in range(n)]


def write_wav(path, samples):
    peak = max(1.0, max((abs(s) for s in samples), default=1.0))
    scale = min(1.0, 30000 / peak)  # only ever turned down, so levels compare
    data = b''.join(struct.pack('<h', int(s * scale)) for s in samples)
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' +
                struct.pack('<IHHIIHH', 16, 1, 1, RATE, RATE * 2, 2, 16) + b'data' + struct.pack('<I', len(data)) + data)


def samples_in(ram):
    """(start address / 8, blocks) of each sample: runs of blocks up to an end flag."""
    found, a = [], 0x1000  # below: the capture buffers
    while a < len(ram) - 16:
        start = a
        while a < len(ram) - 16 and not (ram[a + 1] & 1):
            a += 16
        a += 16
        blocks = (a - start) // 16
        if blocks >= 4 and any(ram[start:a]):
            found.append((start // 8, blocks))
    return found


# the menus' sounds (ui/options.c): sample, pitch, ADSR, volume
CURSOR = dict(start=0x1A70, pitch=0x050A, adsr1=0x80FF, adsr2=0x4E00, vol=0x1932)
SELECT = dict(start=0x1264, pitch=0x050A, adsr1=0x80FF, adsr2=0x5FC0, vol=0x274E)


def locked_candidates(ram):
    def snd(base, shift=0, vol_pct=100, **over):
        d = dict(base)
        d.update(over)
        d['pitch'] = d['pitch'] >> shift
        d['vol'] = d['vol'] * vol_pct // 100
        return d

    c = {
        'cursor (as the menus)': [snd(CURSOR)],
        'select (as the menus)': [snd(SELECT), snd(SELECT, 1, 50)],
        'locked: THUD (now)': [snd(CURSOR, 1)],
        'locked: CLUNK (fixed)': [snd(SELECT, 1, 80, adsr1=0x80A0)],
        'locked: THUMP (fixed)': [snd(CURSOR, 1), snd(SELECT, 1, 60, adsr1=0x80A0)],
        'locked: DOUBLE TAP': [snd(CURSOR, 1), dict(snd(CURSOR, 1), delay=int(0.09 * RATE))],
        'locked: LOW TAP': [snd(CURSOR, 0, 100, pitch=0x0380)],
        'locked: SHORT BOOM': [snd(SELECT, 0, 70, adsr1=0x80B0)],
    }
    return c


def main():
    ram = Path(sys.argv[1]).read_bytes()
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, voices in locked_candidates(ram).items():
        file = 'locked_' + ''.join(ch if ch.isalnum() else '_' for ch in name.lower()).strip('_') + '.wav'
        rendered = [play(ram, v['start'], v['pitch'], v['adsr1'], v['adsr2'], v['vol'], 1.5, v.get('delay', 0))
                    for v in voices]
        write_wav(out / file, mix(*rendered))
        rows.append((name, file))
    for k, (start, blocks) in enumerate(samples_in(ram)):
        for label, pitch in (('22050 Hz', 0x0800), ('menu pitch', 0x050A)):
            file = f'sample_{k:03d}_{start:05X}_{"22k" if pitch == 0x800 else "menu"}.wav'
            write_wav(out / file, play(ram, start, pitch, seconds=4.0))
            rows.append((f'sample {k} ({start:05X}, {blocks} blocks), {label}', file))
    html = ['<!doctype html><meta charset="utf-8"><title>TM2 menu sounds</title>',
            '<style>body{font:15px sans-serif;background:#111;color:#ddd;padding:16px}'
            'div{margin:6px 0}span{display:inline-block;width:360px}</style>',
            '<h1>TM2 menu sounds</h1><p>The locked candidates first, then every sample in the title\'s sound bank.</p>']
    for name, file in rows:
        html.append(f'<div><span>{name}</span><audio controls preload="none" src="{file}"></audio></div>')
    (out / 'index.html').write_text('\n'.join(html), encoding='utf-8')
    print(f'{len(rows)} sounds in {out}')


if __name__ == '__main__':
    main()
