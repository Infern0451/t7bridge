# T7 Bridge enclosure (3D printed)

Sealed two-part case for an ESP32-S3 DevKitC-style board **without pin
headers** (measured board: 57 × 28.1 mm plus a 6 × 18 mm antenna tab, 4.8 mm
tall). Outside size: **91.2 × 36.3 × 14.0 mm**, all outside edges rounded
(1.5 mm fillet).

Fit: the lid's locating lip has **0.2 mm clearance** per side to the base,
tuned for a 0.4 mm nozzle. If your printer runs tight or loose, change
`PRESS_FIT` in `make_case.py` and regenerate (see below). The gasket is
1.0 × 1.7 mm in a 1.2 × 1.2 mm groove, so it is squeezed 0.5 mm when the
lid is clamped.

| File | Print in | Orientation |
|---|---|---|
| `stl/t7bridge_base.stl` | PETG | open side up (as exported) |
| `stl/t7bridge_lid.stl` | PETG | top face on the bed, lip up (as exported) |
| `stl/t7bridge_gasket.stl` | TPU 95A (optional) | flat |

**Print settings (PETG):** 0.2 mm layers, **4 perimeters** (helps keep water
out), 3 top/bottom layers or more, 25-40 % infill, no supports. The zip-tie
channels on the bottom faces are short bridges that print fine.

## What you need

- A **short USB-C cable with a straight plug** on the board end. The case
  holds the plug inside: plug body up to about 18 mm long, 12 mm wide and
  7.5 mm thick; cable up to about 4.2 mm thick. The bike end can be
  right-angled.
- Either the printed TPU gasket or a tube of **neutral-cure RTV silicone**.
- **Two zip ties** up to 5 mm wide.
- Optional: a small piece of foam tape on top of the board to stop rattles.
- **3M VHB** tape for mounting.

## Assembly

1. **Dry-fit first.** Put the board in (components up, antenna end against
   the far wall, USB end towards the notch), close the lid, check it seats.
2. Plug the cable into the board's **USB** port and lay the cable in the notch.
3. Fit the gasket in the groove, **or** run a thin bead of RTV in the groove.
   Add a dab of RTV around the cable where it passes through the notch.
4. Press the lid on and fit two zip ties in the channels, snug, not crushing.
5. Mount with VHB on the flat bottom between the channels.

Keep metal (brackets, foil tape) away from the antenna end, opposite the USB
plug.

## Changing the size

All dimensions are at the top of `make_case.py`. Edit them and run:

```sh
python3 make_case.py
```

(Needs Python 3 and numpy.) It regenerates the three STL files and checks they
are watertight. The STLs are fine-meshed (base about 19 MB, lid about 13 MB)
so the fillets come out smooth; raise `VOXEL` (e.g. to 0.4) for smaller files.

## License

CC BY-SA 4.0 (see `LICENSE` in this folder). Share and remix freely, with credit, under the same license.
