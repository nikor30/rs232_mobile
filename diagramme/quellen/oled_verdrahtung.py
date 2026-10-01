import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.lines import Line2D

fig, ax = plt.subplots(figsize=(10, 6.2), dpi=200)
ax.set_xlim(0, 10)
ax.set_ylim(0, 6.2)
ax.axis("off")

# ---- Title ----
ax.text(5, 5.95, "OLED 128×64 (I²C) am ESP32-DevKit-Mockup anschließen",
        ha="center", va="top", fontsize=15, fontweight="bold", color="#1a1a1a")
ax.text(5, 5.55, "Firmware-Env  esp32dev-max3232  –  Pins sind bereits fest in config.h reserviert",
        ha="center", va="top", fontsize=9.5, color="#555555", style="italic")

# ---- Board: ESP32 DevKit ----
board_x, board_y, board_w, board_h = 0.6, 1.0, 3.0, 3.6
board = mpatches.FancyBboxPatch((board_x, board_y), board_w, board_h,
                                 boxstyle="round,pad=0.02,rounding_size=0.08",
                                 linewidth=1.8, edgecolor="#2b6cb0", facecolor="#eaf2fb")
ax.add_patch(board)
ax.text(board_x + board_w/2, board_y + board_h - 0.28, "ESP32 DevKit\n(WROOM-32, 30-Pin)",
        ha="center", va="top", fontsize=11, fontweight="bold", color="#1a3a5c")
# small USB connector nub for orientation
usb = mpatches.FancyBboxPatch((board_x + board_w/2 - 0.35, board_y + board_h - 0.02), 0.7, 0.18,
                               boxstyle="round,pad=0.01,rounding_size=0.03",
                               linewidth=1.2, edgecolor="#2b6cb0", facecolor="#cfe0f5")
ax.add_patch(usb)
ax.text(board_x + board_w/2, board_y + board_h + 0.16, "USB-C", ha="center", va="bottom",
        fontsize=7.5, color="#2b6cb0")

pins_left = [
    ("3V3",  "3V3  →  Stromversorgung 3,3 V", "#d64545"),
    ("GND",  "GND  →  Masse",                  "#333333"),
    ("D21",  "D21 / GPIO21  →  SDA",            "#2f7a3d"),
    ("D22",  "D22 / GPIO22  →  SCL",            "#7a5cc9"),
]
pin_y = [board_y + 0.55 + i*0.85 for i in range(4)][::-1]

for (label, _, color), y in zip(pins_left, pin_y):
    ax.plot([board_x + board_w], [y], marker="s", markersize=7, color=color, zorder=5)
    ax.text(board_x + board_w - 0.12, y, label, ha="right", va="center",
            fontsize=10, fontweight="bold", color="#1a1a1a", zorder=6,
            bbox=dict(boxstyle="round,pad=0.12", facecolor="#eaf2fb", edgecolor="none"))

# ---- Board: OLED module ----
oled_x, oled_y, oled_w, oled_h = 6.9, 1.6, 2.5, 2.5
oled_board = mpatches.FancyBboxPatch((oled_x, oled_y), oled_w, oled_h,
                                      boxstyle="round,pad=0.02,rounding_size=0.08",
                                      linewidth=1.8, edgecolor="#946200", facecolor="#fff3df")
ax.add_patch(oled_board)
screen = mpatches.Rectangle((oled_x + 0.25, oled_y + 0.75), oled_w - 0.5, oled_h - 1.1,
                             linewidth=1.2, edgecolor="#946200", facecolor="#111111")
ax.add_patch(screen)
ax.text(oled_x + oled_w/2, oled_y + oled_h - 0.9, "128×64", ha="center", va="center",
        fontsize=9, color="#33ff66", fontfamily="monospace")
ax.text(oled_x + oled_w/2, oled_y + oled_h + 0.18, "OLED 128×64 I²C\n(SSD1306, 0.96\")",
        ha="center", va="bottom", fontsize=11, fontweight="bold", color="#5c3d00")

pins_right = [
    ("GND", "#333333"),
    ("VCC", "#d64545"),
    ("SCL", "#7a5cc9"),
    ("SDA", "#2f7a3d"),
]
pin_x = [oled_x + 0.35 + i*0.62 for i in range(4)]
for (label, color), x in zip(pins_right, pin_x):
    ax.plot([x], [oled_y], marker="s", markersize=7, color=color, zorder=5)
    ax.text(x, oled_y - 0.12, label, ha="center", va="top", fontsize=9.5,
            fontweight="bold", color="#1a1a1a", zorder=6,
            bbox=dict(boxstyle="round,pad=0.12", facecolor="#fff3df", edgecolor="none"))

ax.text(oled_x + oled_w/2, 5.04,
        "Pin-Reihenfolge je nach Modul anders –\nnach Beschriftung anschließen, nicht nach Position!",
        ha="center", va="center", fontsize=8, color="#8a4b00", style="italic")

# ---- Connecting wires (orthogonal routing) ----
color_map = {"3V3": "#d64545", "GND": "#333333", "D21": "#2f7a3d", "D22": "#7a5cc9"}
target_for = {"3V3": "VCC", "GND": "GND", "D21": "SDA", "D22": "SCL"}
right_pin_pos = {label: (x, oled_y) for (label, _), x in zip(pins_right, pin_x)}

for (label, _, color), y in zip(pins_left, pin_y):
    tx, ty = right_pin_pos[target_for[label]]
    midx = board_x + board_w + 0.5 + (list(color_map).index(label) * 0.18)
    ax.plot([board_x + board_w, midx, midx, tx, tx],
            [y, y, ty - 0.9 + list(color_map).index(label)*0.12, ty - 0.9 + list(color_map).index(label)*0.12, ty],
            color=color, linewidth=2.2, solid_capstyle="round", zorder=3)

# ---- Legend ----
legend_items = [
    Line2D([0], [0], color="#d64545", lw=2.5, label="VCC / 3,3 V"),
    Line2D([0], [0], color="#333333", lw=2.5, label="GND"),
    Line2D([0], [0], color="#2f7a3d", lw=2.5, label="SDA (Daten)"),
    Line2D([0], [0], color="#7a5cc9", lw=2.5, label="SCL (Takt)"),
]
ax.legend(handles=legend_items, loc="lower center", bbox_to_anchor=(0.5, -0.02),
          ncol=4, frameon=False, fontsize=9.5)

# ---- Footer note ----
ax.text(5, 0.35,
        "Firmware erkennt das Display beim Start automatisch (I²C-Adresse 0x3C oder 0x3D) – keine Code-Änderung nötig.",
        ha="center", va="center", fontsize=8.5, color="#555555")

plt.tight_layout()
plt.savefig("/tmp/claude-0/-home-claude/830cb3ec-f4fd-513a-bde5-dc117f387d2b/scratchpad/oled_diagram/oled_wiring.png",
            dpi=200, facecolor="white", bbox_inches="tight")
print("done")
