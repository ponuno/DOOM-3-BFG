#!/usr/bin/env python3
"""
gpuhang.py: decodes sdmc:/doom3/gpuhang.bin, written by the 3DS port when the GPU does
not finish a frame (renderer/3DS/n3ds_gpu.c, N3DSGPU_HangDump).

    python3 gpuhang.py gpuhang.bin [-v]

Prints the GX queue (which entry the GPU is stuck on), every draw of the frame's
command list(s) with the state the PICA reads for it (vertex / index buffers,
texture units, lighting, combiners) and checks it: addresses outside VRAM / FCRAM,
textures that run past the end of memory, units sampling a disabled or impossible
setup. With the registers read at the time of the hang, the draws whose state
matches them are named: the GPU is stuck in one of those.

Draw numbers are those of the "its N draws" list in log.txt (one PICA draw per
logged draw).
"""
import struct
import sys

VRAM = (0x18000000, 0x18600000)
FCRAM = (0x20000000, 0x30000000)		# New 3DS: 256 MB

TEX_FORMATS = ["RGBA8", "RGB8", "RGBA5551", "RGB565", "RGBA4", "LA8", "HILO8", "L8",
			   "A8", "LA4", "L4", "A4", "ETC1", "ETC1A4"]
TEX_BPP = [32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8]

GX_NAMES = {0: "DMA", 1: "command list", 2: "memory fill", 3: "display transfer", 4: "texture copy", 5: "cache flush"}


def in_mem(a, size=0):
	for lo, hi in (VRAM, FCRAM):
		if lo <= a and a + size <= hi:
			return True
	return False


