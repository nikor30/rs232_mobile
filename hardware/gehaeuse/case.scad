// RS232-WLAN-Konsole -- 3D-printed enclosure, V0.2
// ARCHITECTURE CHANGE from v0.1: the design now stacks TWO boards instead of
// housing one custom all-in-one PCB:
//   Tier 1 (walled, bottom): the new I2C daughterboard (112.5x110mm, from
//   gen_pcb.py) -- 4x RJ45 + MAX3232 + SC16IS750 + local 3.3V regulator.
//   Tier 2 (open frame, standoffs only, no side walls): the off-the-shelf
//   LCDwiki "2.8inch ESP32-S3 Display" base board (50x86x10.6mm), which
//   supplies the ESP32-S3, LCD+touch, and USB-C/battery charging. It is NOT
//   rigidly plugged into the daughterboard (the base board's P3/P4 header is
//   1.25mm pitch, not stackable) -- the two boards are linked by a short
//   hand-wired pigtail cable (I2C + shared battery), so Tier 2 is left open
//   on the sides rather than walled: the base board's exact USB-C/button
//   positions weren't confirmed (see gen_pcb.py / parts.py docstring), so an
//   open shelf stays correct regardless of where they turn out to be, rather
//   than guessing a cutout position that might be wrong.
//
// Honestly-flagged assumptions:
//  - Base board mounting holes: 4x M3 (3.2mm), 42x78mm spacing, centered on
//    the 50x86mm board -- from LCDwiki's ES3C28P_Size.pdf dimension drawing.
//  - Daughterboard mounting holes: NOT actually present in gen_pcb.py yet
//    (no drilled mounting-hole footprints) -- standoffs below assume a
//    6mm corner inset, matching v0.1's convention. Add real mounting holes
//    to gen_pcb.py before ordering, or adjust these positions once you do.
//  - Battery: same 1000-2000mAh LiPo pouch assumption as v0.1 (~6.5mm thick),
//    now shared (Y-tapped) between both boards.
//  - Wall/standoff/heat-set-insert dimensions are standard FDM defaults,
//    not yet validated against a specific printer/filament.
//
// Prints as THREE separate parts, not two: modeling Tier-2's standoffs as
// part of the same contiguous solid as the bottom shell doesn't work -- they
// have to start above the daughterboard's own thickness (the real PCB fills
// that gap, not printed plastic), so in the CAD model they'd be floating,
// disconnected islands with nothing under them (caught via the "Volumes"
// count: a healthy hollow bottom shell with its standoffs properly fused
// reads "Volumes: 2"; modeling Tier 2 as more of the same solid gave
// "Volumes: 6" -- 4 extra, genuinely disconnected pieces, one per standoff).
// So Tier 2 is its own small part ("spacer"): an open frame that sits loose
// on top of the assembled daughterboard (resting on the real PCB surface,
// its own screws going through it) and holds the base board above that.
// This is a normal, sound pattern for a stacked enclosure, not a workaround.
//
// Usage:
//   openscad -o bottom.stl  -D part=\"bottom\"  case.scad
//   openscad -o spacer.stl  -D part=\"spacer\"  case.scad
//   openscad -o top.stl     -D part=\"top\"     case.scad
//   openscad -o preview.png -D part=\"assembly\" --imgsize=1600,1200 case.scad

part = "assembly"; // "bottom" | "spacer" | "top" | "assembly" (preview only, not for printing)

// -------------------------------------------------- daughterboard (Tier 1) --
// From gen_pcb.py: BOARD_W=107.0, BOARD_H=110.0 (KiCad mm, Y-down). BOARD_W
// dropped from the earlier 112.5mm once the daughterboard moved from 4
// separate one-chip-per-port support clusters to 2 shared clusters (see
// parts.py's v2.1 docstring: 2x SC16IS752 + 2x MAX3232, both channels used,
// instead of 4x SC16IS750 + 4x MAX3232) -- less board area needed for the
// same 4 RJ45 ports. Kept in sync here because DB_CORNERS (the standoff
// positions) are computed FROM this value by inset, not independently.
DB_W = 107.0;
DB_L = 110.0;
DB_THICKNESS = 1.6;

