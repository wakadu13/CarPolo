"""Build game3d/assets/world.bin (format: game3d/DESIGN.md) from open data.

Sources (all anonymous HTTPS on AWS open data):
  * Overture Maps release OVERTURE_RELEASE (OSM-derived): roads, connectors, buildings, land use,
    land (trees, tree rows, forests, scrub), water, places/divisions, addresses.
  * Mapzen/AWS Terrarium elevation tiles (EU-DEM based in Europe) — terrain + road profile.
  * ESA WorldCover 10 m v200 (2021) — land cover base layer.

Run:  python3 build_world.py            (≈1–2 min with a warm cache)
"""
import math
import os
import struct
import sys
import hashlib
import collections

import numpy as np
import pyproj
import shapely
import shapely.wkb as wkb
from shapely.geometry import LineString, Point, Polygon, MultiPolygon
from shapely.ops import unary_union
from PIL import Image, ImageDraw
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import overture      # noqa: E402
import georef        # noqa: E402
import route as R    # noqa: E402
import elevation     # noqa: E402

OUT = os.path.join(HERE, "..", "..", "game3d", "assets", "world.bin")
PREVIEW = os.environ.get("CARPOLO_PREVIEW_DIR",
                         "/tmp/claude-0/-home-user-CarPolo/d1d9fb6e-3b18-57b2-82b7-a90f72455094/scratchpad/world")

STEP = 2.0            # route resampling (m)
MARGIN = 1600.0       # DEM/LCOV margin around the route (m)
CELL = 10.0           # DEM/LCOV cell (m)
FAR_SIZE = 40000.0    # far grid extent (m)
FAR_CELL = 150.0
BLDG_RADIUS = 600.0
ROAD_RADIUS = 1200.0
TREE_RADIUS = 1200.0

# DESIGN land-cover classes
UNKNOWN, FOREST, SHRUB, GRASS, CROP, VINE, ORCHARD, RESID, INDUS, BARE, WATER, WETLAND, PAVED = range(13)
WORLDCOVER = {10: FOREST, 20: SHRUB, 30: GRASS, 40: CROP, 50: RESID, 60: BARE, 70: BARE, 80: WATER,
              90: WETLAND, 95: WETLAND, 100: GRASS}


# ----------------------------------------------------------------------------- strings
class Strings:
    def __init__(self):
        self.items, self.index = [], {}

    def id(self, s):
        if s is None:
            return 0xFFFF
        if s not in self.index:
            self.index[s] = len(self.items)
            self.items.append(s)
        return self.index[s]


# ----------------------------------------------------------------------------- helpers
def gaussian_smooth(a, sigma, mode="nearest"):
    return ndimage.gaussian_filter1d(a, sigma, mode=mode) if sigma > 0 else a


def stable_rand(key, lo, hi):
    h = int(hashlib.md5(key.encode()).hexdigest()[:8], 16) / 0xFFFFFFFF
    return lo + (hi - lo) * h


