"""Compute the driven route on the Overture road graph, following the dark (selected) route of
the Google Maps screenshot.

Edges cost length x (1 + penalty), where the penalty grows with the distance between the edge
and the dark route pixels of the georeferenced screenshot: Dijkstra then naturally follows the
line Google drew, while still obeying one-way streets and car access.
"""
import heapq
import math
import json

import numpy as np
import shapely.wkb as wkb
from scipy import ndimage

import overture
import georef

BBOX = georef.BBOX
START_ADDR = (5.069327, 43.551357)   # 2000 Chemin Départemental 15, 13250 (Overture addresses)
PIN_PX = (538.0, 47.0)               # tip of the red destination pin in the screenshot

CAR_CLASSES = {
    "motorway": 1.0, "trunk": 1.0, "primary": 1.0, "secondary": 1.0, "tertiary": 1.0,
    "unclassified": 1.1, "residential": 1.15, "living_street": 1.5, "service": 1.6, "unknown": 1.6,
}
CLASS_CODE = {"service": 0, "unknown": 0, "unclassified": 0, "living_street": 1, "residential": 1, "tertiary": 2,
              "secondary": 3, "primary": 4, "trunk": 5, "motorway": 6}

R_EARTH = 6371008.8


def hav(a, b):
    lon1, lat1 = map(math.radians, a)
    lon2, lat2 = map(math.radians, b)
    dlat, dlon = lat2 - lat1, lon2 - lon1
    h = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    return 2 * R_EARTH * math.asin(math.sqrt(h))


def car_access(row):
    """Return (forward_ok, backward_ok)."""
    fwd = bwd = True
    for a in row["access_restrictions"] or []:
        w = a["when"] or {}
        modes = w.get("mode")
        if a["between"] is not None:
            continue
        if w.get("vehicle") or w.get("during") or w.get("using") or w.get("recognized"):
            continue
        applies = modes is None or "motor_vehicle" in modes or "car" in modes or "vehicle" in modes
        if not applies:
            continue
        if a["access_type"] == "denied":
            h = w.get("heading")
            if h == "backward":
                bwd = False
            elif h == "forward":
                fwd = False
            else:
                fwd = bwd = False
        elif a["access_type"] == "allowed" and modes and "motor_vehicle" in modes:
            fwd = bwd = True
    return fwd, bwd


def value_at(rules, t, key):
    """Linear-referenced rule lookup (between=[a,b] fractions)."""
    for r in rules or []:
        b = r.get("between")
        if b is None or (b[0] - 1e-9 <= t <= b[1] + 1e-9):
            return r[key] if key in r else r
    return None


def split_line(coords, cuts):
    """Split a polyline (list of lon/lat) at length fractions `cuts` (sorted, incl. 0 and 1)."""
    c = np.array(coords)
    seg_len = np.array([hav(c[i], c[i + 1]) for i in range(len(c) - 1)])
    cum = np.concatenate([[0], np.cumsum(seg_len)])
    total = cum[-1] if cum[-1] > 0 else 1e-9

    def point_at(f):
        d = f * total
        i = int(np.searchsorted(cum, d, side="right") - 1)
        i = min(max(i, 0), len(c) - 2)
        t = (d - cum[i]) / (seg_len[i] or 1e-9)
        return c[i] + (c[i + 1] - c[i]) * t, i

    parts = []
    for a, b in zip(cuts[:-1], cuts[1:]):
        pa, ia = point_at(a)
        pb, ib = point_at(b)
        pts = [pa] + [c[k] for k in range(ia + 1, ib + 1)] + [pb]
        # drop duplicates
        out = [pts[0]]
        for p in pts[1:]:
            if np.hypot(*(p - out[-1])) > 1e-9:
                out.append(p)
        if len(out) == 1:
            out.append(out[0])
        parts.append((a, b, out))
    return parts, total


