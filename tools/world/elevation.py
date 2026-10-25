"""Elevation sources.

- Terrarium tiles (Mapzen / AWS "elevation-tiles-prod"): height = R*256 + G + B/256 - 32768.
  In Europe they are based on EU-DEM (25 m), smooth and close to bare earth.
- Copernicus GLO-30 COG (DSM, includes canopy/buildings) — used only for comparison.
"""
import io
import math
import os
import urllib.request
import concurrent.futures as cf

import numpy as np
from PIL import Image

CACHE = os.path.expanduser("~/.cache/carpolo-world/terrarium")
URL = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"


def lonlat_to_tile(lon, lat, z):
    n = 2 ** z
    x = (lon + 180.0) / 360.0 * n
    y = (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n
    return x, y


def _tile(z, x, y):
    os.makedirs(CACHE, exist_ok=True)
    p = os.path.join(CACHE, "%d_%d_%d.png" % (z, x, y))
    if not os.path.exists(p):
        with urllib.request.urlopen(URL.format(z=z, x=x, y=y), timeout=60) as r:
            data = r.read()
        with open(p, "wb") as f:
            f.write(data)
    a = np.asarray(Image.open(p).convert("RGB")).astype(np.float64)
    return a[..., 0] * 256 + a[..., 1] + a[..., 2] / 256 - 32768


class Terrarium:
    """Bilinear sampler over a mosaic of terrarium tiles covering a lon/lat bbox."""

    def __init__(self, bbox, z=14):
        lon0, lat0, lon1, lat1 = bbox
        x0, y1 = lonlat_to_tile(lon0, lat0, z)
        x1, y0 = lonlat_to_tile(lon1, lat1, z)
        self.z = z
        self.tx0, self.ty0 = int(x0), int(y0)
        tx1, ty1 = int(x1), int(y1)
        nx, ny = tx1 - self.tx0 + 1, ty1 - self.ty0 + 1
        mosaic = np.zeros((ny * 256, nx * 256))
        jobs = {}
        with cf.ThreadPoolExecutor(8) as ex:
            for j in range(ny):
                for i in range(nx):
                    jobs[(i, j)] = ex.submit(_tile, z, self.tx0 + i, self.ty0 + j)
        for (i, j), fut in jobs.items():
            mosaic[j * 256:(j + 1) * 256, i * 256:(i + 1) * 256] = fut.result()
        self.m = mosaic

    def sample(self, lon, lat):
        lon = np.asarray(lon, dtype=np.float64)
        lat = np.asarray(lat, dtype=np.float64)
        n = 2 ** self.z
        px = ((lon + 180.0) / 360.0 * n - self.tx0) * 256 - 0.5
        py = ((1.0 - np.arcsinh(np.tan(np.radians(lat))) / np.pi) / 2.0 * n - self.ty0) * 256 - 0.5
        H, W = self.m.shape
        px = np.clip(px, 0, W - 1.001)
        py = np.clip(py, 0, H - 1.001)
        i0 = np.floor(px).astype(int)
        j0 = np.floor(py).astype(int)
        fx, fy = px - i0, py - j0
        m = self.m
        return ((m[j0, i0] * (1 - fx) + m[j0, i0 + 1] * fx) * (1 - fy)
                + (m[j0 + 1, i0] * (1 - fx) + m[j0 + 1, i0 + 1] * fx) * fy)


def copernicus(lon, lat):
    """Sample Copernicus GLO-30 (DSM) bilinearly at points (comparison only)."""
    import rasterio
    lon = np.asarray(lon); lat = np.asarray(lat)
    os.environ.setdefault("CURL_CA_BUNDLE", "/etc/ssl/certs/ca-certificates.crt")
    if os.environ.get("HTTPS_PROXY"):
        os.environ.setdefault("GDAL_HTTP_PROXY", os.environ["HTTPS_PROXY"])
    url = ("/vsicurl/https://copernicus-dem-30m.s3.amazonaws.com/Copernicus_DSM_COG_10_N43_00_E005_00_DEM/"
           "Copernicus_DSM_COG_10_N43_00_E005_00_DEM.tif")
    from rasterio.windows import from_bounds
    with rasterio.open(url) as ds:
        w = from_bounds(lon.min() - 0.002, lat.min() - 0.002, lon.max() + 0.002, lat.max() + 0.002, ds.transform)
        a = ds.read(1, window=w).astype(np.float64)
        tr = ds.window_transform(w)
    px = (lon - tr.c) / tr.a - 0.5
    py = (lat - tr.f) / tr.e - 0.5
    i0 = np.clip(np.floor(px).astype(int), 0, a.shape[1] - 2)
    j0 = np.clip(np.floor(py).astype(int), 0, a.shape[0] - 2)
    fx, fy = px - i0, py - j0
    return ((a[j0, i0] * (1 - fx) + a[j0, i0 + 1] * fx) * (1 - fy)
            + (a[j0 + 1, i0] * (1 - fx) + a[j0 + 1, i0 + 1] * fx) * fy)