// RJ45 port positions copied from gen_pcb.py's port_cols_x / cluster row1:
RJ45_X = [18.0, 42.0, 66.0, 90.0];
RJ45_Y = 23.5;                    // cluster origin_y(12) + RJ45 CLASS_SIZE height(23)/2

// Shared I2C/battery-pigtail connector (J1/J2) sits on the "shared" cluster,
// origin (8,78) -- on the LEFT wall (low-X edge), roughly 2/3 down the board.
CABLE_X_POS = 20.0;               // approx J1/J2 cluster Y-center on the left edge

// ----------------------------------------------------- base board (Tier 2) --
DISP_W = 50.0;
DISP_L = 86.0;
DISP_THICKNESS = 10.6;            // touch variant (ES3C28P), tallest case
DISP_HOLE_DX = 42.0;              // mounting-hole spacing (LCDwiki ES3C28P_Size.pdf)
DISP_HOLE_DY = 78.0;
DISP_HOLE_D = 3.2;

// Centered on the daughterboard's footprint (Tier 2 has no walls of its own,
// so exact centering isn't safety-critical -- just needs to clear the lid's
// LCD window, set from the same center below).
DISP_CX = DB_W / 2;
DISP_CY = DB_L / 2;

// ------------------------------------------------------------ case shell --
WALL = 2.0;
FLOOR = 2.0;
LID_THICKNESS = 2.0;
GAP = 0.3;                        // clearance between DB edge and inner wall (Tier 1 only)
BATTERY_CLEARANCE_H = 6.5;        // typical 1000-2000mAh LiPo pouch, lies flat on the floor
DB_STANDOFF_H = 5.0;              // floor -> daughterboard seat
DISP_STANDOFF_H = 10.0;           // daughterboard top -> base-board seat; clears the
                                   // RJ45 jacks' upward protrusion (~7mm above the DB
                                   // plane, since they straddle the board edge) plus
                                   // room for the I2C/battery pigtail to loop through.
STANDOFF_OD = 6.0;
STANDOFF_ID = 2.6;                // pilot hole for a self-tapping M3 screw

INNER_W = DB_W + 2 * GAP;
INNER_L = DB_L + 2 * GAP;
OUTER_W = INNER_W + 2 * WALL;
OUTER_L = INNER_L + 2 * WALL;

DB_SEAT_Z   = FLOOR + BATTERY_CLEARANCE_H + DB_STANDOFF_H;             // bottom of daughterboard
DB_MID_Z    = DB_SEAT_Z + DB_THICKNESS / 2;                            // for RJ45 cutout centering
DISP_SEAT_Z = DB_SEAT_Z + DB_THICKNESS + DISP_STANDOFF_H;              // bottom of base board
DISP_TOP_Z  = DISP_SEAT_Z + DISP_THICKNESS;                            // top of base board (LCD surface)

// RJ45 (8P8C shielded jack) cutout -- same generous sizing as v0.1.
RJ45_CUT_W = 16.5;
RJ45_CUT_H = 14.0;

// Tier 1 (walled) height: floor to just above the daughterboard + enough
// margin for the RJ45 jacks' upward protrusion -- derived, not guessed, same
// lesson as v0.1 (an under-sized wall broke the RJ45 cutout through the top
// edge there). RJ45_TOP_MARGIN leaves a solid strip of wall material above
// the cutout on purpose: without it, the cutout's top face and the wall's
// top rim land exactly coincident (verified by rendering a straight top-down
// view -- the notch didn't show at all, a classic degenerate coplanar-face
// CAD case), which would print as a 0mm-thick knife edge over each RJ45 port
// at best, and is a coin-flip on floating-point rounding at worst.
RJ45_TOP_MARGIN = 2.0;
TIER1_H = DB_SEAT_Z + DB_THICKNESS + max(RJ45_CUT_H / 2 - DB_THICKNESS / 2, 3.0) + RJ45_TOP_MARGIN;

// Overall case height: Tier 1 wall + open Tier 2 standoffs + base board +
// lid clearance + lid thickness.
LID_CLEARANCE = 2.0;              // air gap above the base board before the lid's ceiling
TOTAL_H = DISP_TOP_Z + LID_CLEARANCE + LID_THICKNESS;
LID_WALL_H = TOTAL_H - TIER1_H;   // how far the lid's skirt drops to meet the Tier-1 shell