def main():
    os.makedirs(PREVIEW, exist_ok=True)
    S = Strings()

    # ------------------------------------------------------------------ route on the road graph
    geo, pieces, start_addr, pin = R.route()
    edges_all = R.route.edges
    ring_infos = R.route.ring_ids

    lonlat, piece_of = [], []
    for k, (pts, e) in enumerate(pieces):
        for q in pts:
            if lonlat and abs(q[0] - lonlat[-1][0]) + abs(q[1] - lonlat[-1][1]) < 1e-10:
                continue
            lonlat.append(q)
            piece_of.append(k)
    lonlat = np.array(lonlat)
    origin = lonlat[0]
    proj = pyproj.Transformer.from_crs(
        "EPSG:4326",
        "+proj=tmerc +lat_0=%.8f +lon_0=%.8f +k=1 +x_0=0 +y_0=0 +ellps=WGS84 +units=m +no_defs" % (origin[1], origin[0]),
        always_xy=True)

    def fwd(lon, lat):
        e, n = proj.transform(np.asarray(lon), np.asarray(lat))
        return np.asarray(e), -np.asarray(n)        # game x, z

    def inv(x, z):
        lon, lat = proj.transform(np.asarray(x), -np.asarray(z), direction="INVERSE")
        return np.asarray(lon), np.asarray(lat)

    px, pz = fwd(lonlat[:, 0], lonlat[:, 1])
    seglen = np.hypot(np.diff(px), np.diff(pz))
    cum = np.concatenate([[0], np.cumsum(seglen)])
    total = cum[-1]
    ss = np.arange(0, total, STEP)
    if total - ss[-1] > 0.5:
        ss = np.append(ss, total)
    rx = np.interp(ss, cum, px)
    rz = np.interp(ss, cum, pz)
    # piece index of each sample (segment that contains s)
    seg_idx = np.clip(np.searchsorted(cum, ss, side="right") - 1, 0, len(piece_of) - 2)
    rpiece = np.array([piece_of[i + 1] if piece_of[i] != piece_of[i + 1] else piece_of[i] for i in seg_idx])
    # light planimetric smoothing (keeps roundabouts / real corners, removes polyline kinks)
    sx = gaussian_smooth(rx, 1.5); sz = gaussian_smooth(rz, 1.5)
    sx[0], sz[0], sx[-1], sz[-1] = rx[0], rz[0], rx[-1], rz[-1]
    rx, rz = sx, sz
    rlon, rlat = inv(rx, rz)
    print("route: %.0f m, %d samples" % (total, len(ss)))

    # ------------------------------------------------------------------ bounding boxes
    minX, maxX = rx.min() - MARGIN, rx.max() + MARGIN
    minZ, maxZ = rz.min() - MARGIN, rz.max() + MARGIN
    x0 = math.floor(minX / CELL) * CELL
    z0 = math.floor(minZ / CELL) * CELL
    nx = int(math.ceil((maxX - x0) / CELL)) + 1
    nz = int(math.ceil((maxZ - z0) / CELL)) + 1
    gx = x0 + np.arange(nx) * CELL
    gz = z0 + np.arange(nz) * CELL
    GX, GZ = np.meshgrid(gx, gz)
    glon, glat = inv(GX.ravel(), GZ.ravel())
    llbox = (glon.min() - 0.003, glat.min() - 0.003, glon.max() + 0.003, glat.max() + 0.003)
    print("grid %dx%d cells of %.0f m, lon/lat box %s" % (nx, nz, CELL, tuple(round(v, 4) for v in llbox)))

    # ------------------------------------------------------------------ elevation
    terr = elevation.Terrarium(llbox, z=14)
    dem = terr.sample(glon, glat).reshape(nz, nx)
    dem = ndimage.gaussian_filter(dem, 0.8)        # remove resampling noise of the 25 m source

    # ------------------------------------------------------------------ land cover
    lcov = landcover(llbox, glon, glat, nx, nz, x0, z0, fwd)
    dem = np.where((lcov == WATER) & (dem < 0), 0.0, dem)

    # ------------------------------------------------------------------ route attributes
    ry_raw = terr.sample(rlon, rlat)
    ry = gaussian_smooth(ry_raw, 15.0)          # ≈30 m sigma
    attrs = [R.piece_attrs(e, 0.5) for _, e in pieces]
    # bridges: keep the deck straight between its ends
    bridge = np.array([("is_bridge" in attrs[p]["flags"]) for p in rpiece])
    tunnel = np.array([("is_tunnel" in attrs[p]["flags"]) for p in rpiece])
    for mask in (bridge, tunnel):
        lab, n = ndimage.label(mask)
        for k in range(1, n + 1):
            idx = np.nonzero(lab == k)[0]
            a, b = max(idx[0] - 1, 0), min(idx[-1] + 1, len(ry) - 1)
            ry[a:b + 1] = np.linspace(ry[a], ry[b], b - a + 1)
    # grade limit 8 %
    for _ in range(3):
        for i in range(1, len(ry)):
            d = ry[i] - ry[i - 1]
            lim = 0.08 * STEP
            if abs(d) > lim:
                ry[i] = ry[i - 1] + math.copysign(lim, d)
        for i in range(len(ry) - 2, -1, -1):
            d = ry[i] - ry[i + 1]
            lim = 0.08 * STEP
            if abs(d) > lim:
                ry[i] = ry[i + 1] + math.copysign(lim, d)

    # built-up: residential/industrial cells around the route, or a mapped 50 km/h limit
    built = (lcov == RESID) | (lcov == INDUS)
    bfrac = ndimage.uniform_filter(built.astype(np.float32), size=13)     # 130 m window
    ii = np.clip(((rx - x0) / CELL).round().astype(int), 0, nx - 1)
    jj = np.clip(((rz - z0) / CELL).round().astype(int), 0, nz - 1)
    rbuilt = bfrac[jj, ii] > 0.33
    sp_data = np.array([attrs[p]["speed"] or 0 for p in rpiece])
    rbuilt |= (sp_data > 0) & (sp_data <= 50)
    # remove short flickers (< 150 m)
    rbuilt = ndimage.binary_closing(rbuilt, structure=np.ones(40))
    rbuilt = ndimage.binary_opening(rbuilt, structure=np.ones(75))

    # communes
    areas = overture.fetch("divisions", "division_area", llbox)
    communes = []
    for r in areas.select(["subtype", "names", "geometry"]).to_pylist():
        if r["subtype"] == "locality":
            communes.append((r["names"]["primary"], wkb.loads(r["geometry"])))
    rplace = []
    last = None
    for lon, lat in zip(rlon, rlat):
        p = Point(lon, lat)
        if last is not None and last[1].contains(p):
            rplace.append(last[0]); continue
        last = None
        for nm, g in communes:
            if g.contains(p):
                last = (nm, g); break
        rplace.append(last[0] if last else None)

    # smooth commune flicker where the road follows a commune limit (majority over ±300 m)
    ids = {n: k for k, n in enumerate(sorted(set(x for x in rplace if x)))}
    arr = np.array([ids.get(x, -1) for x in rplace])
    sm = arr.copy()
    W = 300
    for i in range(len(arr)):
        win = arr[max(0, i - W): i + W + 1]
        win = win[win >= 0]
        if len(win):
            sm[i] = np.bincount(win).argmax()
    inv_ids = {k: n for n, k in ids.items()}
    rplace = [inv_ids.get(int(v)) for v in sm]

    route_recs = []
    road_names = []
    for i in range(len(ss)):
        a = attrs[rpiece[i]]
        e = pieces[rpiece[i]][1]
        is_ring = id(e["info"]) in ring_infos or bool(e.get("roundabout"))
        cls_code = R.CLASS_CODE.get(a["cls"], 0)
        if "is_link" in a["flags"] and a["cls"] == "motorway":
            cls_code = 7
        oneway = a["oneway"] or is_ring
        b = bool(rbuilt[i])
        if a["width"]:
            width = float(a["width"])
        elif is_ring:
            width = 7.0
        elif oneway:
            width = 4.5 if b else 5.5
        else:
            width = {"secondary": 6.5, "primary": 7.0, "tertiary": 5.6, "unclassified": 4.8,
                     "residential": 5.0, "living_street": 4.0}.get(a["cls"], 4.0)
        lanes = 1 if oneway else 2
        fwd_l = 1
        flags = (1 if oneway else 0) | (2 if is_ring else 0) | (4 if bridge[i] else 0) | (8 if tunnel[i] else 0) | (16 if b else 0)
        spd = a["speed"] or (30 if is_ring else (50 if b else 80))
        if is_ring:
            spd = min(spd, 30)
        name = a["name"]
        if i * STEP < 1100 and (name is None or name == "Route de Lançon"):
            name = name or "Route de Lançon"
        road_names.append(name)
        route_recs.append((rx[i], ry[i], rz[i], width, lanes, fwd_l, cls_code, flags, int(spd),
                           S.id(name), S.id(rplace[i])))

    # ------------------------------------------------------------------ zones (HUD banners)
    zones = make_zones(S, geo, fwd, rx, rz, ss, rbuilt, rplace, road_names, total)

    # ------------------------------------------------------------------ buildings / roads / trees / places
    route_line = LineString(np.stack([rx, -rz], 1))         # east/north plane
    bldg = buildings(llbox, fwd, route_line)
    roads = other_roads(edges_all, pieces, fwd, route_line, llbox, ring_infos)
    trees = map_trees(llbox, fwd, route_line)
    places = make_places(S, geo, fwd, llbox, x0, z0, nx, nz)

    # ------------------------------------------------------------------ far grid
    far, farc, fx0, fz0, fn = far_grid(fwd, inv, rx, rz)

    # ------------------------------------------------------------------ write
    write(OUT, origin, (x0, z0, x0 + (nx - 1) * CELL, z0 + (nz - 1) * CELL), S, route_recs,
          (x0, z0, CELL, nx, nz, dem), (x0, z0, CELL, nx, nz, lcov),
          (fx0, fz0, FAR_CELL, fn, fn, far), (fx0, fz0, FAR_CELL, fn, fn, farc),
          bldg, roads, trees, places, zones)
    print("wrote", os.path.abspath(OUT), os.path.getsize(OUT), "bytes")

    previews(geo, fwd, rx, rz, ry, ry_raw, ss, dem, lcov, x0, z0, nx, nz, bldg, roads, trees, places, zones, S)


