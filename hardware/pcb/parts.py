"""Component + net definitions shared by the schematic and PCB generators.
Single source of truth: both gen_schematic.py and gen_pcb.py import `components` and `nets`.

=== ARCHITECTURE v2 (I2C daughterboard) ===
Pivoted from a single custom ESP32-S3 board to a small DAUGHTERBOARD that plugs
into an off-the-shelf LCDwiki "2.8inch ESP32-S3 Display" base board
(https://www.lcdwiki.com/2.8inch_ESP32-S3_Display, SKU ES3C28P/ES3N28P).
That base board already supplies: ESP32-S3-WROOM-1-N16R8 (exact match to the
existing v1.5.0 firmware's target -- no flash/PSRAM downgrade), the 2.8" ILI9341
LCD + FT6336G capacitive touch, USB-C + TP4054 LiPo charging. None of that is on
this board anymore -- this file now models ONLY the daughterboard: 4x RJ45 + the
RS232-port electronics, connected to the base board over I2C instead of the
previous SPI/native-UART mix, because the base board only exposes 4 free GPIOs
(IO2, IO3, IO14, IO21, on two identical 1.25mm-pitch 4-pin headers P3/P4) -- far
too few for 4x SPI or native UART. I2C needs only 2 shared wires (SDA/SCL)
regardless of how many devices hang off it, so this fits with 2 pins to spare.

=== v2.1 correction: half the UART-bridge and level-shifter chip count ===
The first v2 cut (see git history / earlier delivered zip) used 4x SC16IS750
(single-channel I2C UART bridge) + 4x MAX3232 (using only ONE of its two
transceiver channels each) -- one full chip pair per RJ45 port. That was a real
inefficiency, caught before ordering anything: BOTH chips are natively
dual-capable and neither datasheet needs a second physical chip to get there --
- MAX3232 (SOIC-16) already has TWO independent driver/receiver channels
  (T1IN/T1OUT/R1IN/R1OUT AND T2IN/T2OUT/R2IN/R2OUT) sharing the SAME 4 external
  0.1uF charge-pump/decoupling caps -- using only channel 1 per chip, as v2
  first did, wastes the second channel for free.
- SC16IS752 (TSSOP-28) is NXP's dual-UART sibling of the SC16IS750, and its
  datasheet confirms the SAME I2C-bus/SPI interface pin (pin 9, I2C/~SPI) as
  the SC16IS750 -- it is NOT an SPI-only part. Verified directly against the
  KiCad "SC16IS752IPW" symbol (/usr/share/kicad/symbols/Interface_UART.kicad_sym,
  same authoritative source already used for SC16IS750xPW -- no hand-transcribed
  PDF pinout risk), which carries the identical VDD/VSS/I2C-~SPI/A0/A1/SCL/SDA/
  RESET/XTAL1/XTAL2/~IRQ pin pattern as the single-channel part, just with a
  second UART channel (TXB/RXB/~RTSB/~CTSB/GPIO0-3) added.
So ONE SC16IS752 (I2C mode, one address) + ONE MAX3232 (both channels used) now
serves TWO RJ45 ports. Net effect for 4 ports: 2x SC16IS752 + 2x MAX3232
instead of 4x SC16IS750 + 4x MAX3232 -- half the chip count in both expensive
families, and (per current LCSC pricing, see the delivery doc's BOM) roughly a
factor of 5 cheaper on the UART-bridge line alone, since SC16IS752 is not simply
"2x the price" of SC16IS750 -- it is cheaper per unit despite doing more.
Physical RJ45 positions on the board are UNCHANGED (still 4 jacks, same 24mm
pitch) -- only the support electronics underneath them are now shared in pairs,
so the enclosure's RJ45 cutout positions did not need to change either.

Known conflict in vendor docs, resolved defensively: the product page calls
IO2/3/14/21 "four idle IO ports", but a separate vendor IO-allocation table
(supplied by Niko) lists IO2 as "LCD backlight control pin" -- a concrete,
specific conflict, unlike the same table's blanket (and almost certainly
boilerplate/copy-paste) "RGB data pin" label on nearly every other pin. Since
I2C only needs 2 of the 4 available pins, IO2 is simply avoided: SDA/SCL are
assigned to IO3 + IO14, IO21 is used as a spare interrupt line, and the pin
carrying "IO2" on the mating cable is left unconnected on this board.

Exact physical pin ORDER on the base board's P3/P4 header (which of the 4
physical positions is IO3 vs IO14 vs IO21) could NOT be confirmed from the
vendor's schematic/manual PDFs (graphical diagrams, not extractable as text).
This is deliberately a NON-ISSUE for this board's own design: J1 below is a
generic 4-pin connector under OUR control (pin1=SDA, pin2=SCL, pin3=IRQ,
pin4=NC), and the mating cable is a loose pigtail wired by hand once the real
board is in hand -- matching by continuity test, not by a fixed pre-made
cable. Worst case of a wrong guess is "I2C doesn't respond", not damage: the
connector carries only 3.3V-logic GPIO signals, no power rail to short.

No 3V3/GND is available on P3/P4, so this board is power-independent: it taps
the RAW LiPo voltage from the shared battery (J2, wired in parallel/Y-spliced
with the base board's own JP1 battery input) and regulates its own local 3.3V
via U10 (AMS1117-3.3). This also supplies the common ground reference I2C
needs, since GND is shared through the battery connection either way.
"""

