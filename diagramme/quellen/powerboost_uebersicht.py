import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mp
from matplotlib.lines import Line2D

RED, BLK, GRN, ORA, VIO, MUT = "#d64545", "#333333", "#2f7a3d", "#b45309", "#7a5cc9", "#6b7280"
BLUE = "#2b6cb0"
fig, ax = plt.subplots(figsize=(12, 8.2), dpi=200)
ax.set_xlim(0, 12); ax.set_ylim(0, 8.2); ax.axis("off")

ax.text(6, 8.0, "PowerBoost 1000C am ESP32-DevKit-Mockup", ha="center", va="top",
        fontsize=16.5, fontweight="bold", color="#111")
ax.text(6, 7.62, "LiPo laden, 5 V erzeugen, Akkuwarnung an den ESP32 melden",
        ha="center", va="top", fontsize=10.5, color=MUT, style="italic")

# ---- Warnung ----
ax.add_patch(mp.FancyBboxPatch((0.4, 6.62), 11.2, 0.62, boxstyle="round,pad=0.05,rounding_size=0.08",
                               linewidth=1.6, edgecolor="#c05621", facecolor="#fff5eb"))
ax.text(6, 6.93, "Nur LiPo / Li-Ion mit EINER Zelle. Das 4,8-V-NiCd-Pack aus dem bisherigen Aufbau "
                 "darf hier nicht dran – geladen wird fest auf 4,2 V.",
        ha="center", va="center", fontsize=10, color="#7c2d12")

# ---- Akku ----
ax.add_patch(mp.FancyBboxPatch((0.4, 4.45), 1.75, 1.0, boxstyle="round,pad=0.02,rounding_size=0.1",
                               linewidth=1.8, edgecolor="#8a6d1f", facecolor="#fdf3d4"))
ax.text(1.27, 5.18, "LiPo 1S", ha="center", fontsize=11.5, fontweight="bold", color="#5c4708")
ax.text(1.27, 4.90, "3,7 V, z. B. 2500 mAh", ha="center", fontsize=8.2, color="#5c4708")
ax.text(1.27, 4.63, "JST-PH", ha="center", fontsize=8.2, color=MUT, style="italic")

# ---- PowerBoost ----
ax.add_patch(mp.FancyBboxPatch((3.3, 3.5), 3.0, 2.3, boxstyle="round,pad=0.02,rounding_size=0.1",
                               linewidth=2, edgecolor=BLUE, facecolor="#e8f0fb"))
ax.text(4.8, 5.62, "PowerBoost 1000C", ha="center", fontsize=12.5, fontweight="bold", color="#14365e")
ax.text(4.8, 5.34, "TPS61090 · 5,2 V / 1 A", ha="center", fontsize=9, color=MUT)
ax.text(3.42, 4.95, "BAT", ha="left", va="center", fontsize=9.5, fontweight="bold", color="#8a6d1f")
ax.plot([2.15, 3.3], [4.95, 4.95], color="#8a6d1f", lw=2.6, solid_capstyle="round")

# Lade-USB unten links am Board
ax.add_patch(mp.FancyBboxPatch((3.15, 3.72), 0.34, 0.42, boxstyle="round,pad=0.01,rounding_size=0.04",
                               linewidth=1.3, edgecolor=BLUE, facecolor="#c9dcf5"))
ax.text(3.05, 3.93, "Micro-USB\nzum Laden", ha="right", va="center", fontsize=8.2, color=BLUE)

pb_pins = [("5Vo", 5.15, RED), ("GND", 4.75, BLK), ("LBO", 4.35, GRN), ("EN", 3.95, ORA)]
for name, y, col in pb_pins:
    ax.add_patch(plt.Circle((6.3, y), 0.095, facecolor=col, edgecolor="#222", linewidth=1.1, zorder=5))
    ax.text(6.14, y, name, ha="right", va="center", fontsize=9.5, fontweight="bold", color=col)

# ---- DevKit ----
ax.add_patch(mp.FancyBboxPatch((8.8, 3.5), 2.8, 2.3, boxstyle="round,pad=0.02,rounding_size=0.1",
                               linewidth=2, edgecolor=BLUE, facecolor="#eaf2fb"))
ax.text(10.2, 5.62, "ESP32 DevKit", ha="center", fontsize=12.5, fontweight="bold", color="#14365e")
ax.text(10.2, 5.34, "WROOM-32, 30 Pin", ha="center", fontsize=9, color=MUT)
for name, y, col in [("VIN", 5.15, RED), ("GND", 4.75, BLK), ("D23", 4.35, GRN)]:
    ax.add_patch(plt.Circle((8.8, y), 0.095, facecolor=col, edgecolor="#222", linewidth=1.1, zorder=5))
    ax.text(8.96, y, name, ha="left", va="center", fontsize=9.5, fontweight="bold", color=col)

for y, col, lab in [(5.15, RED, "5,2 V"), (4.75, BLK, "Masse"), (4.35, GRN, "LBO: leer unter 3,2 V")]:
    ax.plot([6.3, 8.8], [y, y], color=col, lw=2.6, solid_capstyle="round", zorder=3)
    ax.text(7.55, y + 0.09, lab, ha="center", va="bottom", fontsize=8.8, color=col,
            bbox=dict(boxstyle="round,pad=0.12", facecolor="white", edgecolor="none"))