# ----------------------------------------------------------------------------- land cover
def landcover(llbox, glon, glat, nx, nz, x0, z0, fwd):
    import rasterio
    from rasterio.windows import from_bounds
    os.environ.setdefault("CURL_CA_BUNDLE", "/etc/ssl/certs/ca-certificates.crt")
    if os.environ.get("HTTPS_PROXY"):
        os.environ.setdefault("GDAL_HTTP_PROXY", os.environ["HTTPS_PROXY"])
    cache = os.path.expanduser("~/.cache/carpolo-world/worldcover_%.4f_%.4f_%.4f_%.4f.npz" % llbox)
    if os.path.exists(cache):
        z = np.load(cache)
        a, tr = z["a"], tuple(z["tr"])
    else:
        url = ("/vsicurl/https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/"
               "ESA_WorldCover_10m_2021_v200_N42E003_Map.tif")
        with rasterio.open(url) as ds:
            w = from_bounds(*llbox, ds.transform)
            a = ds.read(1, window=w)
            t = ds.window_transform(w)
            tr = (t.a, t.c, t.e, t.f)
        np.savez_compressed(cache, a=a, tr=np.array(tr))
    ta, tc, te, tf = tr
    ci = np.clip(((glon - tc) / ta).astype(int), 0, a.shape[1] - 1)
    cj = np.clip(((glat - tf) / te).astype(int), 0, a.shape[0] - 1)
    wc = a[cj, ci].reshape(nz, nx)
    lut = np.zeros(256, np.uint8)
    for k, v in WORLDCOVER.items():
        lut[k] = v
    lc = lut[wc]

    # rasterise Overture polygons in grid pixels
    def grid_poly(geom):
        out = []
        polys = [geom] if geom.geom_type == "Polygon" else list(getattr(geom, "geoms", []))
        for pg in polys:
            if pg.geom_type != "Polygon":
                continue
            ex = np.array(pg.exterior.coords)
            x, z = fwd(ex[:, 0], ex[:, 1])
            out.append(list(zip((x - x0) / CELL, (z - z0) / CELL)))
        return out

    def paint(geoms, value, allowed=None, width=None):
        img = Image.new("L", (nx, nz), 0)
        d = ImageDraw.Draw(img)
        for g in geoms:
            if g.geom_type in ("LineString", "MultiLineString"):
                lines = [g] if g.geom_type == "LineString" else list(g.geoms)
                for ln in lines:
                    c = np.array(ln.coords)
                    x, z = fwd(c[:, 0], c[:, 1])
                    d.line(list(zip((x - x0) / CELL, (z - z0) / CELL)), fill=1, width=max(1, int(round((width or CELL) / CELL))))
            else:
                for ring in grid_poly(g):
                    if len(ring) >= 3:
                        d.polygon(ring, fill=1)
        m = np.asarray(img).astype(bool)
        if allowed is not None:
            m &= np.isin(lc, list(allowed))
        lc[m] = value

    lu = overture.fetch("base", "land_use", llbox)
    land = overture.fetch("base", "land", llbox)
    water = overture.fetch("base", "water", llbox)
    by = collections.defaultdict(list)
    for r in lu.select(["subtype", "class", "geometry"]).to_pylist():
        by[("lu", r["subtype"], r["class"])].append(wkb.loads(r["geometry"]))
    for r in land.select(["subtype", "class", "geometry"]).to_pylist():
        by[("land", r["subtype"], r["class"])].append(wkb.loads(r["geometry"]))
    for r in water.select(["subtype", "class", "geometry"]).to_pylist():
        by[("water", r["subtype"], r["class"])].append(wkb.loads(r["geometry"]))

    def g(*keys):
        out = []
        for k in keys:
            out += by.get(k, [])
        return out

    open_land = {GRASS, CROP, SHRUB, UNKNOWN, BARE}
    paint(g(("lu", "agriculture", "farmland"), ("lu", "agriculture", "farmyard")), CROP, allowed=open_land | {RESID})
    paint(g(("lu", "agriculture", "meadow"), ("lu", "managed", "grass"), ("lu", "park", "park"),
            ("lu", "golf", "green"), ("lu", "golf", "tee"), ("lu", "recreation", "pitch"),
            ("lu", "park", "village_green")), GRASS, allowed=open_land | {RESID})
    paint(g(("land", "forest", "forest"), ("land", "forest", "wood")), FOREST, allowed=open_land)
    paint(g(("land", "shrub", "scrub"), ("land", "shrub", "heath"), ("land", "shrub", "shrubbery")), SHRUB,
          allowed={GRASS, UNKNOWN, BARE, CROP})
    paint(g(("land", "rock", "bare_rock"), ("land", "rock", "stone")), BARE)
    paint(g(("lu", "horticulture", "vineyard")), VINE, allowed=open_land | {FOREST, RESID})
    paint(g(("lu", "horticulture", "orchard")), ORCHARD, allowed=open_land | {FOREST, RESID})
    paint(g(("lu", "residential", "residential"), ("lu", "horticulture", "garden"), ("lu", "education", "school"),
            ("lu", "cemetery", "cemetery")), RESID, allowed={GRASS, CROP, UNKNOWN, BARE, RESID, SHRUB})
    paint(g(("lu", "developed", "industrial"), ("lu", "developed", "retail"), ("lu", "developed", "commercial"),
            ("lu", "construction", "construction")), INDUS, allowed={GRASS, CROP, UNKNOWN, BARE, RESID, SHRUB})
    poly_water = [x for k, v in by.items() if k[0] == "water" and k[2] not in ("swimming_pool",)
                  for x in v if x.geom_type in ("Polygon", "MultiPolygon")]
    paint(poly_water, WATER)
    paint(g(("water", "canal", "canal"), ("water", "river", "river")), WATER, width=12)
    print("land cover histogram", np.bincount(lc.ravel(), minlength=13))
    return lc