// LCD window in the lid: ES3C28P's active area is 43.20x57.60mm per the
// dimension drawing (LCD AA), centered on the base board's own center.
LCD_ACTIVE_W = 43.2;
LCD_ACTIVE_H = 57.6;

// corner standoff positions
INSET = 6.0;
DB_CORNERS = [
    [INSET, INSET], [DB_W - INSET, INSET],
    [INSET, DB_L - INSET], [DB_W - INSET, DB_L - INSET],
];
DISP_CORNERS = [
    [DISP_CX - DISP_HOLE_DX/2, DISP_CY - DISP_HOLE_DY/2],
    [DISP_CX + DISP_HOLE_DX/2, DISP_CY - DISP_HOLE_DY/2],
    [DISP_CX - DISP_HOLE_DX/2, DISP_CY + DISP_HOLE_DY/2],
    [DISP_CX + DISP_HOLE_DX/2, DISP_CY + DISP_HOLE_DY/2],
];

module rounded_rect(w, h, r) {
    hull() {
        for (dx = [r, w - r])
            for (dy = [r, h - r])
                translate([dx, dy]) circle(r = r, $fn = 32);
    }
}

// ------------------------------------------------------------ bottom shell
// Same hollow-then-union-standoffs ordering as v0.1 (subtracting a full
// interior cavity from a union that already contains the standoffs erases
// them -- see v0.1's case.scad history), plus the same epsilon-overlap trick
// so standoffs and screw bosses actually fuse into one manifold part instead
// of floating as separate CGAL volumes.
module bottom_shell() {
    EPS = 0.01;
    difference() {
        union() {
            difference() {
                translate([-WALL, -WALL, 0])
                    linear_extrude(height = TIER1_H)
                        rounded_rect(OUTER_W, OUTER_L, 3);
                translate([0, 0, FLOOR])
                    linear_extrude(height = TIER1_H)
                        rounded_rect(INNER_W, INNER_L, 2);
                // RJ45 cutouts, front wall (low-Y edge), Z-centered on the
                // daughterboard plane so the jack's body (which straddles
                // the PCB) isn't clipped by an undersized wall.
                for (x = RJ45_X)
                    translate([x - RJ45_CUT_W / 2, -WALL - 1, DB_MID_Z - RJ45_CUT_H / 2])
                        cube([RJ45_CUT_W, WALL + 2, RJ45_CUT_H]);
                // Cable pass-through slot, left wall (low-X edge), for the
                // I2C + battery pigtail running out to the base board (which
                // sits on open Tier-2 standoffs above, not inside this wall).
                translate([-WALL - 1, CABLE_X_POS - 4, DB_MID_Z - 3])
                    cube([WALL + 2, 8, 6]);
            }
            // Tier 1 standoffs: daughterboard seat.
            for (c = DB_CORNERS)
                translate([c[0], c[1], FLOOR - EPS])
                    cylinder(h = BATTERY_CLEARANCE_H + DB_STANDOFF_H + EPS, d = STANDOFF_OD, $fn = 24);
        }
        // screw pilot holes through the standoffs + the floor beneath them
        for (c = DB_CORNERS)
            translate([c[0], c[1], -1])
                cylinder(h = TIER1_H + 2, d = STANDOFF_ID, $fn = 16);
    }
}

// --------------------------------------------------------------- spacer ---
// Tier 2, printed separately (see docstring above): an open frame that rests
// on the assembled daughterboard's TOP surface and holds the base board
// DISP_STANDOFF_H above it. Own self-contained part -- floor-less, wall-less,
// just 4 legs joined by thin corner-to-corner struts for rigidity so it
// isn't 4 loose cylinders on the print bed.
SPACER_RING_T = 8.0;    // ring wall thickness -- wide enough to fully contain
                         // the STANDOFF_ID pilot hole at each corner
SPACER_MARGIN = 7.0;    // beyond the mounting-hole bounding box
SPACER_OUTER_W = DISP_HOLE_DX + 2 * SPACER_MARGIN;
SPACER_OUTER_L = DISP_HOLE_DY + 2 * SPACER_MARGIN;
SPACER_INNER_W = SPACER_OUTER_W - 2 * SPACER_RING_T;
SPACER_INNER_L = SPACER_OUTER_L - 2 * SPACER_RING_T;

