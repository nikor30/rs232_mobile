// ============================================================================
//  RS232 Web Console - 3D-printable enclosure (parametric, OpenSCAD)
//  LilyGO T-RSS3 + 1S LiPo + charge/boost module + 0.96"/1.3" OLED
//
//  !!! All part dimensions below are ESTIMATES. Measure your parts with a
//  !!! caliper and adjust the "MEASURE" values before printing.
//
//  Stack (bottom -> top):  floor | LiPo + charger module | T-RSS3 on side
//  ledges | OLED under the lid.  DB9 through the front wall.
//
//  Export:  part = "body";  -> F6 -> Export STL
//           part = "lid";   -> F6 -> Export STL
//  Print: PETG/PLA, 0.2 mm layers, 3 walls, no supports (lid upside down).
// ============================================================================

part = "assembly";   // [assembly, body, lid]

/* [T-RSS3 board - MEASURE] */
pcb_w      = 55.0;   // X, left-right, DB9 edge facing you
pcb_d      = 52.0;   // Y, front (DB9 edge) to back
pcb_t      = 1.6;
comp_h     = 13.0;   // tallest part above PCB top (RSM modules / DB9 / terminals)
under_h    = 2.5;    // solder joints / pins below the PCB
db9_x      = 35.0;   // DB9 centre, measured from LEFT PCB edge
db9_z      = 6.3;    // DB9 centre above PCB TOP surface
db9_front  = 1.0;    // how far the DB9 flange sticks out past the PCB front edge

/* [Battery + charge/boost module - MEASURE] */
bat_l      = 50.0;   // 103450 LiPo (2000 mAh): 50 x 34 x 10 mm
bat_w      = 34.0;
bat_t      = 10.5;   // incl. protection PCB / tape
chg_l      = 25.0;   // module along the LEFT wall (Y)
chg_w      = 21.0;   // module depth into the box (X)
chg_t      = 5.0;    // incl. USB-C socket
chg_usb_z  = 2.4;    // USB-C centre above module bottom
chg_usb_y  = 12.5;   // USB-C centre from the front inner wall

/* [OLED module - MEASURE] */
oled_pcb_w = 27.8;   // 0.96" module: 27.3 x 27.8 ; 1.3" module: 35.4 x 33.5
oled_pcb_h = 27.8;
oled_win_w = 23.5;   // visible glass area (+ margin)
oled_win_h = 12.5;
oled_win_dy= 5.0;    // glass window starts this far from the module's top (pin) edge ... see preview
oled_stack = 4.0;    // module thickness incl. soldered wires
oled_cx    = 0;      // module centre offset from lid centre (X)
oled_cy    = 4;      // module centre offset from lid centre (Y, + = towards back)

/* [Controls] */
btn_d      = 7.2;    // 7 mm panel push button (front wall, below the DB9)
btn_x      = 44.0;   // from inner left wall
btn_z      = 7.0;    // centre above floor top
sw_slot_l  = 7.5;    // SS12D00 slide switch: lever slot in the right wall
sw_slot_h  = 3.6;
sw_y       = 12.0;   // from inner front wall
sw_z       = 6.0;    // centre above floor top
sw_hole_dx = 0;      // set to 15 for panel switches with 2 x M2 holes, 0 = glue in

/* [Enclosure] */
wall       = 2.0;
floor_t    = 2.0;
lid_t      = 2.0;
clr        = 0.4;    // clearance around parts
ledge_w    = 2.0;    // PCB support ledges on the side walls
corner_r   = 3.0;
screw_d    = 2.2;    // pilot hole for M2.5 x 6 self-tapping screws (lid)
screw_head = 5.0;
lip_h      = 3.0;    // lid lip that sits inside the walls

$fn = 48;

// ---------------------------------------------------------------- derived
in_w  = max(pcb_w, bat_l + 2) + 2 * clr;
in_d  = max(pcb_d + db9_front, bat_w + chg_l + 3) + 2 * clr;
pcb_z = floor_t + max(bat_t, chg_t) + under_h + 0.5;    // PCB bottom (absolute)
pcb_top = pcb_z + pcb_t;
body_h = pcb_top + comp_h + oled_stack + 1.0;            // top edge of the walls
out_w = in_w + 2 * wall;
out_d = in_d + 2 * wall;
pcb_x0 = wall + (in_w - pcb_w) / 2;                      // PCB left edge (absolute)
pcb_y0 = wall + db9_front + clr;                         // PCB front edge (absolute)

echo(str("Enclosure outside: ", out_w, " x ", out_d, " x ", body_h + lid_t, " mm"));
echo(str("PCB bottom at z = ", pcb_z, " mm"));

module rbox(w, d, h, r) {
  hull() for (x = [r, w - r], y = [r, d - r]) translate([x, y, 0]) cylinder(r = r, h = h);
}

module rrect(w, h, r, t) {   // rounded rectangle in the XZ plane, extruded along Y by t
  rotate([90, 0, 0]) linear_extrude(t, center = true)
    offset(r) square([w - 2 * r, h - 2 * r], center = true);
}