# ----------------------------------------------------------------------------- zones
def make_zones(S, geo, fwd, rx, rz, ss, rbuilt, rplace, names, total):
    zones = [(0.0, "D15 · La Pécette", "2000 chemin départemental 15")]

    def s_near(px, py):
        lon, lat = geo.to_lonlat(px, py)
        x, z = fwd(lon, lat)
        d = np.hypot(rx - x, rz - z)
        return float(ss[int(np.argmin(d))]), float(d.min())

    # communes along the route
    prev = rplace[0]
    def road_ahead(i):
        for k in range(i, min(len(ss), i + 400)):
            if names[k]:
                return names[k]
        return ""

    announced = {rplace[0]}
    last_name = "Route de Lançon"
    for i in range(1, len(ss)):
        if names[i]:
            last_name = names[i]
        if rplace[i] != prev and rplace[i] and rplace[i] not in announced:
            announced.add(rplace[i])
            zones.append((float(ss[i]), rplace[i], road_ahead(i) or last_name))
        prev = rplace[i]
    # lieux-dits read on the Google screenshot
    s_gb, d_gb = s_near(440, 410)
    if d_gb < 400:
        zones.append((s_gb, "Les Grandes Bastides", "Route de Lançon"))
    # entering Lançon's built-up area
    for i in range(1, len(ss)):
        if rbuilt[i] and not rbuilt[i - 1] and rplace[i] == "Lançon-Provence":
            zones.append((float(ss[i]), "Lançon-Provence", "Traversée du village"))
            break
    # road changes after Lançon
    seen = set()
    for i in range(1, len(ss)):
        n = names[i]
        if n in ("Route de Pélissanne", "Chemin de Saint-Jean-en-Crau") and n not in seen and not rbuilt[i]:
            seen.add(n)
            sub = "Direction Pélissanne" if n == "Route de Pélissanne" else "Vers l’A54"
            zones.append((float(ss[i]), n, sub))
    zones.append((max(0.0, total - 250.0), "Arrivée", "Près de l’A54"))
    zones.sort()
    # one banner per title (keep the last one: e.g. the village entry rather than the commune limit)
    last_of = {}
    for i, z in enumerate(zones):
        last_of[z[1]] = i
    zones = [z for i, z in enumerate(zones) if last_of[z[1]] == i]
    # drop banners closer than 250 m to the previous one (keep the later, more specific one)
    out = []
    for z in zones:
        if out and z[0] - out[-1][0] < 250 and z[1] != "Arrivée" and out[-1][0] > 0:
            out[-1] = z
        else:
            out.append(z)
    for z in out:
        print("zone %6.0f m  %s — %s" % z)
    return [(s, S.id(t), S.id(sub)) for s, t, sub in out]