# --- Symbol source (library_file, symbol_name, base-symbol-if-extends, footprint) ---
SYM = {
    'MAX3232':  ('/usr/share/kicad/symbols/Interface_UART.kicad_sym', 'MAX3232', 'MAX232',
                 'Package_SO:SOIC-16_3.9x9.9mm_P1.27mm'),
    'SC16IS752':('/usr/share/kicad/symbols/Interface_UART.kicad_sym', 'SC16IS752IPW', None,
                 'Package_SO:TSSOP-28_4.4x9.7mm_P0.65mm'),
    'AMS1117':  ('/usr/share/kicad/symbols/Regulator_Linear.kicad_sym', 'AMS1117-3.3', 'AP1117-15',
                 'Package_TO_SOT_SMD:SOT-223-3_TabPin2'),
    'OSC':      ('/usr/share/kicad/symbols/Oscillator.kicad_sym', 'ASE-xxxMHz', None,
                 'Oscillator:Oscillator_SMD_Abracon_ASE-4Pin_3.2x2.5mm'),
    'RJ45':     ('/usr/share/kicad/symbols/Connector.kicad_sym', 'RJ45_Shielded', '8P8C_Shielded',
                 'Connector_RJ:RJ45_Amphenol_RJHSE5380'),
    'CONN2':    ('/usr/share/kicad/symbols/Connector_Generic.kicad_sym', 'Conn_01x02', None,
                 'Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical'),
    'CONN4JST': ('/usr/share/kicad/symbols/Connector_Generic.kicad_sym', 'Conn_01x04', None,
                 'Connector_JST:JST_SH_BM04B-SRSS-TB_1x04-1MP_P1.00mm_Vertical'),
    'R':        ('/usr/share/kicad/symbols/Device.kicad_sym', 'R', None,
                 'Resistor_SMD:R_0805_2012Metric'),
    'C':        ('/usr/share/kicad/symbols/Device.kicad_sym', 'C', None,
                 'Capacitor_SMD:C_0805_2012Metric'),
}

# --- Components: ref -> (SYM key, value string) --------------------------
components = {}
def C_(ref, key, value):
    components[ref] = (key, value)

C_('U2', 'MAX3232', 'MAX3232ESE+T')        # both channels used: Port 1 (ch.A) + Port 2 (ch.B)
C_('U3', 'MAX3232', 'MAX3232ESE+T')        # both channels used: Port 3 (ch.A) + Port 4 (ch.B)
C_('U6', 'SC16IS752', 'SC16IS752IPW,128')  # I2C UART bridge, Ports 1+2, addr A1=0 A0=0
C_('U7', 'SC16IS752', 'SC16IS752IPW,128')  # I2C UART bridge, Ports 3+4, addr A1=0 A0=1
C_('U10','AMS1117', 'AMS1117-3.3')         # local 3.3V LDO, fed from shared-battery raw voltage
C_('X1', 'OSC', '1.8432MHz active osc (3.3V CMOS)')  # shared clock for both SC16IS752
C_('J1', 'CONN4JST', 'TO_BASEBOARD_I2C')   # -> LCDwiki ESP32-S3 board P3/P4 (SDA,SCL,IRQ,NC)
C_('J2', 'CONN2', 'BATT_JST-PH2 (shared w/ base board, Y-tap)')
C_('J10', 'RJ45', 'PORT1')
C_('J11', 'RJ45', 'PORT2')
C_('J12', 'RJ45', 'PORT3')
C_('J13', 'RJ45', 'PORT4')

_r = 0
def R_(value):
    global _r; _r += 1; ref = f'R{_r}'; C_(ref, 'R', value); return ref
_c = 0
def Cp_(value):
    global _c; _c += 1; ref = f'C{_c}'; C_(ref, 'C', value); return ref

# --- Net list: net_name -> [(ref, pin_number), ...] -----------------------
nets = {}
def N(name, *pins):
    nets.setdefault(name, []).extend(pins)