def read_dump(path):
	data = open(path, "rb").read()
	pos = 0

	def u32():
		nonlocal pos
		v = struct.unpack_from("<I", data, pos)[0]
		pos += 4
		return v

	magic, version = u32(), u32()
	if magic != 0x4847334E or version != 1:
		sys.exit("not a gpuhang.bin of version 1")
	lin_virt, lin_phys, lin_size, num, cur, last = (u32() for _ in range(6))
	entries = []
	for _ in range(num):
		entries.append(struct.unpack_from("<8I", data, pos))
		pos += 32
	lists = {}
	while pos + 4 <= len(data):
		tag = u32()
		if tag == 0x444E4521:		# !END
			break
		if tag != 0x4C444D43:
			sys.exit("bad section %08x at %d" % (tag, pos - 4))
		idx, size = u32(), u32()
		lists[idx] = struct.unpack_from("<%dI" % (size // 4), data, pos)
		pos += size
	regs, ext = {}, {}
	if pos + 4 <= len(data) and struct.unpack_from("<I", data, pos)[0] == 0x53474552:
		pos += 4
		while pos + 4 <= len(data):
			r = u32()
			if r == 0xFFFFFFFF:
				break
			v = u32()
			if r & 0x10000:
				ext[r & 0xFFFF] = v
			else:
				regs[r] = v
	return dict(lin_virt=lin_virt, lin_phys=lin_phys, lin_size=lin_size, num=num, cur=cur, last=last,
				entries=entries, lists=lists, regs=regs, ext=ext)


def virt_to_phys(d, a):
	if d["lin_virt"] <= a < d["lin_virt"] + d["lin_size"]:
		return a - d["lin_virt"] + d["lin_phys"]
	if 0x1F000000 <= a < 0x1F600000:
		return a - 0x1F000000 + 0x18000000
	return None


def decode_list(words):
	"""yields (register, value, mask) in order; ('draw', reg) at draw triggers"""
	i = 0
	n = len(words)
	while i + 1 < n:
		param, header = words[i], words[i + 1]
		reg = header & 0xFFFF
		mask = (header >> 16) & 0xF
		extra = (header >> 20) & 0x7FF
		cons = header >> 31
		values = [param] + list(words[i + 2:i + 2 + extra])
		i += 2 + extra
		if i & 1:
			i += 1
		bm = 0
		for b in range(4):
			if mask & (1 << b):
				bm |= 0xFF << (8 * b)
		for k, v in enumerate(values):
			yield (reg + k if cons else reg), v, bm


def tex_bytes(fmt, w, h, levels):
	if fmt >= len(TEX_BPP):
		return 0
	total = 0
	for _ in range(levels):
		total += w * h * TEX_BPP[fmt] // 8
		w, h = max(w // 2, 1), max(h // 2, 1)
	return total


class State:
	def __init__(self):
		self.r = {}

	def get(self, reg):
		return self.r.get(reg, 0)

	def write(self, reg, v, bm):
		self.r[reg] = (self.get(reg) & ~bm) | (v & bm)


TEX_UNITS = [  # dim, param, lod, addr, type
	(0x082, 0x083, 0x084, 0x085, 0x08E),
	(0x092, 0x093, 0x094, 0x095, 0x096),
	(0x09A, 0x09B, 0x09C, 0x09D, 0x09E),
]


def draw_state(s, indexed):
	st = {}
	base = s.get(0x200) << 3
	st["attr_base"] = base
	bufs = []
	for b in range(12):
		off = s.get(0x203 + 3 * b)
		c2 = s.get(0x205 + 3 * b)
		ncomp = c2 >> 28
		stride = (c2 >> 16) & 0xFF
		if ncomp:
			bufs.append((b, base + off, stride, ncomp, s.get(0x204 + 3 * b), c2 & 0xFFFF))
	st["buffers"] = bufs
	st["num_attrs"] = (s.get(0x202) >> 28) + 1
	ib = s.get(0x227)
	st["index"] = (base + (ib & 0x0FFFFFFF), 16 if ib >> 31 else 8) if indexed else None
	st["count"] = s.get(0x228)
	cfg = s.get(0x080)
	units = []
	for u, (dim, param, lod, addr, typ) in enumerate(TEX_UNITS):
		enabled = (cfg >> u) & 1
		dv = s.get(dim)
		w, h = (dv >> 16) & 0x7FF, dv & 0x7FF
		lv = s.get(lod)
		max_level = (lv >> 16) & 0xF
		units.append(dict(enabled=enabled, w=w, h=h, fmt=s.get(typ) & 0xF, addr=s.get(addr) << 3,
						  levels=max_level + 1, param=s.get(param)))
	st["tex"] = units
	st["tex_config"] = cfg
	st["lighting"] = s.get(0x08F) & 1
	st["num_lights"] = (s.get(0x1C2) & 7) + 1
	st["light_cfg0"] = s.get(0x1C3)
	st["light_cfg1"] = s.get(0x1C4)
	st["light_perm"] = s.get(0x1D9)
	st["combiners"] = [tuple(s.get(r + k) for k in range(5)) for r in (0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8)]
	st["buffer_cfg"] = s.get(0x0E0)
	st["vsh_inputs"] = (s.get(0x2B9) & 0xF) + 1
	st["vsh_outmask"] = s.get(0x2BD)
	return st


def check(st):
	problems = []
	if not in_mem(st["attr_base"]):
		problems.append("attribute base %08x outside memory" % st["attr_base"])
	for b, addr, stride, ncomp, _, _ in st["buffers"]:
		if not in_mem(addr):
			problems.append("buffer %d at %08x outside memory" % (b, addr))
		if stride == 0:
			problems.append("buffer %d stride 0" % b)
	if st["index"] is not None:
		addr, bits = st["index"]
		if not in_mem(addr, st["count"] * bits // 8):
			problems.append("index buffer %08x + %d indexes runs outside memory" % (addr, st["count"]))
		if bits == 16 and addr & 1:
			problems.append("16-bit index buffer at an odd address %08x" % addr)
	if st["count"] == 0:
		problems.append("0 vertexes")
	for u, t in enumerate(st["tex"]):
		if not t["enabled"]:
			continue
		if t["w"] < 8 or t["h"] < 8 or t["w"] > 1024 or t["h"] > 1024 or t["w"] & (t["w"] - 1) or t["h"] & (t["h"] - 1):
			problems.append("unit %d: size %dx%d" % (u, t["w"], t["h"]))
		if t["fmt"] >= len(TEX_FORMATS):
			problems.append("unit %d: format %d" % (u, t["fmt"]))
			continue
		size = tex_bytes(t["fmt"], t["w"], t["h"], t["levels"])
		if not in_mem(t["addr"], size):
			problems.append("unit %d: %s %dx%d %d lv at %08x (+%d) outside memory" % (
				u, TEX_FORMATS[t["fmt"]], t["w"], t["h"], t["levels"], t["addr"], size))
		if t["addr"] & 7:
			problems.append("unit %d: address %08x not 8-aligned" % (u, t["addr"]))
		if t["levels"] > 1 and (min(t["w"], t["h"]) >> (t["levels"] - 1)) < 8:
			problems.append("unit %d: %d levels go below 8 pixels (%dx%d)" % (u, t["levels"], t["w"], t["h"]))
	if st["lighting"]:
		bump_mode = (st["light_cfg0"] >> 28) & 3
		bump_sel = (st["light_cfg0"] >> 22) & 3
		if bump_mode and not ((st["tex_config"] >> bump_sel) & 1):
			problems.append("bump map from unit %d, which is disabled" % bump_sel)
	return problems


def describe(st):
	out = []
	bufs = ", ".join("b%d %08x stride %d x%d" % (b, a, s, n) for b, a, s, n, _, _ in st["buffers"])
	idx = ("index %08x (%d-bit)" % st["index"]) if st["index"] else "arrays"
	out.append("%d vertexes, %s, attrs %d (vsh inputs %d), %s" % (st["count"], idx, st["num_attrs"], st["vsh_inputs"], bufs))
	tex = []
	for u, t in enumerate(st["tex"]):
		if t["enabled"]:
			f = TEX_FORMATS[t["fmt"]] if t["fmt"] < len(TEX_FORMATS) else "?%d" % t["fmt"]
			tex.append("u%d %s %dx%d %dlv @%08x" % (u, f, t["w"], t["h"], t["levels"], t["addr"]))
	out.append("textures: " + (", ".join(tex) if tex else "none"))
	if st["lighting"]:
		cfg0 = st["light_cfg0"]
		out.append("lighting: %d lights, perm %08x, config0 %08x (bump mode %d from unit %d), config1 %08x" % (
			st["num_lights"], st["light_perm"], cfg0, (cfg0 >> 28) & 3, (cfg0 >> 22) & 3, st["light_cfg1"]))
	return out


def main():
	if len(sys.argv) < 2:
		sys.exit(__doc__)
	verbose = "-v" in sys.argv
	d = read_dump(sys.argv[1])
	print("GX queue: %d entries, %d submitted, %d finished" % (d["num"], d["cur"], d["last"]))
	for i, e in enumerate(d["entries"]):
		state = "done" if i < d["last"] else ("STUCK" if i == d["last"] else ("running" if i < d["cur"] else "waiting"))
		name = GX_NAMES.get(e[0] & 0xFF, "?")
		extra = ""
		if (e[0] & 0xFF) == 4:
			src, dst = virt_to_phys(d, e[1]), virt_to_phys(d, e[2])
			extra = " %08x -> %08x, %d bytes%s" % (e[1], e[2], e[3], "" if src and dst else " (address not in linear / VRAM!)")
			if e[3] & 15:
				extra += " (size not a multiple of 16!)"
		elif (e[0] & 0xFF) == 1:
			extra = " %08x, %d bytes" % (e[1], e[2])
		print("  %2d %-7s %s%s" % (i, state, name, extra))

	draws = []
	for idx in sorted(d["lists"]):
		s = State()
		for reg, v, bm in decode_list(d["lists"][idx]):
			if reg in (0x22E, 0x22F):
				st = draw_state(s, reg == 0x22F)
				st["list"] = idx
				draws.append(st)
			else:
				s.write(reg, v, bm)
	print("\n%d draws in the command list(s)" % len(draws))
	for n, st in enumerate(draws):
		problems = check(st)
		if verbose or problems:
			print("draw %d:" % n)
			for line in describe(st):
				print("    " + line)
			for p in problems:
				print("    PROBLEM: " + p)

	if d["regs"]:
		r = d["regs"]
		print("\nregisters at the hang:")
		ext = d["ext"]
		if 0x34 in ext:
			print("  GPU status %08x" % ext[0x34])
		s = State()
		for reg, v in r.items():
			s.write(reg, v, 0xFFFFFFFF)
		now = draw_state(s, True)
		for line in describe(now):
			print("    " + line)
		matches = []
		for n, st in enumerate(draws):
			score = 0
			score += st["attr_base"] == now["attr_base"]
			score += st["count"] == now["count"]
			score += (st["index"] is not None and now["index"] is not None and st["index"][0] == now["index"][0])
			score += sum(1 for a, b in zip(st["tex"], now["tex"]) if a["enabled"] and a["addr"] == b["addr"])
			matches.append((score, n))
		matches.sort(reverse=True)
		best = matches[0][0] if matches else 0
		print("  draws matching the registers best (score %d): %s" % (best, ", ".join(str(n) for sc, n in matches if sc == best)))


if __name__ == "__main__":
	main()