# ----------------------------------------------------------------------------- buildings
KIND = {
    "house": 0, "detached": 0, "semidetached_house": 0, "terrace": 0, "bungalow": 0, "residential": 0, "farmhouse": 0,
    "apartments": 1, "dormitory": 1, "hotel": 1,
    "industrial": 2, "warehouse": 2, "commercial": 2, "retail": 2, "office": 2, "supermarket": 2, "hangar": 2,
    "service": 2, "school": 2, "hospital": 2, "civic": 2, "public": 2, "sports_hall": 2, "kindergarten": 2,
    "farm": 3, "barn": 3, "farm_auxiliary": 3, "greenhouse": 3, "stable": 3, "cowshed": 3, "silo": 3,
    "church": 4, "chapel": 4, "cathedral": 4, "religious": 4, "temple": 4, "mosque": 4,
}
SUBTYPE_KIND = {"residential": 0, "agricultural": 3, "commercial": 2, "industrial": 2, "education": 2,
                "civic": 2, "service": 2, "religious": 4, "transportation": 2, "outbuilding": 5}
ROOF = {"flat": 0, "hipped": 1, "pyramidal": 1, "gabled": 2}


def buildings(llbox, fwd, route_line):
    t = overture.fetch("buildings", "building", llbox)
    zone = route_line.buffer(BLDG_RADIUS)
    out = []
    for r in t.select(["id", "subtype", "class", "height", "min_height", "num_floors", "roof_shape",
                       "is_underground", "geometry"]).to_pylist():
        if r["is_underground"]:
            continue
        g = wkb.loads(r["geometry"])
        polys = [g] if g.geom_type == "Polygon" else [p for p in getattr(g, "geoms", []) if p.geom_type == "Polygon"]
        for pg in polys:
            ex = np.array(pg.exterior.coords)
            x, z = fwd(ex[:, 0], ex[:, 1])
            P = Polygon(np.stack([x, -z], 1))
            if not P.is_valid:
                P = P.buffer(0)
                if P.geom_type != "Polygon":
                    continue
            if P.area < 12 or not zone.intersects(P.centroid):
                continue
            P = shapely.geometry.polygon.orient(P.simplify(0.3, preserve_topology=True), 1.0)
            ring = np.array(P.exterior.coords)[:-1]
            if len(ring) < 3 or len(ring) > 200:
                continue
            cls = r["class"]
            kind = KIND.get(cls, SUBTYPE_KIND.get(r["subtype"], None))
            area = P.area
            if kind is None:
                kind = 0 if area < 400 else 2
            if cls in ("garage", "garages", "shed", "carport", "roof", "shelter", "hut"):
                kind = 5
            h = r["height"]
            if not h and r["num_floors"]:
                h = r["num_floors"] * 3.0 + 1.0
            if not h:
                key = r["id"]
                if kind == 5 or area < 30:
                    h = stable_rand(key, 2.6, 3.4)
                elif kind == 0:
                    h = stable_rand(key, 5.6, 7.6) if area > 60 else stable_rand(key, 3.2, 4.2)
                elif kind == 1:
                    h = stable_rand(key, 9, 13)
                elif kind == 2:
                    h = stable_rand(key, 6.5, 9.5)
                elif kind == 3:
                    h = stable_rand(key, 4, 6)
                elif kind == 4:
                    h = 11.0
                else:
                    h = 4.0
            h = float(min(max(h, 2.2), 45.0))
            roof = ROOF.get(r["roof_shape"], 3)
            if roof == 3 and kind == 2 and area > 300:
                roof = 0          # big warehouses / shops: flat roofs
            out.append((kind, roof, h, float(r["min_height"] or 0.0), [(float(a), float(-b)) for a, b in ring]))
    print("buildings:", len(out), collections.Counter(b[0] for b in out))
    return out


# ----------------------------------------------------------------------------- other roads
ROAD_W = {"motorway": 11.0, "trunk": 9.0, "primary": 7.0, "secondary": 6.5, "tertiary": 5.6, "unclassified": 4.8,
          "residential": 5.0, "living_street": 4.0, "service": 3.5, "unknown": 3.5, "track": 3.0}