# --- local power: raw battery -> AMS1117 -> regulated +3V3 ---
N('BATT_RAW', ('J2', '1'), ('U10', '3'))     # U10 pin3 = VI
N('GND', ('J2', '2'), ('U10', '1'))          # U10 pin1 = GND
N('+3V3', ('U10', '2'))                      # U10 pin2 = VO
c_batt = Cp_('1uF'); c_in = Cp_('10uF'); c_out = Cp_('10uF')
N('BATT_RAW', (c_batt, '1'), (c_in, '1')); N('GND', (c_batt, '2'), (c_in, '2'))
N('+3V3', (c_out, '1')); N('GND', (c_out, '2'))

# --- shared 1.8432MHz active oscillator (drives XTAL1 on both SC16IS752; XTAL2 left NC each) ---
N('+3V3', ('X1', '4'), ('X1', '1'))   # pin4=Vdd, pin1=EN tied high (always enabled)
N('GND', ('X1', '2'))                 # pin2=GND
N('OSC_CLK', ('X1', '3'))             # pin3=OUT
c_osc = Cp_('100nF')
N('+3V3', (c_osc, '1')); N('GND', (c_osc, '2'))

# --- shared I2C bus (to base board via J1) + shared IRQ (open-drain wired-OR) + shared RESET ---
N('I2C_SDA', ('J1', '1')); N('I2C_SCL', ('J1', '2')); N('IRQ_SHARED', ('J1', '3'))
# J1 pin 4: intentionally NC -- would-be "IO2", avoided (see module docstring: vendor docs
# disagree on whether it's free; costs nothing to leave unused since I2C only needs 2 wires)
r_sda = R_('4.7k'); r_scl = R_('4.7k'); r_irq = R_('10k'); r_rst = R_('10k')
N('+3V3', (r_sda, '1'), (r_scl, '1'), (r_irq, '1'), (r_rst, '1'))
N('I2C_SDA', (r_sda, '2')); N('I2C_SCL', (r_scl, '2'))
N('IRQ_SHARED', (r_irq, '2'))
N('SC_RESET_SHARED', (r_rst, '2'))
c_rst = Cp_('100nF')
N('SC_RESET_SHARED', (c_rst, '1')); N('GND', (c_rst, '2'))

# --- MAX3232 dual-port helper: BOTH transceiver channels used, one RJ45 each -------------
# Pinout (SOIC-16, real symbol pins -- MAX3232 extends the base MAX232 symbol, same
# library already verified against KiCad's own Interface_UART.kicad_sym):
#  1 C1+  2 VS+  3 C1-  4 C2+  5 C2-  6 VS-  7 T2OUT  8 R2IN  9 R2OUT  10 T2IN
#  11 T1IN  12 R1OUT  13 R1IN  14 T1OUT  15 GND  16 VCC
# Channel A (port rj_a): T1IN(11)<-tx_a  R1OUT(12)->rx_a  T1OUT(14)->RJ45 TXD  R1IN(13)<-RJ45 RXD
# Channel B (port rj_b): T2IN(10)<-tx_b  R2OUT(9)->rx_b   T2OUT(7)->RJ45 TXD   R2IN(8)<-RJ45 RXD
# All 4 charge-pump/decoupling caps are shared per CHIP (not per channel) -- standard
# MAX3232 application circuit needs exactly 4 external 0.1uF caps regardless of whether
# one or both channels are used, so using channel B "for free" costs zero extra caps.
def max3232_dual(u_ref, rj_a, rj_b, tx_a, rx_a, tx_b, rx_b):
    c1 = Cp_('100nF'); c2 = Cp_('100nF'); c3 = Cp_('100nF'); cd = Cp_('100nF')
    N('+3V3', (u_ref,'16'), (cd,'1')); N('GND', (u_ref,'15'), (cd,'2'))
    N(f'{u_ref}_CPP', (u_ref,'1'), (c1,'1')); N(f'{u_ref}_CPN', (u_ref,'3'), (c1,'2'))
    N(f'{u_ref}_CPP2', (u_ref,'4'), (c2,'1')); N(f'{u_ref}_CPN2', (u_ref,'5'), (c2,'2'))
    N(f'{u_ref}_VSP', (u_ref,'2'), (c3,'1')); N(f'{u_ref}_VSN', (u_ref,'6'), (c3,'2'))
    # -- channel A -> rj_a --
    N(tx_a, (u_ref,'11'))    # T1IN  (TTL in, from SC16IS752 TXA)
    N(rx_a, (u_ref,'12'))    # R1OUT (TTL out, to SC16IS752 RXA)
    rser_tx_a = R_('150R'); rser_rx_a = R_('150R')
    N(f'{u_ref}_TX232A', (u_ref,'14'), (rser_tx_a,'1'))
    N(f'{rj_a}_TXD', (rser_tx_a,'2'), (rj_a,'3'))
    N(f'{u_ref}_RX232A', (u_ref,'13'), (rser_rx_a,'1'))
    N(f'{rj_a}_RXD', (rser_rx_a,'2'), (rj_a,'6'))
    N('GND', (rj_a,'4'), (rj_a,'5'), (rj_a,'SH'))
    # -- channel B -> rj_b --
    N(tx_b, (u_ref,'10'))    # T2IN  (TTL in, from SC16IS752 TXB)
    N(rx_b, (u_ref,'9'))     # R2OUT (TTL out, to SC16IS752 RXB)
    rser_tx_b = R_('150R'); rser_rx_b = R_('150R')
    N(f'{u_ref}_TX232B', (u_ref,'7'), (rser_tx_b,'1'))
    N(f'{rj_b}_TXD', (rser_tx_b,'2'), (rj_b,'3'))
    N(f'{u_ref}_RX232B', (u_ref,'8'), (rser_rx_b,'1'))
    N(f'{rj_b}_RXD', (rser_rx_b,'2'), (rj_b,'6'))
    N('GND', (rj_b,'4'), (rj_b,'5'), (rj_b,'SH'))