def build_graph():
    seg = overture.fetch("transportation", "segment", BBOX)
    rows = seg.select(["id", "subtype", "class", "subclass", "connectors", "access_restrictions", "road_flags",
                       "speed_limits", "width_rules", "level_rules", "routes", "names", "geometry"]).to_pylist()
    edges = []          # dict per directed edge
    for row in rows:
        if row["subtype"] != "road" or row["class"] not in CAR_CLASSES:
            continue
        fwd, bwd = car_access(row)
        if not (fwd or bwd):
            continue
        g = wkb.loads(row["geometry"])
        coords = list(g.coords)
        cons = sorted(row["connectors"] or [], key=lambda c: c["at"])
        cuts = [c["at"] for c in cons]
        ids = [c["connector_id"] for c in cons]
        if not cuts or cuts[0] > 1e-6:
            cuts = [0.0] + cuts; ids = ["%s@0" % row["id"]] + ids
        if cuts[-1] < 1 - 1e-6:
            cuts = cuts + [1.0]; ids = ids + ["%s@1" % row["id"]]
        parts, total = split_line(coords, cuts)
        for k, (a, b, pts) in enumerate(parts):
            length = sum(hav(pts[i], pts[i + 1]) for i in range(len(pts) - 1))
            info = dict(seg=row, a=a, b=b, pts=pts, length=length, seg_len=total)
            if fwd:
                edges.append(dict(u=ids[k], v=ids[k + 1], pts=pts, info=info, reverse=False))
            if bwd:
                edges.append(dict(u=ids[k + 1], v=ids[k], pts=pts[::-1], info=info, reverse=True))
    return edges


def _ring_props(pieces_ll):
    """Area / perimeter / centroid of a closed ring given as consecutive lon/lat polylines."""
    pts = []
    for pl in pieces_ll:
        pts.extend(pl if not pts else pl[1:])
    lat0 = pts[0][1]
    k = math.cos(math.radians(lat0)) * 111320.0
    xy = np.array([((p[0] - pts[0][0]) * k, (p[1] - pts[0][1]) * 110540.0) for p in pts])
    x, y = xy[:, 0], xy[:, 1]
    area = 0.5 * abs(np.dot(x[:-1], y[1:]) - np.dot(x[1:], y[:-1]))
    seg = np.hypot(np.diff(x), np.diff(y))
    per = float(np.sum(seg))
    # dense resampling (1 m) -> radius variation around the area centroid
    cum = np.concatenate([[0], np.cumsum(seg)])
    t = np.arange(0, per, 1.0)
    dx, dy = np.interp(t, cum, x), np.interp(t, cum, y)
    cx, cy = dx.mean(), dy.mean()
    rr = np.hypot(dx - cx, dy - cy)
    cv = float(rr.std() / max(rr.mean(), 1e-6))
    c = (pts[0][0] + cx / k, pts[0][1] + cy / 110540.0)
    return area, per, c, cv


def mark_roundabouts(edges):
    """Detect roundabout rings (small, nearly circular cycles of road pieces) and keep only the
    counter-clockwise direction (France: right-hand traffic). Returns the set of id(info) on rings.

    Overture loses OSM's implicit oneway of junction=roundabout, so this is reconstructed."""
    infos = {}
    und = {}
    for e in edges:
        info = e["info"]
        if id(info) in infos:
            continue
        # canonical endpoints (u, v) in the info's own orientation
        u, v = (e["u"], e["v"]) if not e["reverse"] else (e["v"], e["u"])
        infos[id(info)] = (info, u, v)
        und.setdefault(u, []).append((id(info), v))
        und.setdefault(v, []).append((id(info), u))
    ring = {}
    for iid, (info, u, v) in infos.items():
        if info["length"] > 150 or iid in ring:
            continue
        # DFS for a short cycle v -> ... -> u not using this piece
        best = None
        stack = [(v, [iid], info["length"])]
        while stack:
            node, path, dist = stack.pop()
            if node == u and len(path) > 1:
                if best is None or dist < best[1]:
                    best = (path, dist)
                continue
            if dist > 320 or len(path) > 10:
                continue
            for jid, nxt in und.get(node, []):
                if jid in path:
                    continue
                jinfo = infos[jid][0]
                stack.append((nxt, path + [jid], dist + jinfo["length"]))
        if not best or best[1] > 320 or len(best[0]) < 2:
            continue
        # orient the pieces as a chain starting at u -> v
        chain = []
        node = u
        ok = True
        for jid in best[0]:
            jinfo, ju, jv = infos[jid]
            pts = [tuple(p) for p in jinfo["pts"]]
            if ju == node:
                chain.append(pts); node = jv
            elif jv == node:
                chain.append(pts[::-1]); node = ju
            else:
                ok = False; break
        if not ok:
            continue
        area, per, c, cv = _ring_props(chain)
        if per < 35 or per > 320 or 4 * math.pi * area / (per * per) < 0.85 or cv > 0.06:
            continue
        if any(infos[j][0]["seg"]["class"] in ("service", "unknown") for j in best[0]):
            continue
        for jid in best[0]:
            ring[jid] = c
    # keep only counter-clockwise directed edges on rings
    keep = []
    for e in edges:
        c = ring.get(id(e["info"]))
        if c is None:
            keep.append(e)
            continue
        pts = e["pts"]
        mid = len(pts) // 2
        a, b = np.array(pts[max(mid - 1, 0)]), np.array(pts[min(mid + 1, len(pts) - 1)])
        if np.allclose(a, b):
            a, b = np.array(pts[0]), np.array(pts[-1])
        rx, ry = a[0] - c[0], a[1] - c[1]
        dx, dy = b[0] - a[0], b[1] - a[1]
        if rx * dy - ry * dx > 0:      # counter-clockwise in the east/north plane
            e["roundabout"] = True
            keep.append(e)
    return keep, set(ring.keys())