def other_roads(edges, pieces, fwd, route_line, llbox, ring_ids=frozenset()):
    used = set()
    partial = {}
    for k, (pts, e) in enumerate(pieces):
        used.add(id(e["info"]))
    # parts of the first/last pieces that are not driven
    first_e, last_e = pieces[0][1], pieces[-1][1]
    zone = route_line.buffer(ROAD_RADIUS)
    seen = set()
    out = []

    def add(pts_ll, a, info_flags, cls, oneway):
        c = np.array(pts_ll)
        x, z = fwd(c[:, 0], c[:, 1])
        ln = LineString(np.stack([x, -z], 1))
        if not zone.intersects(ln):
            return
        ln = ln.simplify(0.5)
        coords = np.array(ln.coords)
        if len(coords) < 2:
            return
        flags = ((1 if oneway else 0) | (2 if "roundabout" in info_flags else 0) |
                 (4 if "is_bridge" in info_flags else 0) | (8 if "is_tunnel" in info_flags else 0))
        if cls == "track":
            flags |= 32
        code = R.CLASS_CODE.get(cls, 0)
        if cls == "motorway" and "is_link" in info_flags:
            code = 7
        lanes = 3 if (cls == "motorway" and "is_link" not in info_flags) else (1 if (oneway or cls in ("service", "track")) else 2)
        width = ROAD_W.get(cls, 4.0)
        if code == 7:
            width = 5.5
        out.append((code, lanes, width, flags, [(float(p[0]), float(-p[1])) for p in coords]))

    for e in edges:
        info = e["info"]
        if id(info) in seen:
            continue
        seen.add(id(info))
        at = R.piece_attrs(e, 0.5)
        pts = info["pts"]
        if id(info) in used:
            continue
        if id(info) in ring_ids:
            # store the ring piece in its counter-clockwise driving direction
            add(e["pts"], at, set(at["flags"]) | {"roundabout"}, at["cls"], True)
            continue
        add(pts, at, at["flags"], at["cls"], at["oneway"])
    # complementary halves of the start/end edges
    for e, which in ((first_e, "head"), (last_e, "tail")):
        drv = pieces[0][0] if which == "head" else pieces[-1][0]
        full = e["pts"]
        at = R.piece_attrs(e, 0.5)
        if which == "head":
            rest = [p for p in full if R.hav(p, drv[0]) > 1 and R.hav(p, drv[-1]) > R.hav(drv[0], drv[-1])]
            if rest:
                add(rest + [drv[0]], at, at["flags"], at["cls"], at["oneway"])
        else:
            rest = [p for p in full if R.hav(p, drv[-1]) > 1 and R.hav(p, drv[0]) > R.hav(drv[0], drv[-1])]
            if rest:
                add([drv[-1]] + rest, at, at["flags"], at["cls"], at["oneway"])
    # tracks (not in the routing graph)
    seg = overture.fetch("transportation", "segment", R.BBOX)
    for r in seg.select(["subtype", "class", "road_flags", "geometry"]).to_pylist():
        if r["subtype"] == "road" and r["class"] == "track":
            g = wkb.loads(r["geometry"])
            flags = set()
            for f in r["road_flags"] or []:
                flags.update(f["values"])
            add(list(g.coords), None, flags, "track", False)
    print("other roads:", len(out), collections.Counter(o[0] for o in out))
    return out


# ----------------------------------------------------------------------------- trees
def tree_type(tags):
    t = {k: (v or "").lower() for k, v in (tags or {}).items()}
    s = " ".join(t.get(k, "") for k in ("genus", "species", "taxon", "species:fr"))
    if "platan" in s:
        return 1
    if "pin" in s and "pinus" in s or "pinus" in s or "sylvestre" in s:
        return 2
    if "cupress" in s or "cyprès" in s or "cypres" in s:
        return 3
    if "olea" in s or "olivier" in s:
        return 4
    if t.get("denotation") == "avenue":
        return 1
    if t.get("leaf_type") == "needleleaved":
        return 2
    return 0


def map_trees(llbox, fwd, route_line):
    land = overture.fetch("base", "land", llbox)
    zone = route_line.buffer(TREE_RADIUS)
    out = []
    for r in land.select(["subtype", "class", "source_tags", "geometry"]).to_pylist():
        if r["subtype"] != "tree":
            continue
        tags = dict(r["source_tags"] or {})
        g = wkb.loads(r["geometry"])
        if r["class"] == "tree" and g.geom_type == "Point":
            x, z = fwd(g.x, g.y)
            if zone.contains(Point(float(x), float(-z))):
                out.append((float(x), float(z), tree_type(tags)))
        elif r["class"] == "tree_row":
            lines = [g] if g.geom_type == "LineString" else ([LineString(g.exterior.coords)] if g.geom_type == "Polygon" else [])
            for ln in lines:
                c = np.array(ln.coords)
                x, z = fwd(c[:, 0], c[:, 1])
                L = LineString(np.stack([x, -z], 1))
                tt = tree_type(tags) or 1
                for d in np.arange(0, L.length + 0.01, 9.0):
                    p = L.interpolate(d)
                    if zone.contains(p):
                        out.append((float(p.x), float(-p.y), tt))
    print("trees:", len(out), collections.Counter(t[2] for t in out))
    return out


# ----------------------------------------------------------------------------- places
TOWNS = {"Salon-de-Provence": 0, "Miramas": 0, "Istres": 0, "Berre-l'Étang": 0}


def make_places(S, geo, fwd, llbox, x0, z0, nx, nz):
    div = overture.fetch("divisions", "division", (4.95, 43.45, 5.35, 43.75))
    out = []
    x1, z1 = x0 + nx * CELL, z0 + nz * CELL
    for r in div.select(["subtype", "names", "geometry"]).to_pylist():
        if r["subtype"] != "locality" or not r["names"]:
            continue
        g = wkb.loads(r["geometry"])
        if g.geom_type != "Point":
            continue
        x, z = fwd(g.x, g.y)
        if x0 - 3000 <= x <= x1 + 3000 and z0 - 3000 <= z <= z1 + 3000:
            nm = r["names"]["primary"]
            out.append((float(x), float(z), S.id(nm), TOWNS.get(nm, 1)))
    # lieux-dits read on the Google screenshot (label centres)
    for nm, (px, py) in {"La Pécette": (334, 564), "Les Grandes Bastides": (440, 410), "Confoux": (390, 362),
                         "Le Pont de Rhaud": (205, 330)}.items():
        lon, lat = geo.to_lonlat(px, py)
        x, z = fwd(lon, lat)
        out.append((float(x), float(z), S.id(nm), 2))
    print("places:", len(out))
    return out