# --- SC16IS752 helper: one chip per PAIR of RS232 ports, shared I2C/OSC/RESET/IRQ busses -
# Pinout (TSSOP-28, real symbol pins, verified against the KiCad library's SC16IS752IPW
# symbol -- same authoritative source as SC16IS750xPW, so no hand-transcribed-PDF risk):
#  1 ~RTSA(NC)  2 ~CTSA(tie GND)  3 TXA  4 RXA  5 ~RESET  6 XTAL1  7 XTAL2(NC)  8 VDD
#  9 I2C/~SPI (tie HIGH = I2C mode -- TBD verify polarity against NXP datasheet, same
#    open item as v2's SC16IS750)   10 A0/~CS (I2C addr bit A0)   11 A1/SI (I2C addr bit A1)
#  12 n.c./SO (unused in I2C mode)   13 SCL/SCLK   14 SDA/VSS (=SDA in I2C mode)
#  15 ~IRQ (open-drain)   16 ~CTSB(tie GND)   17 ~RTSB(NC)   18-21 GPIOx/modem-B (NC)
#  22 VSS   23 TXB   24 RXB   25-28 GPIOx/modem-A (NC)
def sc16is752_pair(u_ref, mx_ref, rj_a, rj_b, addr_a0, addr_a1):
    N('+3V3', (u_ref,'8'), (u_ref,'9'))              # VDD, I2C/~SPI=HIGH selects I2C mode
    N('GND', (u_ref,'22'), (u_ref,'2'), (u_ref,'16'))  # VSS, ~CTSA + ~CTSB tied low (no flow ctrl)
    N(addr_a0, (u_ref,'10'))              # A0/~CS
    N(addr_a1, (u_ref,'11'))              # A1/SI
    N('OSC_CLK', (u_ref,'6'))             # XTAL1 driven by shared external oscillator
    N('I2C_SCL', (u_ref,'13')); N('I2C_SDA', (u_ref,'14'))
    N('SC_RESET_SHARED', (u_ref,'5'))
    N('IRQ_SHARED', (u_ref,'15'))
    cd = Cp_('100nF')
    N('+3V3', (cd,'1')); N('GND', (cd,'2'))
    max3232_dual(mx_ref, rj_a, rj_b,
                 f'{u_ref}_TXA', f'{u_ref}_RXA', f'{u_ref}_TXB', f'{u_ref}_RXB')
    N(f'{u_ref}_TXA', (u_ref,'3'))    # TXA  -> MAX3232 T1IN
    N(f'{u_ref}_RXA', (u_ref,'4'))    # RXA  <- MAX3232 R1OUT
    N(f'{u_ref}_TXB', (u_ref,'23'))   # TXB  -> MAX3232 T2IN
    N(f'{u_ref}_RXB', (u_ref,'24'))   # RXB  <- MAX3232 R2OUT

sc16is752_pair('U6', 'U2', 'J10', 'J11', 'GND',  'GND')    # Ports 1+2: A0=0, A1=0
sc16is752_pair('U7', 'U3', 'J12', 'J13', '+3V3', 'GND')    # Ports 3+4: A0=1, A1=0

print(f"components={len(components)} nets={len(nets)}")
if __name__ == '__main__':
    # sanity: no ref/pin used twice on different nets, no empty nets
    seen = {}
    for name, pins in nets.items():
        for p in pins:
            if p in seen and seen[p] != name:
                print(f"COLLISION: pin {p} on both '{seen[p]}' and '{name}'")
            seen[p] = name
    for ref in components:
        used = [p for n,pins in nets.items() for (r,p) in pins if r == ref]
        if not used:
            print(f"WARNING: {ref} has no net connections at all")
    print("check done")
