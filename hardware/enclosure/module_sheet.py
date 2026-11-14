#!/usr/bin/env python3
"""Measurement sheet of the Waveshare ESP32-S3-Touch-LCD-2.

Draws the module from the numbers in carrier/generate.py (WS), which were read
from the vendor's STEP model (esp32-s3-touch-lcd-2_20241108.stp) and agree with
the vendor's drawing.  Writes out/waveshare_module_dimensions.png and .pdf.
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, FancyBboxPatch, Rectangle

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "carrier"))
import generate as G  # noqa: E402

WS = G.WS
OUT = os.path.join(HERE, "out")


def dim(ax, p1, p2, text, off=0.0, side="h", fs=7.5, colour="#b3261e"):
    """A dimension line between two points on one axis, shifted sideways by off."""
    (x1, y1), (x2, y2) = p1, p2
    if side == "h":
        y = y1 + off
        for x, yy in ((x1, y1), (x2, y2)):
            ax.plot([x, x], [yy, y], color=colour, lw=0.4)
        ax.annotate("", (x1, y), (x2, y), arrowprops=dict(arrowstyle="<->", color=colour, lw=0.7, shrinkA=0, shrinkB=0))
        ax.text((x1 + x2) / 2, y + (0.45 if off >= 0 else -0.45), text, ha="center", va="bottom" if off >= 0 else "top", fontsize=fs, color=colour)
    else:
        x = x1 + off
        for xx, y in ((x1, y1), (x2, y2)):
            ax.plot([xx, x], [y, y], color=colour, lw=0.4)
        ax.annotate("", (x, y1), (x, y2), arrowprops=dict(arrowstyle="<->", color=colour, lw=0.7, shrinkA=0, shrinkB=0))
        ax.text(x + (0.45 if off >= 0 else -0.45), (y1 + y2) / 2, text, ha="left" if off >= 0 else "right", va="center", fontsize=fs, color=colour, rotation=90)


def rect(ax, b, **kw):
    ax.add_patch(Rectangle((b[0], b[1]), b[2] - b[0], b[3] - b[1], **kw))


def main():
    fig = plt.figure(figsize=(16.5, 11.7))
    a1 = fig.add_axes([0.04, 0.06, 0.44, 0.86])
    a2 = fig.add_axes([0.54, 0.68, 0.43, 0.24])
    a3 = fig.add_axes([0.54, 0.36, 0.43, 0.27])
    a4 = fig.add_axes([0.52, 0.04, 0.24, 0.27])
    a5 = fig.add_axes([0.76, 0.04, 0.22, 0.27])
    g, p = WS["glass"], WS["pcb"]

    # ------------------------------------------------ plan view, seen from the display side
    a1.add_patch(FancyBboxPatch((g[0] + 2.6, g[1] + 2.6), g[2] - g[0] - 5.2, g[3] - g[1] - 5.2, boxstyle=f"round,pad={WS['glass_r']}",
                                fc="#eef0f3", ec="k", lw=1.2))
    rect(a1, p, fc="#c9d6ee", ec="#1f3b73", lw=1.0)
    # lit view area: 30.99 x 41.20, 3.06 from the left glass edge, 7.51 from the antenna end
    va = (g[0] + 3.06, g[3] - 7.51 - 41.20, g[0] + 3.06 + 30.99, g[3] - 7.51)
    rect(a1, va, fc="none", ec="#555", lw=0.7, ls="--")
    a1.text((va[0] + va[2]) / 2, (va[1] + va[3]) / 2 + 8, "view area 30.99 x 41.20\n(240 x 320 px)", ha="center", fontsize=8, color="#555")
    for sx in (-1, 1):
        for i in range(14):
            a1.add_patch(Circle((sx * WS["hdr_x"], WS["hdr_y1"] - i * 2.54), 0.5, fc="#c9a227", ec="k", lw=0.3))
    a1.text(-WS["hdr_x"] + 1.2, WS["hdr_y1"], "1", fontsize=7, va="center")
    a1.text(-WS["hdr_x"] + 1.2, WS["hdr_y1"] - 13 * 2.54, "14", fontsize=7, va="center")
    a1.text(WS["hdr_x"] - 1.2, WS["hdr_y1"] - 13 * 2.54, "15", fontsize=7, va="center", ha="right")
    a1.text(WS["hdr_x"] - 1.2, WS["hdr_y1"], "28", fontsize=7, va="center", ha="right")
    for x, y in WS["standoffs"]:
        a1.add_patch(Circle((x, y), WS["standoff_d"] / 2, fc="none", ec="#7a5c00", lw=0.9))
        a1.add_patch(Circle((x, y), 1.0, fc="none", ec="#7a5c00", lw=0.5))
    sw_, sh_ = WS["usbc_shell"]
    rect(a1, (-sw_ / 2, WS["usbc_front_y"], sw_ / 2, WS["usbc"][3]), fc="#b9babd", ec="k", lw=0.8)
    rect(a1, (-WS["usbc_tongue"][0] / 2, WS["usbc_tongue"][2], WS["usbc_tongue"][0] / 2, WS["usbc_back_y"]), fc="#3a3a3d", ec="k", lw=0.4)
    a1.text(0, WS["usbc"][3] - 1.3, "USB-C", ha="center", fontsize=7)
    aw, ah, ap = WS["button_act"]
    for name, (x, y) in WS["buttons"].items():
        rect(a1, (x - WS["button_w"] / 2, y + ap, x + WS["button_w"] / 2, y + ap + WS["button_d"]), fc="#f1efe6", ec="k", lw=0.6)
        rect(a1, (x - aw / 2, y, x + aw / 2, y + ap), fc="#3a3a3d", ec="k", lw=0.4)
        a1.text(x, y + ap + WS["button_d"] + 0.5, name, ha="center", fontsize=6.5)
    rect(a1, WS["bat"], fc="#f1efe6", ec="k", lw=0.6)
    a1.text((WS["bat"][0] + WS["bat"][2]) / 2, WS["bat"][3] + 0.6, "battery\nMX1.25", ha="center", fontsize=6.5)
    rect(a1, WS["tf"], fc="#b9babd", ec="k", lw=0.6)
    a1.text((WS["tf"][0] + WS["tf"][2]) / 2, (WS["tf"][1] + WS["tf"][3]) / 2, "micro-SD", ha="center", va="center", fontsize=6.5)
    rect(a1, WS["cam"], fc="#3a3a3d", ec="k", lw=0.6)
    a1.text(0, (WS["cam"][1] + WS["cam"][3]) / 2, "camera FPC", ha="center", va="center", fontsize=6.5, color="w")
    a1.text(0, p[3] + 2.4, "chip antenna at this end of the PCB", ha="center", fontsize=7, color="#1f3b73")
    a1.plot([0, 0], [g[1] - 2, g[3] + 2], color="#888", lw=0.4, ls="-.")
    a1.plot([g[0] - 2, g[2] + 2], [0, 0], color="#888", lw=0.4, ls="-.")
    a1.text(g[2] + 2.2, 0.4, "y = 0", fontsize=6, color="#888")

    dim(a1, (g[0], g[3]), (g[2], g[3]), f"glass {g[2] - g[0]:.2f}  (R {WS['glass_r']:.2f})", off=8.5)
    dim(a1, (p[0], p[3]), (p[2], p[3]), f"PCB {p[2] - p[0]:.2f}", off=9.0)
    dim(a1, (-WS["hdr_x"], WS["hdr_y1"]), (WS["hdr_x"], WS["hdr_y1"]), f"pin rows {2 * WS['hdr_x']:.2f}", off=2.6)
    dim(a1, (g[2], g[1]), (g[2], g[3]), f"glass {g[3] - g[1]:.2f}", off=10.5, side="v")
    dim(a1, (p[2], p[1]), (p[2], p[3]), f"PCB {p[3] - p[1]:.2f}", off=6.2, side="v")
    so = WS["standoffs"]
    dim(a1, (so[0][0], so[0][1]), (so[1][0], so[1][1]), f"standoffs M2  {so[1][0] - so[0][0]:.2f}", off=-11.5)
    dim(a1, (so[0][0], so[0][1]), (so[2][0], so[2][1]), f"standoffs {so[2][1] - so[0][1]:.2f}", off=-7.2, side="v")
    dim(a1, (g[0], g[1]), (g[0], p[1]), f"{p[1] - g[1]:.2f}", off=-3.0, side="v")
    dim(a1, (g[0], p[3]), (g[0], g[3]), f"{g[3] - p[3]:.2f}", off=-3.0, side="v")
    dim(a1, (g[0], -22.0), (p[0], -22.0), f"{p[0] - g[0]:.2f}", off=-13.5)
    dim(a1, (-WS["hdr_x"], p[3]), (-WS["hdr_x"], WS["hdr_y1"]), f"pin 1: {p[3] - WS['hdr_y1']:.2f}", off=-9.0, side="v")
    dim(a1, (WS["hdr_x"], WS["hdr_y1"] - 13 * 2.54), (WS["hdr_x"], WS["hdr_y1"]), "13 x 2.54 = 33.02", off=-4.6, side="v")
    dim(a1, (-sw_ / 2, WS["usbc"][1]), (sw_ / 2, WS["usbc"][1]), f"USB-C shell {sw_:.2f}", off=-3.4)
    bx = [v[0] for v in WS["buttons"].values()]
    dim(a1, (min(bx), WS["buttons"]["BOOT"][1]), (max(bx), WS["buttons"]["RST"][1]), f"plungers {max(bx) - min(bx):.2f} apart", off=7.0, fs=6.5)
    a1.annotate(f"USB-C {p[1] - WS['usbc'][1]:.2f} beyond the PCB edge", (WS["usbc"][2], WS["usbc"][1] + 0.3), (g[2] - 6, g[1] - 2.2),
                arrowprops=dict(arrowstyle="->", lw=0.5), fontsize=7)
    a1.set_xlim(g[0] - 15, g[2] + 15); a1.set_ylim(g[1] - 8, g[3] + 12); a1.set_aspect("equal"); a1.axis("off")
    a1.set_title("Seen from the display side, through the glass (USB-C at the bottom; the parts sit on the far side of the PCB). Dimensions in mm.", fontsize=9)

    # ------------------------------------------------ side view along the length (y to the right, z up)
    def layer(ax, b, z, i1, i2, **kw):
        ax.add_patch(Rectangle((b[i1], z[0]), b[i2] - b[i1], z[1] - z[0], **kw))
    for ax, i1, i2 in ((a2, 1, 3), (a3, 0, 2)):
        layer(ax, WS["glass"], WS["glass_z"], i1, i2, fc="#1c1d22", ec="k", lw=0.8)
        layer(ax, WS["tp_sensor"], WS["tp_sensor_z"], i1, i2, fc="#55565c", ec="k", lw=0.5)
        layer(ax, WS["lcm"], WS["lcm_z"], i1, i2, fc="#b9babd", ec="k", lw=0.8)
        layer(ax, WS["pmma"], WS["pmma_z"], i1, i2, fc="#dfe8ef", ec="k", lw=0.8)
        layer(ax, WS["pcb"], WS["pcb_z"], i1, i2, fc="#1f3b73", ec="k", lw=0.8)
        ax.set_aspect("equal"); ax.axis("off")
    hz, tip = WS["hdr_plastic_z"], WS["hdr_pin_tip_z"]
    a2.add_patch(Rectangle((WS["hdr_y1"] - 13 * 2.54 - 1.27, hz[0]), 14 * 2.54, hz[1] - hz[0], fc="#2a2a2d", ec="k", lw=0.6))
    for i in range(14):
        a2.add_patch(Rectangle((WS["hdr_y1"] - i * 2.54 - 0.32, tip), 0.64, hz[0] - tip, fc="#c9a227", ec="k", lw=0.3))
    a2.add_patch(Rectangle((WS["usbc_front_y"], WS["usbc_axis_z"] - sh_ / 2), WS["usbc"][3] - WS["usbc_front_y"], sh_, fc="#b9babd", ec="k", lw=0.6))
    for x, y in WS["standoffs"][::2]:
        a2.add_patch(Rectangle((y - WS["standoff_d"] / 2, WS["standoff_z"][0]), WS["standoff_d"], WS["standoff_z"][1] - WS["standoff_z"][0], fc="#c9a227", ec="k", lw=0.5))
    a2.text(WS["usbc"][1] - 0.6, WS["usbc_z"][0] - 1.6, "USB-C", fontsize=7)
    a2.set_title("Side view along the length (USB-C left, antenna right)", fontsize=10)
    gz, pz = WS["glass_z"], WS["pcb_z"]
    dim(a2, (g[3], pz[0]), (g[3], gz[1]), f"{gz[1] - pz[0]:.2f}", off=3.0, side="v")
    dim(a2, (g[3], hz[0]), (g[3], gz[1]), f"{gz[1] - hz[0]:.2f}", off=7.0, side="v")
    dim(a2, (g[3], tip), (g[3], gz[1]), f"{gz[1] - tip:.2f} to the pin tips", off=11.0, side="v")
    dim(a2, (g[1], gz[0]), (g[1], gz[1]), f"glass {gz[1] - gz[0]:.2f}", off=-3.0, side="v", fs=7)
    dim(a2, (g[1], pz[0]), (g[1], pz[1]), f"PCB {pz[1] - pz[0]:.2f}", off=-7.0, side="v", fs=7)
    dim(a2, (g[1], gz[1]), (p[1], gz[1]), f"{p[1] - g[1]:.2f}", off=2.2)
    dim(a2, (p[3], gz[1]), (g[3], gz[1]), f"{g[3] - p[3]:.2f}", off=2.2)
    a2.text(0, 1.0, "LCD 2.10", ha="center", va="center", fontsize=7)
    a2.text(0, -1.0, "acrylic spacer 2.00", ha="center", va="center", fontsize=7)
    a2.text(0, 3.05, "cover glass 1.10, touch film 0.40 below", ha="center", va="center", fontsize=6.5, color="w")
    a2.text(0, hz[0] + 1.2, "pin strip body 2.50", ha="center", va="center", fontsize=6.5, color="w")
    a2.set_xlim(g[1] - 14, g[3] + 20); a2.set_ylim(tip - 3, gz[1] + 6)

    # ------------------------------------------------ end view from the USB-C end: receptacle and the two switches
    for ax in (a3,):
        pass
    for sx in (-1, 1):
        a3.add_patch(Rectangle((sx * WS["hdr_x"] - 1.27, hz[0]), 2.54, hz[1] - hz[0], fc="#2a2a2d", ec="k", lw=0.6))
        a3.add_patch(Rectangle((sx * WS["hdr_x"] - 0.32, tip), 0.64, hz[0] - tip, fc="#c9a227", ec="k", lw=0.3))
    az = WS["usbc_axis_z"]
    (ow, oh), (tw, tt, _) = WS["usbc_open"], WS["usbc_tongue"]
    a3.add_patch(FancyBboxPatch((-sw_ / 2 + sh_ / 2, az), sw_ - sh_, 0.0001, boxstyle=f"round,pad={sh_ / 2}", fc="#b9babd", ec="k", lw=0.8))
    a3.add_patch(FancyBboxPatch((-ow / 2 + oh / 2, az), ow - oh, 0.0001, boxstyle=f"round,pad={oh / 2}", fc="#ffffff", ec="k", lw=0.6))
    a3.add_patch(Rectangle((-tw / 2, az - tt / 2), tw, tt, fc="#3a3a3d", ec="k", lw=0.4))
    for name, (x, y) in WS["buttons"].items():
        a3.add_patch(Rectangle((x - WS["button_w"] / 2, WS["button_z"][0]), WS["button_w"], WS["button_z"][1] - WS["button_z"][0], fc="#f1efe6", ec="k", lw=0.6))
        a3.add_patch(Rectangle((x - aw / 2, WS["button_act_z"] - ah / 2), aw, ah, fc="#3a3a3d", ec="k", lw=0.4))
        a3.text(x, WS["button_z"][0] - 0.9, name, ha="center", fontsize=7)
    dim(a3, (-ow / 2, az), (ow / 2, az), f"opening {ow:.2f} x {oh:.2f}, shell {sw_:.2f} x {sh_:.2f}", off=-3.4, fs=7)
    dim(a3, (WS["buttons"]["BOOT"][0], az), (0, az), f"{-WS['buttons']['BOOT'][0]:.2f}", off=-7.0, fs=7)
    dim(a3, (0, az), (WS["buttons"]["RST"][0], az), f"{WS['buttons']['RST'][0]:.2f}", off=-7.0, fs=7)
    dim(a3, (g[2], az), (g[2], pz[0]), f"port axis {pz[0] - az:.2f} below the PCB", off=3.0, side="v", fs=6.5)
    dim(a3, (g[0], WS["button_act_z"]), (g[0], pz[0]), f"plunger axis {pz[0] - WS['button_act_z']:.2f}", off=-3.0, side="v", fs=6.5)
    dim(a3, (g[0], gz[1]), (g[2], gz[1]), f"glass {g[2] - g[0]:.2f}", off=2.0)
    a3.annotate(f"plunger {aw:.1f} x {ah:.1f}", (WS["buttons"]["BOOT"][0] - aw / 2, WS["button_act_z"]), (g[0] - 11.5, WS["button_act_z"] - 5.2), arrowprops=dict(arrowstyle="->", lw=0.5), fontsize=7)
    a3.set_xlim(g[0] - 12, g[2] + 16); a3.set_ylim(tip - 0.5, gz[1] + 5.5)
    a3.set_title("End view from the USB-C end: receptacle and the RST / BOOT switches", fontsize=10)

    # ------------------------------------------------ the USB-C end from the side, enlarged: receptacle, switch
    yf, yb = WS["usbc_front_y"], WS["usbc_back_y"]
    by = WS["buttons"]["RST"][1]
    for ax in (a4, a5):
        layer(ax, WS["glass"], WS["glass_z"], 1, 3, fc="#1c1d22", ec="k", lw=0.8)
        layer(ax, WS["tp_sensor"], WS["tp_sensor_z"], 1, 3, fc="#55565c", ec="k", lw=0.5)
        layer(ax, WS["lcm"], WS["lcm_z"], 1, 3, fc="#b9babd", ec="k", lw=0.8)
        layer(ax, WS["pmma"], WS["pmma_z"], 1, 3, fc="#dfe8ef", ec="k", lw=0.8)
        layer(ax, WS["pcb"], WS["pcb_z"], 1, 3, fc="#1f3b73", ec="k", lw=0.8)
        ax.set_xlim(g[1] - 3.5, -17.0); ax.set_ylim(az - 8.2, gz[1] + 3.6); ax.set_aspect("equal"); ax.axis("off")
        dim(ax, (g[1], gz[1]), (p[1], gz[1]), f"glass {p[1] - g[1]:.2f} past the PCB", off=1.4, fs=7)
    a4.add_patch(Rectangle((yf, az - sh_ / 2), WS["usbc"][3] - yf, sh_, fc="#b9babd", ec="k", lw=0.8))
    a4.add_patch(Rectangle((yf, az - oh / 2), yb - yf, oh, fc="#ffffff", ec="k", lw=0.5))
    a4.add_patch(Rectangle((WS["usbc_tongue"][2], az - tt / 2), yb - WS["usbc_tongue"][2], tt, fc="#3a3a3d", ec="k", lw=0.4))
    dim(a4, (yf, az - sh_ / 2), (p[1], az - sh_ / 2), f"{p[1] - yf:.2f}", off=-1.5, fs=7)
    dim(a4, (yf, az - sh_ / 2), (yb, az - sh_ / 2), f"plug shell goes in {yb - yf:.2f}", off=-3.4, fs=7)
    dim(a4, (g[1], az), (yf, az), f"{yf - g[1]:.2f}", off=-5.6, fs=7)
    a4.text(g[1] - 0.4, az - 7.9, "rim 1.18 past the PCB edge, 3.90 behind the glass edge", fontsize=7, color="#b3261e")
    a4.set_title("USB-C receptacle, section", fontsize=10)
    a5.add_patch(Rectangle((by + ap, WS["button_z"][0]), WS["button_d"], WS["button_z"][1] - WS["button_z"][0], fc="#f1efe6", ec="k", lw=0.6))
    a5.add_patch(Rectangle((by, WS["button_act_z"] - ah / 2), ap, ah, fc="#3a3a3d", ec="k", lw=0.4))
    dim(a5, (by, WS["button_z"][0]), (p[1], WS["button_z"][0]), f"{by - p[1]:.2f}", off=-1.5, fs=7)
    dim(a5, (by, WS["button_z"][0]), (by + ap, WS["button_z"][0]), f"{ap:.2f}", off=-3.4, fs=7)
    dim(a5, (g[1], WS["button_act_z"]), (by, WS["button_act_z"]), f"{by - g[1]:.2f}", off=-5.6, fs=7)
    a5.text(g[1] - 0.4, az - 7.9, "plunger tip 0.28 inside the PCB edge, 1.20 proud,\n5.36 behind the glass edge; pushed to the right", fontsize=7, color="#b3261e")
    a5.set_title("RST / BOOT switch, section", fontsize=10)

    fig.suptitle("Waveshare ESP32-S3-Touch-LCD-2: dimensions from the vendor's STEP model (file of 2024-11-08)", fontsize=12, y=0.985)
    os.makedirs(OUT, exist_ok=True)
    fig.savefig(os.path.join(OUT, "waveshare_module_dimensions.png"), dpi=130)
    fig.savefig(os.path.join(OUT, "waveshare_module_dimensions.pdf"))
    print("wrote waveshare_module_dimensions.png/.pdf")


if __name__ == "__main__":
    main()
