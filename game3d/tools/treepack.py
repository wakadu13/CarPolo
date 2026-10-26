#!/usr/bin/env python3
"""Packs low-poly tree models (OBJ + per-model PNG textures) into assets/trees.bin + assets/trees.png.

    tools/treepack.py <pack.obj> <texture dir> <out dir> [tile px]

trees.bin: "CPT2", uint32 n, then per model { char name[32]; float height; float radius; uint32 nVerts;
struct { int16 x, y, z; int8 nx, ny, nz, pad; uint16 u, v; } v[nVerts] } — triangle lists, positions in
units of height/32767 (so a model is 0..32767 tall), base at y = 0, centred on x/z, +Y up; uv / 65535.
trees.png: atlas of the textures (4 x 4 tiles), uvs already remapped into it.
"""
import os, struct, sys
import numpy as np
from PIL import Image

obj_path, tex_dir, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
tile = int(sys.argv[4]) if len(sys.argv) > 4 else 512
os.makedirs(out_dir, exist_ok=True)

# --- materials -> texture file
mtl = {}
cur = None
with open(os.path.splitext(obj_path)[0] + ".mtl") as f:
    for line in f:
        t = line.split()
        if not t: continue
        if t[0] == "newmtl": cur = t[1]
        elif t[0] == "map_Kd" and cur: mtl[cur] = os.path.basename(t[-1])

# --- obj
V, VT, VN = [], [], []
groups = []          # (name, material, [faces])
gname, gmat = "default", None
with open(obj_path) as f:
    for line in f:
        t = line.split()
        if not t: continue
        if t[0] == "v": V.append(tuple(map(float, t[1:4])))
        elif t[0] == "vt": VT.append((float(t[1]), float(t[2])))
        elif t[0] == "vn": VN.append(tuple(map(float, t[1:4])))
        elif t[0] in ("g", "o"): gname = t[1] if len(t) > 1 else gname; groups.append([gname, gmat, []])
        elif t[0] == "usemtl":
            gmat = t[1]
            if not groups or groups[-1][2]: groups.append([gname, gmat, []])
            else: groups[-1][1] = gmat
        elif t[0] == "f":
            if not groups: groups.append([gname, gmat, []])
            idx = []
            for w in t[1:]:
                p = (w.split("/") + ["", ""])[:3]
                idx.append((int(p[0]) - 1, int(p[1]) - 1 if p[1] else -1, int(p[2]) - 1 if p[2] else -1))
            for k in range(1, len(idx) - 1): groups[-1][2].append((idx[0], idx[k], idx[k + 1]))
groups = [g for g in groups if g[2]]
# materials without map_Kd: the pack names its textures after the materials
for g in groups:
    if g[1] and g[1] not in mtl and os.path.exists(os.path.join(tex_dir, g[1] + ".png")): mtl[g[1]] = g[1] + ".png"

# --- atlas
texs = sorted(set(mtl[g[1]] for g in groups if g[1] in mtl))
cols = 4
rows = (len(texs) + cols - 1) // cols
atlas = Image.new("RGBA", (cols * tile, rows * tile), (0, 0, 0, 0))
slot = {}
for i, name in enumerate(texs):
    im = Image.open(os.path.join(tex_dir, name)).convert("RGBA").resize((tile, tile), Image.LANCZOS)
    atlas.paste(im, ((i % cols) * tile, (i // cols) * tile))
    slot[name] = (i % cols, i // cols)
atlas.save(os.path.join(out_dir, "trees.png"), optimize=True)

# --- models
V = np.array(V, np.float32); VT = np.array(VT, np.float32) if VT else np.zeros((1, 2), np.float32)
VN = np.array(VN, np.float32) if VN else np.zeros((1, 3), np.float32)
out = bytearray(b"CPT2" + struct.pack("<I", len(groups)))
for name, mat, faces in groups:
    tri = np.array(faces, np.int32)                       # (n, 3, 3)
    pos = V[tri[:, :, 0]].reshape(-1, 3)
    uv = VT[np.maximum(tri[:, :, 1], 0)].reshape(-1, 2)
    nrm = VN[np.maximum(tri[:, :, 2], 0)].reshape(-1, 3)
    # normalise: base on the ground, centred in x/z
    mn, mx = pos.min(0), pos.max(0)
    pos = pos - np.array([(mn[0] + mx[0]) / 2, mn[1], (mn[2] + mx[2]) / 2], np.float32)
    height = float(mx[1] - mn[1]); radius = float(max(mx[0] - mn[0], mx[2] - mn[2]) / 2)
    texname = mtl.get(mat, "")
    sx, sy = slot.get(texname, (0, 0))
    u = (sx + np.clip(uv[:, 0] % 1.0, 0.002, 0.998)) / cols
    v = (sy + np.clip(1.0 - (uv[:, 1] % 1.0), 0.002, 0.998)) / rows     # OBJ v is bottom-up, PNG top-down
    q = np.clip(np.round(pos / height * 32767), -32767, 32767).astype(np.int16)
    nq = np.clip(np.round(nrm * 127), -127, 127).astype(np.int8)
    uvq = np.clip(np.round(np.stack([u, v], 1) * 65535), 0, 65535).astype(np.uint16)
    rec = np.zeros(len(pos), dtype=[("p", np.int16, 3), ("n", np.int8, 4), ("t", np.uint16, 2)])
    rec["p"] = q; rec["n"][:, :3] = nq; rec["t"] = uvq
    label = (name + "|" + texname)[:31]
    out += label.encode().ljust(32, b"\0") + struct.pack("<ffI", height, radius, len(rec)) + rec.tobytes()
    print(f"{name:16s} {texname:14s} verts {len(rec):6d} height {height:6.2f} radius {radius:5.2f}")
open(os.path.join(out_dir, "trees.bin"), "wb").write(out)
print("atlas", atlas.size, "models", len(groups), "bytes", len(out))
