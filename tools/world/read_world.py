"""Self-test: parse game3d/assets/world.bin back (DESIGN.md format) and print statistics.

python3 read_world.py [path]
"""
import os
import struct
import sys
import collections

import numpy as np

DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "game3d", "assets", "world.bin")
CLASS = ["service/unclassified", "residential", "tertiary", "secondary", "primary", "trunk", "motorway", "motorway_link"]


def read(path):
    data = open(path, "rb").read()
    assert data[:4] == b"CPW1", "bad magic"
    version, lat0, lon0 = struct.unpack_from("<Idd", data, 4)
    bbox = struct.unpack_from("<ffff", data, 24)
    off = 40
    secs = collections.OrderedDict()
    while off < len(data):
        tag = data[off:off + 4].decode()
        n = struct.unpack_from("<I", data, off + 4)[0]
        secs[tag] = data[off + 8: off + 8 + n]
        off += 8 + n
    assert off == len(data), "trailing bytes"
    out = dict(version=version, origin=(lat0, lon0), bbox=bbox, sections={k: len(v) for k, v in secs.items()})

    b = secs["STRS"]; n = struct.unpack_from("<I", b)[0]; o = 4; strs = []
    for _ in range(n):
        L = struct.unpack_from("<H", b, o)[0]; strs.append(b[o + 2:o + 2 + L].decode("utf-8")); o += 2 + L
    out["strings"] = strs

    b = secs["ROUT"]; n = struct.unpack_from("<I", b)[0]
    assert len(b) == 4 + 28 * n
    dt = np.dtype([("x", "<f4"), ("y", "<f4"), ("z", "<f4"), ("width", "<f4"), ("lanes", "u1"), ("lanesForward", "u1"),
                   ("roadClass", "u1"), ("flags", "u1"), ("speed", "<u2"), ("nameId", "<u2"), ("placeId", "<u2"), ("pad", "<u2")])
    out["route"] = np.frombuffer(b, dt, n, 4)

    def grid(tag, typ):
        b = secs[tag]
        x0, z0, cell, nx, nz = struct.unpack_from("<fffII", b)
        a = np.frombuffer(b, typ, nx * nz, 20).reshape(nz, nx)
        assert len(b) == 20 + a.nbytes
        return dict(x0=x0, z0=z0, cell=cell, nx=nx, nz=nz, a=a)

    out["dem"] = grid("DEM ", "<i2"); out["lcov"] = grid("LCOV", "u1")
    out["far"] = grid("FAR ", "<i2"); out["farc"] = grid("FARC", "u1")

    b = secs["BLDG"]; n = struct.unpack_from("<I", b)[0]; o = 4; bl = []
    for _ in range(n):
        nv, kind, roof, h, mh = struct.unpack_from("<HBBff", b, o); o += 12
        ring = np.frombuffer(b, "<f4", nv * 2, o).reshape(nv, 2); o += nv * 8
        bl.append((kind, roof, h, mh, ring))
    assert o == len(b)
    out["buildings"] = bl

    b = secs["ROAD"]; n = struct.unpack_from("<I", b)[0]; o = 4; rd = []
    for _ in range(n):
        np_, cls, lanes, width, flags = struct.unpack_from("<HBBfB3x", b, o); o += 12
        pts = np.frombuffer(b, "<f4", np_ * 2, o).reshape(np_, 2); o += np_ * 8
        rd.append((cls, lanes, width, flags, pts))
    assert o == len(b)
    out["roads"] = rd

    b = secs["TREE"]; n = struct.unpack_from("<I", b)[0]
    out["trees"] = np.frombuffer(b, np.dtype([("x", "<f4"), ("z", "<f4"), ("type", "u1"), ("pad", "3u1")]), n, 4)
    b = secs["PLAC"]; n = struct.unpack_from("<I", b)[0]
    out["places"] = np.frombuffer(b, np.dtype([("x", "<f4"), ("z", "<f4"), ("nameId", "<u2"), ("kind", "u1"), ("pad", "u1")]), n, 4)
    b = secs["ZONE"]; n = struct.unpack_from("<I", b)[0]
    out["zones"] = np.frombuffer(b, np.dtype([("s", "<f4"), ("nameId", "<u2"), ("subId", "<u2")]), n, 4)
    return out


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    w = read(path)
    S = w["strings"]
    name = lambda i: "-" if i == 0xFFFF else S[i]
    print("file %s: %d bytes, version %d, origin lat/lon %.6f %.6f" % (path, os.path.getsize(path), w["version"], *w["origin"]))
    print("bbox x %.0f..%.0f  z %.0f..%.0f" % (w["bbox"][0], w["bbox"][2], w["bbox"][1], w["bbox"][3]))
    for k, v in w["sections"].items():
        print("  %s %9d bytes" % (k, v))
    r = w["route"]
    seg = np.hypot(np.diff(r["x"]), np.diff(r["z"]))
    s = np.concatenate([[0], np.cumsum(seg)])
    grade = np.diff(r["y"]) / np.maximum(seg, 1e-3)
    print("route: %d points, %.0f m, step %.2f..%.2f m, elevation %.1f..%.1f m, max grade %.1f %%"
          % (len(r), s[-1], seg.min(), seg.max(), r["y"].min(), r["y"].max(), 100 * np.abs(grade).max()))
    # run-length summary of attributes
    key = lambda i: (r["roadClass"][i], r["flags"][i], r["speed"][i], r["nameId"][i], r["placeId"][i], round(float(r["width"][i]), 1), r["lanes"][i])
    start = 0
    for i in range(1, len(r) + 1):
        if i == len(r) or key(i) != key(start):
            k = key(start)
            fl = k[1]
            print("  %6.0f-%6.0f m %-20s w=%.1f lanes=%d %3d km/h %-28s %-18s %s" % (
                s[start], s[i - 1], CLASS[k[0]], k[5], k[6], k[2], name(k[3]), name(k[4]),
                " ".join(f for b, f in [(1, "oneway"), (2, "roundabout"), (4, "bridge"), (8, "tunnel"), (16, "built-up")] if fl & b)))
            start = i
    d = w["dem"]
    print("DEM %dx%d cell %.0f  h %.1f..%.1f m" % (d["nx"], d["nz"], d["cell"], d["a"].min() / 10, d["a"].max() / 10))
    lc = w["lcov"]
    print("LCOV %dx%d classes %s" % (lc["nx"], lc["nz"], np.bincount(lc["a"].ravel(), minlength=13).tolist()))
    f = w["far"]
    print("FAR %dx%d cell %.0f  h %.1f..%.1f m" % (f["nx"], f["nz"], f["cell"], f["a"].min() / 10, f["a"].max() / 10))
    b = w["buildings"]
    print("BLDG %d, kinds %s, roofs %s, height %.1f..%.1f, vertices %d" % (
        len(b), dict(collections.Counter(x[0] for x in b)), dict(collections.Counter(x[1] for x in b)),
        min(x[2] for x in b), max(x[2] for x in b), sum(len(x[4]) for x in b)))
    rd = w["roads"]
    print("ROAD %d, classes %s" % (len(rd), dict(collections.Counter(x[0] for x in rd))))
    print("TREE %d types %s" % (len(w["trees"]), dict(collections.Counter(w["trees"]["type"].tolist()))))
    print("PLAC %d: %s" % (len(w["places"]), ", ".join(name(p) for p in w["places"]["nameId"])))
    for z in w["zones"]:
        print("ZONE %6.0f m  %s — %s" % (z["s"], name(z["nameId"]), name(z["subId"])))


if __name__ == "__main__":
    main()