def route():
    geo = georef.fit()
    dark, _ = georef.masks()
    dist = ndimage.distance_transform_edt(~dark)
    H, W = dist.shape

    def dpx(lon, lat):
        x, y = geo.to_px(lon, lat)
        xi, yi = int(round(x)), int(round(y))
        if 0 <= xi < W and 0 <= yi < H:
            return dist[yi, xi]
        return 60.0

    edges, ring_ids = mark_roundabouts(build_graph())
    print("roundabout pieces:", len(ring_ids))
    adj = {}
    for e in edges:
        pts = e["pts"]
        # sample distance to the dark route every ~15 m
        ds = []
        for i in range(len(pts) - 1):
            n = max(1, int(hav(pts[i], pts[i + 1]) / 15))
            for t in np.linspace(0, 1, n, endpoint=False):
                p = np.array(pts[i]) + (np.array(pts[i + 1]) - np.array(pts[i])) * t
                ds.append(dpx(*p))
        ds.append(dpx(*pts[-1]))
        m = float(np.mean(ds))
        pen = 0.0 if m < 2.5 else (m - 2.5) ** 2 * 0.8
        cls = e["info"]["seg"]["class"]
        e["cost"] = e["info"]["length"] * CAR_CLASSES[cls] * (1 + pen) + 0.5
        adj.setdefault(e["u"], []).append(e)

    # snap start / end to the nearest point of a drivable edge, by inserting virtual nodes
    pin = geo.to_lonlat(*PIN_PX)

    def nearest_edge(p, prefer=None, exclude=("service", "unknown")):
        best = None
        for e in edges:
            if e["info"]["seg"]["class"] in exclude:
                continue
            pts = e["pts"]
            for i in range(len(pts) - 1):
                a, b = np.array(pts[i]), np.array(pts[i + 1])
                ab = b - a
                cosl = math.cos(math.radians(p[1]))
                abm = ab * [cosl, 1]
                apm = (np.array(p) - a) * [cosl, 1]
                t = float(np.clip(np.dot(apm, abm) / (np.dot(abm, abm) or 1e-12), 0, 1))
                q = a + ab * t
                d = hav(p, q)
                if best is None or d < best[0]:
                    best = (d, e, i, t, q)
        return best

    s_hit = nearest_edge(START_ADDR)
    e_hit = nearest_edge(pin, exclude=())
    print("start snap %.1f m on %s" % (s_hit[0], (s_hit[1]["info"]["seg"]["names"] or {}).get("primary")))
    print("end snap %.1f m on %s (%s)" % (e_hit[0], (e_hit[1]["info"]["seg"]["names"] or {}).get("primary"), e_hit[1]["info"]["seg"]["class"]))

    # virtual start: S -> e.v along the remainder of the edge (and the reverse edge if two-way)
    def tail_from(hit):
        d, e, i, t, q = hit
        return [tuple(q)] + [tuple(p) for p in e["pts"][i + 1:]]

    def head_to(hit):
        d, e, i, t, q = hit
        return [tuple(p) for p in e["pts"][: i + 1]] + [tuple(q)]

    starts = []
    for e in [s_hit[1]] + [x for x in edges if x["info"] is s_hit[1]["info"] and x is not s_hit[1]]:
        if e is s_hit[1]:
            starts.append((e["v"], tail_from(s_hit), e))
        else:
            # reverse direction of the same piece: from snap point to its v
            pts = e["pts"]
            k = len(pts) - 2 - s_hit[2]
            starts.append((e["v"], [tuple(s_hit[4])] + [tuple(p) for p in pts[k + 1:]], e))
    ends = {}
    for e in [e_hit[1]] + [x for x in edges if x["info"] is e_hit[1]["info"] and x is not e_hit[1]]:
        if e is e_hit[1]:
            ends[e["u"]] = (head_to(e_hit), e)
        else:
            pts = e["pts"]
            k = len(pts) - 2 - e_hit[2]
            ends[e["u"]] = ([tuple(p) for p in pts[: k + 1]] + [tuple(e_hit[4])], e)

    # Dijkstra
    distc = {}
    prev = {}
    pq = []
    for node, pts, e in starts:
        c = 0.0
        distc[node] = c
        prev[node] = ("START", pts, e)
        heapq.heappush(pq, (c, node))
    best_end = None
    while pq:
        c, u = heapq.heappop(pq)
        if c > distc.get(u, 1e18):
            continue
        if u in ends and (best_end is None or c < best_end[0]):
            best_end = (c, u)
        for e in adj.get(u, []):
            nc = c + e["cost"]
            if nc < distc.get(e["v"], 1e18):
                distc[e["v"]] = nc
                prev[e["v"]] = (u, e["pts"], e)
                heapq.heappush(pq, (nc, e["v"]))
    assert best_end, "no route found"
    # rebuild
    u = best_end[1]
    pieces = [(ends[u][0], ends[u][1])]
    while True:
        p = prev[u]
        pieces.append((p[1], p[2]))
        if p[0] == "START":
            break
        u = p[0]
    pieces.reverse()
    route.ring_ids = ring_ids
    route.edges = edges
    return geo, pieces, START_ADDR, pin


