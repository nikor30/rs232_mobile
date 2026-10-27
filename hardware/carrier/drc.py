#!/usr/bin/env python3
"""Fill the zones, run KiCad's DRC and export the fabrication files.

    python3.12 drc.py            # needs the pcbnew module of the installed KiCad (7.0)

Outputs (in kicad/fab/): gerbers + drill (zip for PCBWay), pick-and-place CSV,
DRC report, schematic PDF and board PDFs.
"""
import os
import re
import shutil
import subprocess
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
KICAD = os.path.join(HERE, "kicad")
PCB = os.path.join(KICAD, "rs232_carrier.kicad_pcb")
SCH = os.path.join(KICAD, "rs232_carrier.kicad_sch")
FAB = os.path.join(KICAD, "fab")


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print("  cmd failed:", " ".join(cmd), "\n", r.stderr[-800:])
    return r.returncode == 0


def main():
    os.makedirs(FAB, exist_ok=True)
    board = pcbnew.LoadBoard(PCB)
    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())
    pcbnew.SaveBoard(PCB, board)
    board = pcbnew.LoadBoard(PCB)
    report = os.path.join(FAB, "drc_report.txt")
    pcbnew.WriteDRCReport(board, report, pcbnew.EDA_UNITS_MILLIMETRES, True)
    txt = open(report).read()
    kinds = {}
    for m in re.finditer(r"^\[(\w+)\]:", txt, re.M):
        kinds[m.group(1)] = kinds.get(m.group(1), 0) + 1
    found = re.findall(r"\*\* Found (\d+) (.*?) \*\*", txt)
    print("DRC:", ", ".join(f"{n} {what}" for n, what in found))
    print("DRC by type:", kinds)
    if "--no-export" in sys.argv:
        return 1 if any(int(n) for n, _ in found) else 0
    layers = "F.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts,F.Fab,B.Fab,F.CrtYd"
    gerb = os.path.join(FAB, "gerbers")
    shutil.rmtree(gerb, ignore_errors=True)
    os.makedirs(gerb)
    ok = run(["kicad-cli", "pcb", "export", "gerbers", "--layers", layers, "--no-x2", "--subtract-soldermask", "-o", gerb + "/", PCB])
    ok &= run(["kicad-cli", "pcb", "export", "drill", "--format", "excellon", "--drill-origin", "absolute", "--excellon-separate-th",
               "--generate-map", "--map-format", "gerberx2", "-o", gerb + "/", PCB])
    ok &= run(["kicad-cli", "pcb", "export", "pos", "--format", "csv", "--units", "mm", "--side", "both", "--use-drill-file-origin",
               "-o", os.path.join(FAB, "rs232_carrier-pos.csv"), PCB])
    ok &= run(["kicad-cli", "sch", "export", "pdf", "-o", os.path.join(FAB, "rs232_carrier-schematic.pdf"), SCH])
    ok &= run(["kicad-cli", "pcb", "export", "pdf", "--layers", "F.Cu,F.SilkS,Edge.Cuts,F.Fab", "-o", os.path.join(FAB, "rs232_carrier-top.pdf"), PCB])
    ok &= run(["kicad-cli", "pcb", "export", "pdf", "--layers", "B.Cu,B.SilkS,Edge.Cuts", "--mirror", "-o", os.path.join(FAB, "rs232_carrier-bottom.pdf"), PCB])
    ok &= run(["kicad-cli", "pcb", "export", "svg", "--layers", "F.Cu,B.Cu,F.SilkS,Edge.Cuts,F.Fab", "--page-size-mode", "2", "--exclude-drawing-sheet",
               "-o", os.path.join(FAB, "rs232_carrier-board.svg"), PCB])
    try:
        import cairosvg
        cairosvg.svg2png(url=os.path.join(FAB, "rs232_carrier-board.svg"), write_to=os.path.join(FAB, "rs232_carrier-board.png"),
                         output_width=1800, background_color="black")
    except Exception as exc:   # the PNG is only a preview
        print("board png skipped:", exc)
    zipf = os.path.join(FAB, "rs232_carrier-gerbers-pcbway")
    shutil.make_archive(zipf, "zip", gerb)
    print("fab files:", sorted(os.listdir(FAB)))
    print("gerber zip:", zipf + ".zip", "exports", "ok" if ok else "with errors")
    return 0 if ok and not any(int(n) for n, _ in found) else 1


if __name__ == "__main__":
    sys.exit(main())