# ---- Schalter an EN (nach unten weg, kollidiert mit nichts) ----
ax.plot([6.3, 7.0], [3.95, 3.95], color=ORA, lw=2.6, solid_capstyle="round")
ax.plot([7.0, 7.32], [3.95, 4.16], color=ORA, lw=2.6, solid_capstyle="round")
ax.plot([7.32, 7.62], [3.95, 3.95], color=ORA, lw=2.6, solid_capstyle="round")
ax.plot([7.62, 7.62], [3.95, 3.62], color=ORA, lw=2.6, solid_capstyle="round")
for w, yy in [(0.22, 3.62), (0.14, 3.54), (0.07, 3.46)]:
    ax.plot([7.62 - w, 7.62 + w], [yy, yy], color=ORA, lw=2.2)
ax.text(7.31, 3.28, "Ein/Aus: Schalter von EN nach GND", ha="center", va="top", fontsize=8.8, color=ORA)

# ---- optionaler Spannungsteiler, klar abgesetzt ----
ax.add_patch(mp.FancyBboxPatch((0.4, 0.55), 11.2, 2.3, boxstyle="round,pad=0.05,rounding_size=0.08",
                               linewidth=1.3, edgecolor="#d7dbe0", facecolor="#fbfbfd"))
ax.text(0.7, 2.62, "Optional: Ladezustand in Prozent", fontsize=10.5, fontweight="bold",
        va="center", color="#111")
ax.text(0.7, 2.30, "Ohne Teiler warnt das Gerät nur über LBO (leer / nicht leer). Mit Teiler zeigt es Prozent und Millivolt – "
                   "bei LiPo aussagekräftig, anders als beim flachen NiCd-Verlauf.",
        fontsize=9, va="center", color="#333")

bx = 2.2
ax.text(bx, 1.80, "LiPo-Pin", ha="right", va="center", fontsize=9, fontweight="bold", color=VIO)
ax.plot([bx, bx + 0.5], [1.80, 1.80], color=VIO, lw=2.2, solid_capstyle="round")
ax.plot([bx + 0.5, bx + 0.5], [1.80, 1.55], color=VIO, lw=2.2)
ax.add_patch(mp.Rectangle((bx + 0.34, 1.18), 0.32, 0.37, linewidth=1.5, edgecolor=VIO, facecolor="white"))
ax.text(bx + 0.78, 1.36, "100 k", ha="left", va="center", fontsize=8.5, color=VIO)
ax.plot([bx + 0.5, bx + 0.5], [1.18, 1.00], color=VIO, lw=2.2)
ax.add_patch(mp.Rectangle((bx + 0.34, 0.63), 0.32, 0.37, linewidth=1.5, edgecolor=VIO, facecolor="white"))
ax.text(bx + 0.78, 0.81, "100 k", ha="left", va="center", fontsize=8.5, color=VIO)
for w, yy in [(0.2, 0.58), (0.13, 0.51), (0.06, 0.44)]:
    ax.plot([bx + 0.5 - w, bx + 0.5 + w], [yy, yy], color=BLK, lw=2.0)
# Mittelabgriff nach rechts
ax.plot([bx + 0.5, bx + 1.1], [1.09, 1.09], color=VIO, lw=2.2, solid_capstyle="round")
ax.plot([bx + 1.1, 8.8], [1.09, 1.09], color=VIO, lw=2.2, solid_capstyle="round")
ax.add_patch(plt.Circle((8.8, 1.09), 0.095, facecolor=VIO, edgecolor="#222", linewidth=1.1, zorder=5))
ax.text(8.96, 1.09, "D35  (ADC1)", ha="left", va="center", fontsize=9.5, fontweight="bold", color=VIO)
ax.text(6.7, 1.58, "Mittelabgriff \u2192 D35.  Im Web-UI dann: Menue \u2192 Ports \u2192 Akku-Messung: D35, LiPo 1 Zelle, 2:1",
        ha="center", va="bottom", fontsize=8.8, color=VIO)

ax.legend(handles=[Line2D([0],[0],color=RED,lw=2.6,label="5 V"),
                   Line2D([0],[0],color=BLK,lw=2.6,label="Masse"),
                   Line2D([0],[0],color=GRN,lw=2.6,label="LBO – Akkuwarnung"),
                   Line2D([0],[0],color=ORA,lw=2.6,label="EN – Ein/Aus"),
                   Line2D([0],[0],color=VIO,lw=2.6,label="Spannungsmessung")],
          loc="lower center", bbox_to_anchor=(0.5, -0.035), ncol=5, frameon=False, fontsize=9.3)

plt.tight_layout()
plt.savefig("/tmp/claude-0/-home-claude/830cb3ec-f4fd-513a-bde5-dc117f387d2b/scratchpad/pb/powerboost.png",
            dpi=200, facecolor="white", bbox_inches="tight")
print("done")
