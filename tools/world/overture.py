"""Bounding-box extraction from Overture Maps GeoParquet on S3 (anonymous, HTTP range reads).

Only parquet footers and the row groups whose bbox statistics intersect the requested
area are downloaded. Results are cached as local parquet files under ~/.cache/carpolo-world.
"""
import os
import concurrent.futures as cf

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.fs as pafs
import pyarrow.parquet as pq

RELEASE = os.environ.get("OVERTURE_RELEASE", "2026-10-21.0")
BUCKET = "overturemaps-us-west-2"
CACHE = os.path.expanduser("~/.cache/carpolo-world")


def _fs():
    proxy = os.environ.get("HTTPS_PROXY") or os.environ.get("https_proxy")
    kw = dict(anonymous=True, region="us-west-2")
    if proxy:
        kw["proxy_options"] = proxy
    return pafs.S3FileSystem(**kw)


def _rg_bbox(md, rg, names):
    """Return (xmin, ymin, xmax, ymax) of a row group from column statistics, or None."""
    g = md.row_group(rg)
    vals = {}
    for c in range(g.num_columns):
        col = g.column(c)
        p = col.path_in_schema
        if p in names and col.statistics is not None and col.statistics.has_min_max:
            vals[p] = (col.statistics.min, col.statistics.max)
    try:
        return (vals["bbox.xmin"][0], vals["bbox.ymin"][0], vals["bbox.xmax"][1], vals["bbox.ymax"][1])
    except KeyError:
        return None


def fetch(theme, typ, bbox, columns=None, tag="", workers=16):
    """bbox = (lon_min, lat_min, lon_max, lat_max). Returns a pyarrow Table (cached)."""
    os.makedirs(CACHE, exist_ok=True)
    key = "%s_%s_%s_%.4f_%.4f_%.4f_%.4f%s.parquet" % (RELEASE, theme, typ, *bbox, tag)
    path = os.path.join(CACHE, key)
    if os.path.exists(path):
        return pq.read_table(path)
    fs = _fs()
    base = "%s/release/%s/theme=%s/type=%s/" % (BUCKET, RELEASE, theme, typ)
    files = [i.path for i in fs.get_file_info(pafs.FileSelector(base)) if i.path.endswith(".parquet")]
    names = {"bbox.xmin", "bbox.ymin", "bbox.xmax", "bbox.ymax"}
    x0, y0, x1, y1 = bbox

    def scan(fp):
        f = pq.ParquetFile(fp, filesystem=fs)
        md = f.metadata
        rgs = []
        for rg in range(md.num_row_groups):
            b = _rg_bbox(md, rg, names)
            if b is None or (b[0] <= x1 and b[2] >= x0 and b[1] <= y1 and b[3] >= y0):
                rgs.append(rg)
        if not rgs:
            return None
        t = f.read_row_groups(rgs, columns=columns)
        bb = t.column("bbox")
        m = pc.and_(pc.and_(pc.less_equal(pc.struct_field(bb, "xmin"), x1), pc.greater_equal(pc.struct_field(bb, "xmax"), x0)),
                    pc.and_(pc.less_equal(pc.struct_field(bb, "ymin"), y1), pc.greater_equal(pc.struct_field(bb, "ymax"), y0)))
        t = t.filter(m)
        return t if t.num_rows else None

    tables = []
    with cf.ThreadPoolExecutor(workers) as ex:
        for t in ex.map(scan, files):
            if t is not None:
                tables.append(t)
    if tables:
        tab = pa.concat_tables(tables, promote_options="default")
    else:
        tab = pa.table({})
    pq.write_table(tab, path)
    return tab