// ---------------------------------------------------------------- body
module body() {
  difference() {
    union() {
      difference() {
        rbox(out_w, out_d, body_h, corner_r);
        translate([wall, wall, floor_t]) cube([in_w, in_d, body_h]);
      }
      // PCB ledges (left, right, back)
      for (x = [wall, wall + in_w - ledge_w])
        translate([x, wall + 8, pcb_z - 2]) cube([ledge_w, in_d - 8, 2]);
      translate([wall, wall + in_d - ledge_w, pcb_z - 2]) cube([in_w, ledge_w, 2]);
      // lid screw blocks in the top corners (above all components)
      for (x = [wall, wall + in_w - 6], y = [wall, wall + in_d - 6])
        translate([x, y, body_h - 5]) cube([6, 6, 5]);
    }
    // lid screw pilot holes
    for (x = [wall + 3, wall + in_w - 3], y = [wall + 3, wall + in_d - 3])
      translate([x, y, body_h - 4.5]) cylinder(d = screw_d, h = 5);

    // DB9 (female on the board): D cut-out + 2 jack screw holes, front wall
    translate([pcb_x0 + db9_x, wall / 2, pcb_top + db9_z]) {
      rrect(20.5, 11.5, 1.5, wall + 2);
      for (dx = [-12.5, 12.5]) translate([dx, 0, 0]) rotate([90, 0, 0]) cylinder(d = 3.4, h = wall + 2, center = true);
    }
    // USB-C of the charge module, left wall
    translate([wall / 2, wall + clr + chg_usb_y, floor_t + chg_usb_z])
      rotate([0, 0, 90]) rrect(9.8, 4.0, 1.6, wall + 2);
    // push button, front wall (below the PCB)
    translate([wall + btn_x, wall / 2, floor_t + btn_z])
      rotate([90, 0, 0]) cylinder(d = btn_d, h = wall + 2, center = true);
    // slide switch slot (+ optional screw holes), right wall
    translate([out_w - wall / 2, wall + sw_y, floor_t + sw_z]) {
      rotate([0, 0, 90]) rrect(sw_slot_l, sw_slot_h, 0.8, wall + 2);
      if (sw_hole_dx > 0) for (dy = [-sw_hole_dx / 2, sw_hole_dx / 2])
        translate([0, dy, 0]) rotate([0, 90, 0]) cylinder(d = 2.2, h = wall + 2, center = true);
    }
    // vent slots in the back wall (charger / LiPo)
    for (i = [0 : 4]) translate([out_w / 2 - 14 + i * 7, out_d - wall / 2, floor_t + 4])
      rrect(3, 7, 1.4, wall + 2);
  }
}

// ---------------------------------------------------------------- lid
module lid() {
  ow = oled_pcb_w + 2 * clr;
  oh = oled_pcb_h + 2 * clr;
  cx = out_w / 2 + oled_cx;
  cy = out_d / 2 + oled_cy;
  difference() {
    union() {
      rbox(out_w, out_d, lid_t, corner_r);
      // lip
      translate([wall + clr, wall + clr, -lip_h]) difference() {
        cube([in_w - 2 * clr, in_d - 2 * clr, lip_h]);
        translate([1.6, 1.6, -1]) cube([in_w - 2 * clr - 3.2, in_d - 2 * clr - 3.2, lip_h + 2]);
        // clear the screw blocks
        for (x = [-1, in_w - 2 * clr - 7], y = [-1, in_d - 2 * clr - 7]) translate([x, y, -1]) cube([8, 8, lip_h + 2]);
      }
      // OLED locating frame on the underside
      translate([cx - ow / 2 - 1.2, cy - oh / 2 - 1.2, -1.5]) difference() {
        cube([ow + 2.4, oh + 2.4, 1.5]);
        translate([1.2, 1.2, -1]) cube([ow, oh, 3]);
        translate([1.2 + ow / 2 - 7, oh, -1]) cube([14, 4, 3]);   // gap for the pin header / wires
      }
    }
    // OLED window (module pins towards the back)
    translate([cx - oled_win_w / 2, cy + oh / 2 - oled_win_dy - oled_win_h, -2]) cube([oled_win_w, oled_win_h, lid_t + 4]);
    // screw holes + countersink
    for (x = [wall + 3, wall + in_w - 3], y = [wall + 3, wall + in_d - 3]) translate([x, y, -5]) {
      cylinder(d = 2.9, h = 10);
      translate([0, 0, 5 + lid_t - 1.4]) cylinder(d1 = 2.9, d2 = screw_head, h = 1.41);
    }
    // engraved label
    translate([out_w / 2, wall + 7, lid_t - 0.6]) linear_extrude(1)
      text("RS232", size = 5, halign = "center", valign = "center", font = "Liberation Sans:style=Bold");
  }
}

// ---------------------------------------------------------------- dummies for the preview
module dummies() {
  // PCB
  color("#1a1a1a") translate([pcb_x0, pcb_y0, pcb_z]) cube([pcb_w, pcb_d - db9_front, pcb_t]);
  color("#333") translate([pcb_x0 + 3, pcb_y0 + 12, pcb_top]) cube([pcb_w - 6, 24, comp_h - 1]);
  color("silver") translate([pcb_x0 + db9_x - 15.4, wall + clr, pcb_top]) cube([30.8, 10, 12.5]);
  // LiPo + charger
  color("#3b6fb6") translate([wall + clr, wall + in_d - bat_w - clr, floor_t]) cube([bat_l, bat_w, bat_t]);
  color("#2e7d32") translate([wall + clr, wall + clr, floor_t]) cube([chg_w, chg_l, chg_t]);
}

if (part == "body") body();
else if (part == "lid") translate([0, out_d, lid_t]) rotate([180, 0, 0]) lid();
else {
  body();
  %dummies();
  translate([0, 0, body_h + 12]) lid();
}