def piece_attrs(e, frac_mid):
    seg = e["info"]["seg"]
    a, b = e["info"]["a"], e["info"]["b"]
    t = a + (b - a) * frac_mid
    flags = set()
    for f in seg["road_flags"] or []:
        bt = f["between"]
        if bt is None or bt[0] <= t <= bt[1]:
            flags.update(f["values"])
    sl = None
    for r in seg["speed_limits"] or []:
        bt = r["between"]
        if (bt is None or bt[0] <= t <= bt[1]) and r.get("max_speed") and not r.get("when"):
            sl = r["max_speed"]["value"]
            if r["max_speed"].get("unit") == "mph":
                sl = int(sl * 1.609)
    width = None
    for r in seg["width_rules"] or []:
        bt = r["between"]
        if bt is None or bt[0] <= t <= bt[1]:
            width = r["value"]
    level = 0
    for r in seg["level_rules"] or []:
        bt = r["between"]
        if bt is None or bt[0] <= t <= bt[1]:
            level = r["value"]
    ref = None
    for r in seg["routes"] or []:
        if r.get("ref") and (r.get("network") or "").startswith("FR"):
            ref = r["ref"].replace(" ", "")
            break
    name = (seg["names"] or {}).get("primary")
    fwd, bwd = car_access(seg)
    return dict(cls=seg["class"], subclass=seg["subclass"], flags=flags, speed=sl, width=width, level=level,
                ref=ref, name=name, oneway=not (fwd and bwd), seg_id=seg["id"])


if __name__ == "__main__":
    geo, pieces, s, e = route()
    total = 0
    prev_lbl = None
    for pts, ed in pieces:
        L = sum(hav(pts[i], pts[i + 1]) for i in range(len(pts) - 1))
        total += L
        at = piece_attrs(ed, 0.5)
        lbl = (at["ref"], at["name"], at["cls"])
        if lbl != prev_lbl:
            print("%7.0f m  %-8s %-10s %-40s oneway=%s flags=%s speed=%s" % (total, at["ref"], at["cls"], at["name"], at["oneway"], sorted(at["flags"]), at["speed"]))
            prev_lbl = lbl
    print("TOTAL %.0f m" % total)
