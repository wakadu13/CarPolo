# tools/world — real-world data for CarPolo 3D

Builds `game3d/assets/world.bin` (format: `game3d/DESIGN.md`) for the real route
**2000 chemin départemental 15 (Les Plaines / La Pécette) → Les Grandes Bastides → Lançon-Provence →
route de Pélissanne → chemin de Saint-Jean-en-Crau → arrival just south of the A54 (Salon-de-Provence)**,
≈12.5 km — the dark "15 min · 12,6 km" route of the user's Google Maps screenshot.

## Sources (anonymous HTTPS, AWS open data; OSM/Overpass/Google are not reachable from the build box)

| data | source |
|---|---|
| roads, connectors, buildings, land use, trees/tree rows, forests, water, communes, addresses | Overture Maps release `2026-10-21.0` (OpenStreetMap-derived; `overturemaps-us-west-2` S3, GeoParquet, bbox-pruned range reads) |
| terrain (DEM, route profile, far horizon) | Mapzen/AWS Terrarium tiles z14 (and z11 for the far grid) — EU-DEM based, smoother than Copernicus GLO-30 (a DSM with canopy/buildings), which was compared and rejected |
| land cover base | ESA WorldCover 10 m v200 (2021), refined with Overture land use (vineyards, orchards/olive groves, farmland, residential, industrial, forests, scrub, rock, water) |
| route shape | the user's screenshot, reduced to binary line masks in `data/` (no map imagery kept) |

## How the route is found

1. `georef.py` fits the screenshot as a Web-Mercator crop (scale + offset) from 4 locality labels,
   refined by trimmed ICP between the screenshot's road pixels and Overture roads (mean residual 0.75 px ≈ 11 m).
2. `route.py` builds a car-routable graph from Overture segments/connectors (one-way and access
   restrictions respected; roundabouts reconstructed geometrically and forced counter-clockwise, since
   Overture drops OSM's implicit `junction=roundabout` one-way), then runs Dijkstra with edge cost =
   length × (1 + penalty(distance to the dark Google line)). Start = Overture address
   "2000 Chemin Départemental 15, 13250" snapped to the road; end = tip of the red pin.
   Result: 12.5 km, the A7 motorway is **not** used (the route only runs near it, on the D-roads).
3. `build_world.py` resamples the route every 2 m (light smoothing), adds per-point attributes
   (class, width, lanes, one-way, roundabout, bridge/tunnel, built-up, speed limit, road name, commune),
   smooths the road profile (≈30 m, ≤8 % grade, level bridges), and writes every section.

## Run

```
pip install numpy pillow pyarrow shapely rasterio pyproj scipy
python3 build_world.py      # ≈1 min with a warm cache (~/.cache/carpolo-world), writes game3d/assets/world.bin
python3 read_world.py       # parses the file back and prints section stats + a route attribute summary
```
Previews (map, elevation profile, hillshade) go to `$CARPOLO_PREVIEW_DIR`.
Set `CARPOLO_SCREENSHOT` to the original screenshot if available; otherwise `data/*.png` masks are used.

## Notes for the engine

- `ROAD.flags` uses the same bits as `ROUT.flags` (bit0 one-way, bit1 roundabout, bit2 bridge, bit3 tunnel)
  plus **bit5 = unpaved track**. Roundabout pieces are stored in their driving (counter-clockwise) direction.
- `BLDG.roofShape` is mostly 3 (unknown in the data): pick a Provençal terracotta pitched roof for houses;
  big warehouses/shops are set to 0 (flat).
- `TREE` holds the mapped trees (incl. plane-tree avenues: Overture `denotation=avenue`, *Platanus*) and tree rows
  sampled every 9 m; forests/orchards/vineyards are only in `LCOV`.
- `PLAC` = Overture localities + lieux-dits read on the screenshot (La Pécette, Les Grandes Bastides, Confoux, Le Pont de Rhaud).