# ----------------------------------------------------------------------------- far grid
def far_grid(fwd, inv, rx, rz):
    cx, cz = (rx.min() + rx.max()) / 2, (rz.min() + rz.max()) / 2
    n = int(FAR_SIZE / FAR_CELL) + 1
    fx0 = math.floor((cx - FAR_SIZE / 2) / FAR_CELL) * FAR_CELL
    fz0 = math.floor((cz - FAR_SIZE / 2) / FAR_CELL) * FAR_CELL
    GX, GZ = np.meshgrid(fx0 + np.arange(n) * FAR_CELL, fz0 + np.arange(n) * FAR_CELL)
    lon, lat = inv(GX.ravel(), GZ.ravel())
    box = (lon.min() - 0.01, lat.min() - 0.01, lon.max() + 0.01, lat.max() + 0.01)
    terr = elevation.Terrarium(box, z=11)
    h = terr.sample(lon, lat).reshape(n, n)
    # land cover from WorldCover overviews (mode resampling)
    import rasterio
    from rasterio.enums import Resampling
    from rasterio.windows import from_bounds
    cache = os.path.expanduser("~/.cache/carpolo-world/worldcover_far_%.3f_%.3f.npz" % (box[0], box[1]))
    if os.path.exists(cache):
        z = np.load(cache); a, tr = z["a"], tuple(z["tr"])
    else:
        url = ("/vsicurl/https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/"
               "ESA_WorldCover_10m_2021_v200_N42E003_Map.tif")
        with rasterio.open(url) as ds:
            w = from_bounds(*box, ds.transform)
            oh, ow = int(w.height / 6), int(w.width / 6)
            a = ds.read(1, window=w, out_shape=(oh, ow), resampling=Resampling.mode)
            t = ds.window_transform(w)
            tr = (t.a * w.width / ow, t.c, t.e * w.height / oh, t.f)
        np.savez_compressed(cache, a=a, tr=np.array(tr))
    ta, tc, te, tf = tr
    ci = np.clip(((lon - tc) / ta).astype(int), 0, a.shape[1] - 1)
    cj = np.clip(((lat - tf) / te).astype(int), 0, a.shape[0] - 1)
    lut = np.zeros(256, np.uint8)
    for k, v in WORLDCOVER.items():
        lut[k] = v
    c = lut[a[cj, ci]].reshape(n, n)
    h = np.where(c == WATER, np.maximum(h, 0.0) * 0 + np.where(h < 2, 0.0, h), h)
    h = np.where(h < -1, 0.0, h)
    print("far grid %dx%d, h %.0f..%.0f, classes %s" % (n, n, h.min(), h.max(), np.bincount(c.ravel(), minlength=13)))
    return h, c, fx0, fz0, n


# ----------------------------------------------------------------------------- writer
def write(path, origin, bbox, S, route_recs, dem, lcov, far, farc, bldg, roads, trees, places, zones):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    secs = []

    def sec(tag, payload):
        assert len(tag) == 4
        secs.append(tag.encode("ascii") + struct.pack("<I", len(payload)) + payload)

    b = bytearray(struct.pack("<I", len(S.items)))
    for s in S.items:
        u = s.encode("utf-8")
        b += struct.pack("<H", len(u)) + u
    sec("STRS", bytes(b))

    b = bytearray(struct.pack("<I", len(route_recs)))
    for (x, y, z, w, lanes, fl, cls, flags, spd, nid, pid) in route_recs:
        b += struct.pack("<ffffBBBBHHHH", x, y, z, w, lanes, fl, cls, flags, spd, nid, pid, 0)
    sec("ROUT", bytes(b))

    def grid16(g):
        x0, z0, cell, nx, nz, h = g
        a = np.clip(np.round(h * 10), -32768, 32767).astype("<i2")
        return struct.pack("<fffII", x0, z0, cell, nx, nz) + a.tobytes()

    def grid8(g):
        x0, z0, cell, nx, nz, c = g
        return struct.pack("<fffII", x0, z0, cell, nx, nz) + c.astype(np.uint8).tobytes()

    sec("DEM ", grid16(dem))
    sec("LCOV", grid8(lcov))
    sec("FAR ", grid16(far))
    sec("FARC", grid8(farc))

    b = bytearray(struct.pack("<I", len(bldg)))
    for kind, roof, h, mh, ring in bldg:
        b += struct.pack("<HBBff", len(ring), kind, roof, h, mh)
        for x, z in ring:
            b += struct.pack("<ff", x, z)
    sec("BLDG", bytes(b))

    b = bytearray(struct.pack("<I", len(roads)))
    for cls, lanes, width, flags, pts in roads:
        b += struct.pack("<HBBfB3x", len(pts), cls, lanes, width, flags)
        for x, z in pts:
            b += struct.pack("<ff", x, z)
    sec("ROAD", bytes(b))

    b = bytearray(struct.pack("<I", len(trees)))
    for x, z, t in trees:
        b += struct.pack("<ffB3x", x, z, t)
    sec("TREE", bytes(b))

    b = bytearray(struct.pack("<I", len(places)))
    for x, z, nid, kind in places:
        b += struct.pack("<ffHBx", x, z, nid, kind)
    sec("PLAC", bytes(b))

    b = bytearray(struct.pack("<I", len(zones)))
    for s, t, sub in zones:
        b += struct.pack("<fHH", s, t, sub)
    sec("ZONE", bytes(b))

    head = b"CPW1" + struct.pack("<Idd", 1, origin[1], origin[0]) + struct.pack("<ffff", *bbox)
    with open(path, "wb") as f:
        f.write(head)
        for s in secs:
            f.write(s)
    for s in secs:
        print("  section %s %8d bytes" % (s[:4].decode(), len(s) - 8))


