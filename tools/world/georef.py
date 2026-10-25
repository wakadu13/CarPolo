"""Georeference the Google Maps screenshot (Web Mercator crop, no rotation).

pixel x = s * lon + cx ;  pixel y = -s * mercY(lat) + cy   (mercY in "degrees")
Initial fit from locality labels, refined by trimmed ICP between the screenshot's road/route
pixels and Overture road geometry.
"""
import math
import os

import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.spatial import cKDTree
import shapely.wkb as wkb

import overture

SHOT = os.environ.get("CARPOLO_SCREENSHOT",
                      "/tmp/claude-0/-home-user-CarPolo/d1d9fb6e-3b18-57b2-82b7-a90f72455094/images/1.png")
BBOX = (5.0, 43.53, 5.2, 43.66)

# label centre in the screenshot -> Overture locality point
LABELS = {
    "Grans": ((268, 176), (5.06338, 43.60676)),
    "Cornillon-Confoux": ((318, 519), (5.0723, 43.56191)),
    "Miramas-le-Vieux": ((55, 512), (5.02442, 43.56323)),
    "La Barben": ((893, 5), (5.17407, 43.62848)),
}


def mercy(lat):
    return math.degrees(math.log(math.tan(math.pi / 4 + math.radians(lat) / 2)))


def inv_mercy(my):
    return math.degrees(2 * math.atan(math.exp(math.radians(my))) - math.pi / 2)


class Georef:
    def __init__(self, s, cx, cy):
        self.s, self.cx, self.cy = s, cx, cy

    def to_px(self, lon, lat):
        return self.s * lon + self.cx, -self.s * mercy(lat) + self.cy

    def to_lonlat(self, px, py):
        lon = (px - self.cx) / self.s
        return lon, inv_mercy((self.cy - py) / self.s)


DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")


def masks():
    """(dark selected-route pixels, light road/route pixels) of the screenshot.

    Uses the original screenshot when present, else the binary masks saved in data/
    (derived line masks only, no map imagery)."""
    if os.path.exists(SHOT):
        im = np.array(Image.open(SHOT).convert("RGB")).astype(int)
        r, g, b = im[..., 0], im[..., 1], im[..., 2]
        dark = (b > 150) & (r < 120) & (g < 70)
        light = (b > 200) & (r > 100) & (r < 190) & (g > 100) & (g < 190) & (b - r > 40)
    else:
        dark = np.array(Image.open(os.path.join(DATA, "google_dark_route_mask.png")).convert("L")) > 127
        light = np.array(Image.open(os.path.join(DATA, "google_light_lines_mask.png")).convert("L")) > 127
    dark_thick = ndimage.binary_opening(dark, iterations=2)
    return dark_thick, light


def road_points(seg, classes=None, step_px=None):
    pts = []
    for row in seg.select(["subtype", "class", "geometry"]).to_pylist():
        if row["subtype"] != "road":
            continue
        if classes and row["class"] not in classes:
            continue
        g = wkb.loads(row["geometry"])
        c = np.array(g.coords)
        # densify every ~5 m (in degrees ~ 5e-5)
        for i in range(len(c) - 1):
            a, bb = c[i], c[i + 1]
            n = max(1, int(np.hypot(*(bb - a)) / 5e-5))
            for t in np.linspace(0, 1, n, endpoint=False):
                pts.append(a + (bb - a) * t)
        pts.append(c[-1])
    return np.array(pts)


def fit():
    # initial guess from labels
    M, rhs = [], []
    for (px, py), (lon, lat) in LABELS.values():
        M.append([lon, 1, 0]); rhs.append(px)
        M.append([-mercy(lat), 0, 1]); rhs.append(py)
    s, cx, cy = np.linalg.lstsq(np.array(M), np.array(rhs), rcond=None)[0]
    seg = overture.fetch("transportation", "segment", BBOX)
    roads = road_points(seg)
    lonlat = roads
    my = np.array([mercy(v) for v in lonlat[:, 1]])
    dark, light = masks()
    ys, xs = np.nonzero(dark | light)
    P = np.stack([xs, ys], 1).astype(float)
    # ICP: transform roads into pixel space, match each screenshot pixel to nearest road pixel
    for it in range(30):
        rx = s * lonlat[:, 0] + cx
        ry = -s * my + cy
        tree = cKDTree(np.stack([rx, ry], 1))
        d, idx = tree.query(P)
        thr = max(2.5, 10 - it * 0.5)
        k = d < thr
        # solve for ds, dcx, dcy: px ≈ (s+ds)*lon + cx+dcx ; py ≈ -(s+ds)*my + cy+dcy
        lon_m = lonlat[idx[k], 0]; my_m = my[idx[k]]
        A = np.zeros((2 * k.sum(), 3)); bvec = np.zeros(2 * k.sum())
        A[0::2, 0] = lon_m; A[0::2, 1] = 1; bvec[0::2] = P[k, 0]
        A[1::2, 0] = -my_m; A[1::2, 2] = 1; bvec[1::2] = P[k, 1]
        s, cx, cy = np.linalg.lstsq(A, bvec, rcond=None)[0]
        if it % 5 == 4 or it == 29:
            print("icp", it, "matched", int(k.sum()), "/", len(P), "mean d", round(float(d[k].mean()), 2))
    return Georef(s, cx, cy)


if __name__ == "__main__":
    g = fit()
    print("s=%.3f cx=%.3f cy=%.3f" % (g.s, g.cx, g.cy))
    print("start", g.to_lonlat(305, 607), "pin", g.to_lonlat(538, 37))
    print("m per px", 40075016.7 * math.cos(math.radians(43.6)) / 360 / g.s)