module spacer() {
    translate([DISP_CX - SPACER_OUTER_W / 2, DISP_CY - SPACER_OUTER_L / 2, 0]) {
        difference() {
            union() {
                difference() {
                    linear_extrude(height = DISP_STANDOFF_H)
                        rounded_rect(SPACER_OUTER_W, SPACER_OUTER_L, 4);
                    translate([SPACER_RING_T, SPACER_RING_T, -1])
                        linear_extrude(height = DISP_STANDOFF_H + 2)
                            rounded_rect(SPACER_INNER_W, SPACER_INNER_L, 2);
                }
                // corner posts, slightly larger than the ring there, so the
                // pilot hole always has full material around it regardless
                // of rounding at the ring's own corners
                for (c = DISP_CORNERS)
                    translate([c[0] - (DISP_CX - SPACER_OUTER_W / 2), c[1] - (DISP_CY - SPACER_OUTER_L / 2), 0])
                        cylinder(h = DISP_STANDOFF_H, d = STANDOFF_OD, $fn = 24);
            }
            for (c = DISP_CORNERS)
                translate([c[0] - (DISP_CX - SPACER_OUTER_W / 2), c[1] - (DISP_CY - SPACER_OUTER_L / 2), -1])
                    cylinder(h = DISP_STANDOFF_H + 2, d = STANDOFF_ID, $fn = 16);
        }
    }
}

// --------------------------------------------------------------- top lid --
module top_lid() {
    EPS2 = 0.01;
    difference() {
        union() {
            translate([-WALL, -WALL, 0])
                linear_extrude(height = LID_THICKNESS)
                    rounded_rect(OUTER_W, OUTER_L, 3);
            difference() {
                translate([-WALL, -WALL, -LID_WALL_H])
                    linear_extrude(height = LID_WALL_H)
                        rounded_rect(OUTER_W, OUTER_L, 3);
                translate([0, 0, -LID_WALL_H - 1])
                    linear_extrude(height = LID_WALL_H + 2)
                        rounded_rect(INNER_W, INNER_L, 2);
            }
        }
        // LCD window, centered on the base board's own center.
        translate([DISP_CX - LCD_ACTIVE_W / 2, DISP_CY - LCD_ACTIVE_H / 2, -1])
            cube([LCD_ACTIVE_W, LCD_ACTIVE_H, LID_THICKNESS + 2]);
        // screw clearance holes, aligned with the Tier-1 standoffs below
        for (c = DB_CORNERS)
            translate([c[0], c[1], -1])
                cylinder(h = LID_THICKNESS + 2, d = STANDOFF_ID + 0.6, $fn = 16);
    }
    // Retention bosses trapping the base board against the window from
    // above -- must reach the plate's underside (z=0) to fuse in the union,
    // same lesson as v0.1's LCD bosses (a short segment floats unconnected).
    BOSS_LEN = 6.0;
    for (c = DISP_CORNERS)
        translate([c[0], c[1], -BOSS_LEN])
            cylinder(h = BOSS_LEN + EPS2, d = 4, $fn = 16);
}

// ------------------------------------------------------------------ views --
if (part == "bottom") {
    bottom_shell();
} else if (part == "spacer") {
    spacer();
} else if (part == "top") {
    top_lid();
} else { // assembly preview only -- all 3 parts, stacked in their real Z order
    color("SteelBlue", 0.9) bottom_shell();
    // ghost daughterboard, real seat height
    %translate([0, 0, DB_SEAT_Z])
        linear_extrude(height = DB_THICKNESS)
            square([DB_W, DB_L]);
    color("LimeGreen", 0.9) translate([0, 0, DB_SEAT_Z + DB_THICKNESS])
        spacer();
    // ghost base board, real seat height
    %translate([DISP_CX - DISP_W/2, DISP_CY - DISP_L/2, DISP_SEAT_Z])
        linear_extrude(height = DISP_THICKNESS)
            square([DISP_W, DISP_L]);
    translate([0, 0, TIER1_H + 25])
        color("Orange", 0.9) top_lid();
}
