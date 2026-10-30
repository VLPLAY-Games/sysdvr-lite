# Dock for the Switch Lite with the Orange Pi Zero 2W

A 3D-printable dock: the honeycomb stand holds the Switch Lite, and a separate block under it holds the
Orange Pi Zero 2W (the sysdvr-lite client). The stand with the console is put on top of the block, four legs on the
stand go into four sockets in the roof of the block. Nothing is glued, the stand can be lifted off.

Status: designed and sliced, the first print is under way. Everything below that is marked *unverified* was not
measured on the real parts.

## Print files

Four STL files in `stand/`, in millimetres:

| File | Part | Print orientation | Notes |
|---|---|---|---|
| `board_block.stl` | board block (139.7 x 58 x 15.2) | as it is: roof side down | already turned, flat smooth top on the bed |
| `board_lid.stl` | lid with bosses, air slots, tabs (139.7 x 58) | as it is: flat | already turned |
| `stand_back_with_legs.stl` | back piece of the stand with four legs | as it is: rear face on the bed | the orientation of your original file; slicers add a little support under the legs |
| `stand_front_mesh.stl` | front panel of the stand | **turn it over**: the flat face with the logo on the bed | this STL is in the orientation of the original file (logo face up) |

The block and the lid fit one 220 x 220 bed together, so do the two stand parts. Sliced for a Flashforge
Adventurer 5M (PLA, 0.4 mm): block + lid 1 h 24 min and 53 g, stand parts 2 h 55 min and 98 g.

## How it is built

- **Stand (honeycomb).** Made from your two stand parts by `make_stand_mesh.py`: hexagonal through-holes in both plates (front: R 7 mm, 2.6 mm ribs, logo area solid; back: R 11 mm, 3.6 mm ribs) and the bottom ledge halved from 24 to 12 mm, cut where the cross-section of both parts is constant (y -40.5 .. -28.5), so it is seamless. The originals are not modified.
- **Board block.** 139.7 x 58 x 15.2 mm (+2.4 mm lid). The board lies at the front **upside down**: the 14 x 14 x 6 mm heatsink and the connectors face the lid, its ports face the rear. Behind the ports there is a 20 mm corridor for the plugs. The short Switch cable comes down from the console through a slot in the roof and goes straight into the USB-C data port (an angled plug is best). HDMI and power plugs sit in the corridor, their cables leave through the rear window.
- **Board fixing.** Four M2.5 x 6 (or 8) screws into 7 mm bosses on the lid, inserted from the flat back of the board (the heads fit in the 3.5 mm under the roof). Air slots in the lid under the heatsink.
- **microSD.** The cavity is 6 mm wider at the card end (the inserted card sticks out of the board) and there is a window to the outside along almost the whole short edge.
- **Legs.** Four tabs 8 x 9 mm, 10 mm long on the bottom edge of the back piece, sockets with 0.3 mm clearance in the solid side parts of the block roof.

Drawing: `preview/board_drawing.png` (view from below with the lid off: board, screws, plugs and cables).
Renders (made with the local scripts): `preview/assembly_front.png`, `preview/assembly_rear.png` (the stand parts are placed approximately),
`preview/stand_front_mesh.png`, `preview/stand_back_with_legs.png`.

## Assembly

1. Lid on the table (bosses up), board on the bosses (heatsink down, between them), four screws.
2. Lower the block over the board, push it on, glue the lid to the block.
3. Put the stand on the block: legs into the sockets, the front panel rests on the flat top of the block.
4. Plug the cables: HDMI, power and the Switch cable into the USB-C data port, the other end of the Switch cable into the console through the slot in the roof.

## Check before relying on it (unverified)

- **Board holes.** The pattern of the Raspberry Pi Zero is assumed: 58 x 23 mm, 3.5 mm from the edges (`HOLE_DX`, `HOLE_DZ` in `make_stand_block.py`). The Orange Pi documentation gives 3.0 mm holes, a Pi Zero has 2.75 mm, both pass an M2.5 screw. Measure the distance between your holes, or print only the lid (20 minutes) and test the board on it.
- **Connectors and microSD.** `PORT_X` (illustrative x positions of mini HDMI and the two USB-C ports) and `SD_LEFT` must match your board. The Switch cable goes into the **data** port, not the power port. The roof slot follows `PORT_X['data']`.
- **Plug.** The shorter ledge removed the groove for the plug, only the through-hole remains. An angled plug fits best, a straight plug needs about 25 mm below the console. `LEDGE_CUT = None` in `make_stand_mesh.py` brings the full ledge back.
- **Stability (paper estimate).** About 120 mm tall, 58 mm deep, the rear face is flush with the back piece: it tips over backwards from a push of about 0.9 N (90 g at the top). A lid with wings (the scripts can make one) gives about 2 N. Rubber feet help.
- **Stand pieces.** How your two stand parts mate is not in the 3MF, the block does not join them.

## Rebuilding

The scripts that generate the files (`make_stand_mesh.py`, `make_stand_block.py`, `make_3mf.py`, `preview.py`,
`diagram.py`, `render.py`) are kept locally and are excluded from the repository by `.gitignore`. In short:
`make_stand_mesh.py` makes the honeycomb stand parts from `switch-lite-dock-front.3mf` (the third-party stand model,
which also stays outside of the repository, check its license before publishing anything made from it),
`make_stand_block.py` makes the block, the lid and the legs (parameters for the board, screws, legs, microSD and lid
are at its top), `make_3mf.py` packs the parts into two 3MF print jobs. They need
`pip install manifold3d trimesh numpy pillow`.