# ----------------------------------------------------------------------------- previews
LC_COLORS = {UNKNOWN: (200, 205, 170), FOREST: (46, 102, 52), SHRUB: (128, 140, 80), GRASS: (170, 196, 110),
             CROP: (222, 210, 140), VINE: (150, 110, 160), ORCHARD: (120, 160, 90), RESID: (215, 190, 170),
             INDUS: (190, 170, 190), BARE: (210, 200, 185), WATER: (80, 140, 210), WETLAND: (110, 170, 170),
             PAVED: (120, 120, 120)}


def previews(geo, fwd, rx, rz, ry, ry_raw, ss, dem, lcov, x0, z0, nx, nz, bldg, roads, trees, places, zones, S):
    lut = np.zeros((256, 3), np.uint8)
    for k, v in LC_COLORS.items():
        lut[k] = v
    img = Image.fromarray(lut[lcov])
    d = ImageDraw.Draw(img)
    P = lambda x, z: ((x - x0) / CELL, (z - z0) / CELL)
    for cls, lanes, width, flags, pts in roads:
        col = (255, 255, 255) if cls >= 2 else (150, 150, 150)
        if cls >= 6:
            col = (240, 90, 60)
        d.line([P(x, z) for x, z in pts], fill=col, width=2 if cls >= 3 else 1)
    for kind, roof, h, mh, ring in bldg:
        d.polygon([P(x, z) for x, z in ring], fill=(60, 50, 50))
    for x, z, t in trees:
        q = P(x, z)
        d.point(q, fill=(0, 60, 0))
    d.line([P(x, z) for x, z in zip(rx, rz)], fill=(255, 0, 0), width=3)
    # screenshot dark route (georeferenced) as blue dots
    dark, _ = georef.masks()
    ys, xs = np.nonzero(dark)
    for i in range(0, len(xs), 7):
        lon, lat = geo.to_lonlat(xs[i], ys[i])
        x, z = fwd(lon, lat)
        q = P(float(x), float(z))
        d.point(q, fill=(30, 30, 255))
    for x, z, nid, kind in places:
        q = P(x, z)
        d.text((q[0] + 3, q[1] - 5), S.items[nid], fill=(0, 0, 0))
    img.save(os.path.join(PREVIEW, "map.png"))
    # zoom on Lançon
    W, H = img.size
    img.save(os.path.join(PREVIEW, "map_full.png"))

    # elevation profile
    pw, ph = 1200, 360
    pr = Image.new("RGB", (pw, ph), (255, 255, 255))
    d = ImageDraw.Draw(pr)
    lo, hi = min(ry_raw.min(), ry.min()) - 2, max(ry_raw.max(), ry.max()) + 2
    X = lambda s: 40 + (pw - 60) * s / ss[-1]
    Y = lambda h: ph - 30 - (ph - 60) * (h - lo) / (hi - lo)
    d.line([(X(s), Y(h)) for s, h in zip(ss[::5], ry_raw[::5])], fill=(180, 180, 180), width=1)
    d.line([(X(s), Y(h)) for s, h in zip(ss[::5], ry[::5])], fill=(200, 0, 0), width=2)
    for s, t, sub in zones:
        d.line([(X(s), 20), (X(s), ph - 30)], fill=(0, 120, 255))
        d.text((X(s) + 2, 22), S.items[t], fill=(0, 0, 0))
    d.text((5, 5), "route elevation %.0f..%.0f m over %.0f m (grey raw DEM, red smoothed road)" % (ry.min(), ry.max(), ss[-1]), fill=(0, 0, 0))
    pr.save(os.path.join(PREVIEW, "profile.png"))

    # hillshade
    gy, gxx = np.gradient(dem, CELL)
    az, alt = math.radians(315), math.radians(40)
    slope = np.arctan(np.hypot(gxx, gy) * 2.0)
    aspect = np.arctan2(-gxx, gy)
    hs = np.sin(alt) * np.cos(slope) + np.cos(alt) * np.sin(slope) * np.cos(az - aspect)
    hs = np.clip(hs * 255, 0, 255).astype(np.uint8)
    im = Image.fromarray(hs).convert("RGB")
    d = ImageDraw.Draw(im)
    d.line([P(x, z) for x, z in zip(rx, rz)], fill=(255, 0, 0), width=2)
    im.save(os.path.join(PREVIEW, "hillshade.png"))


if __name__ == "__main__":
    main()
