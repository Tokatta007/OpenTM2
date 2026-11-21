"""Read files from a PlayStation disc image (raw 2352-byte sectors, .cue/.bin).

Only the data track is read: ISO 9660 with Mode 2 Form 1 sectors (2048 bytes
of user data at offset 24). Files that the file system places on the audio
tracks (TM2's *.DA music entries) are listed but cannot be read as data.
"""
import struct
from pathlib import Path

RAW = 2352
USER = 2048
USER_OFFSET = 24  # 12 sync + 4 header + 8 subheader (Mode 2)


class DiscFile:
    def __init__(self, path, lba, size):
        self.path = path  # e.g. "/SCREEN/CPFONT.TIM"
        self.lba = lba
        self.size = size

    def __repr__(self):
        return f"DiscFile({self.path!r}, lba={self.lba}, size={self.size})"


def find_data_track(image):
    """Return the .bin of track 1 for a .cue, or the path itself for a .bin."""
    image = Path(image)
    if image.suffix.lower() != ".cue":
        return image
    for line in image.read_text(errors="replace").splitlines():
        line = line.strip()
        if line.upper().startswith("FILE "):
            name = line[5:].rsplit(" ", 1)[0].strip().strip('"')
            return image.with_name(name)
    raise ValueError(f"no FILE entry in {image}")


class CdImage:
    def __init__(self, image):
        self.bin_path = find_data_track(image)
        self._f = open(self.bin_path, "rb")
        self.sectors = self.bin_path.stat().st_size // RAW
        pvd = self.read_sector(16)
        if pvd[1:6] != b"CD001":
            raise ValueError(f"{self.bin_path}: no ISO 9660 volume descriptor")
        root = pvd[156:156 + 34]
        self.files = []
        self._walk(struct.unpack_from("<I", root, 2)[0],
                   struct.unpack_from("<I", root, 10)[0], "/")
        self.files.sort(key=lambda f: f.path)

    def close(self):
        self._f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def read_sector(self, lba):
        self._f.seek(lba * RAW + USER_OFFSET)
        return self._f.read(USER)

    def read_extent(self, lba, size):
        out = bytearray()
        for i in range((size + USER - 1) // USER):
            out += self.read_sector(lba + i)
        return bytes(out[:size])

    def is_data(self, f):
        """True if the file lies on the data track (the .DA music does not)."""
        return f.lba + (f.size + USER - 1) // USER <= self.sectors

    def read(self, f):
        if isinstance(f, str):
            f = self.find(f)
        if not self.is_data(f):
            raise ValueError(f"{f.path} is on an audio track")
        return self.read_extent(f.lba, f.size)

    def find(self, path):
        path = path.upper()
        for f in self.files:
            if f.path == path:
                return f
        raise FileNotFoundError(path)

    def _walk(self, lba, size, prefix):
        data = self.read_extent(lba, size)
        i = 0
        while i < len(data):
            length = data[i]
            if length == 0:  # records never cross a sector; skip to the next
                i = (i // USER + 1) * USER
                continue
            rec = data[i:i + length]
            i += length
            name_len = rec[32]
            name = rec[33:33 + name_len]
            if name in (b"\0", b"\1"):
                continue
            name = name.decode("ascii", "replace").split(";")[0]
            elba = struct.unpack_from("<I", rec, 2)[0]
            esize = struct.unpack_from("<I", rec, 10)[0]
            if rec[25] & 2:
                self._walk(elba, esize, prefix + name + "/")
            else:
                self.files.append(DiscFile(prefix + name, elba, esize))


if __name__ == "__main__":
    import sys
    with CdImage(sys.argv[1]) as cd:
        for f in cd.files:
            kind = "data" if cd.is_data(f) else "audio"
            print(f"{f.lba:7d} {f.size:10d} {kind:5s} {f.path}")
