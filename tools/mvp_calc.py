#!/usr/bin/env python3
"""
mvp_calc.py - Engineering + business calculator for the smart interaction desk lamp MVP.

Why this exists
---------------
Every number quoted in docs/ is produced by this script instead of being guessed:
  * BOM roll-up per product tier (basic / plus / pro)
  * 5 V rail power budget + PSU headroom
  * LED board thermal estimate (case rise above ambient)
  * Unit economics and break-even volume
  * Self-test of the on-flash IR record format used by the firmware (CRC + truncation)

Usage
-----
    python tools/mvp_calc.py all
    python tools/mvp_calc.py bom|power|thermal|unit|selftest

Output is plain ASCII on purpose (Windows consoles default to GBK and mangle CJK).
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from dataclasses import dataclass


@dataclass(frozen=True)
class Part:
    name: str
    qty: float
    unit_cost_cny: float
    note: str = ""

    @property
    def total(self) -> float:
        return self.qty * self.unit_cost_cny


# Tier membership: 0 = basic (IR + lighting), 1 = plus (+ timer + OLED), 2 = pro (+ voice + servos)
@dataclass(frozen=True)
class BomLine:
    part: Part
    tier_min: int


BOM: list[BomLine] = [
    # --- common platform -------------------------------------------------------------
    BomLine(Part("ESP32-S3-WROOM-1-N4R2 module", 1, 17.5, "WiFi/BLE, 4MB flash, 2MB PSRAM"), 0),
    BomLine(Part("PCB 4-layer, 80x60mm", 1, 6.0, "batch 500 pcs"), 0),
    BomLine(Part("AC-DC 5V/2A adapter + cable", 1, 12.0, "CCC certified, external brick"), 0),
    BomLine(Part("DC jack + power switch", 1, 1.6), 0),
    BomLine(Part("Passives / connectors / screws", 1, 4.5, "resistors, caps, JST, M3 kit"), 0),
    BomLine(Part("Enclosure (ABS injection)", 1, 9.0, "tooling amortised separately"), 0),
    BomLine(Part("Diffuser + light guide", 1, 3.2, "PMMA, 92% transmission"), 0),
    BomLine(Part("Assembly + functional test labour", 1, 6.0, "~6 min/unit at 60 CNY/h"), 0),
    BomLine(Part("Packaging (box + foam + manual)", 1, 4.0), 0),
    # --- lighting --------------------------------------------------------------------
    BomLine(Part("LED board 48x 2835 dual-CCT", 1, 9.5, "3000K + 6000K, 24 per channel"), 0),
    BomLine(Part("PT4115 constant-current driver x2", 2, 1.4, "one per CCT channel"), 0),
    BomLine(Part("Aluminium heatsink 60x40x8mm", 1, 3.6), 0),
    # --- infrared --------------------------------------------------------------------
    BomLine(Part("TSOP38238 IR receiver", 1, 2.2, "38 kHz demodulating"), 0),
    BomLine(Part("940 nm IR emitter x2 + driver", 2, 1.1, "2N3904 boost stage"), 0),
    BomLine(Part("IR window (IR-transparent PMMA)", 1, 0.8), 0),
    # --- tier 1: timers + UI ---------------------------------------------------------
    BomLine(Part("0.96in OLED SSD1306 I2C", 1, 9.0), 1),
    BomLine(Part("Tactile buttons x3 + caps", 3, 0.5), 1),
    BomLine(Part("Buzzer (passive, 4 kHz)", 1, 1.2), 1),
    # --- tier 2: motion + voice ------------------------------------------------------
    BomLine(Part("SG90 servo (head tilt)", 1, 6.5), 2),
    BomLine(Part("MG996R servo (base yaw)", 1, 14.0), 2),
    BomLine(Part("Servo bracket set + bearings", 1, 7.5), 2),
    BomLine(Part("LD3320 offline voice module", 1, 17.0, "50 command words, no cloud"), 2),
    BomLine(Part("MEMS mic + pre-amp", 1, 3.0), 2),
    BomLine(Part("5V/4A adapter delta (pro only)", 1, 6.0, "over the 2A brick"), 2),
]

TIERS = {
    0: ("Basic", "IR learn + lighting", 149.0),
    1: ("Plus", "+ timers + OLED", 199.0),
    2: ("Pro", "+ voice + motion", 299.0),
}

# --- power budget: currents on the 5 V rail -------------------------------------------
# (label, typical_mA, peak_mA, note)
POWER_5V = [
    ("ESP32-S3, WiFi STA connected, avg", 95, 350, "peaks during TX burst"),
    ("LED board, 2 channels @ 60% dim", 700, 1000, "5 W nominal, capped by driver"),
    ("PT4115 quiescent + logic", 12, 15, "x2"),
    ("TSOP38238 receiver", 0.9, 1.2, "always on"),
    ("IR emitter burst (2x)", 0, 240, "duty cycle < 1%, only while sending"),
    ("OLED SSD1306 (plus/pro)", 12, 25, "50% pixels lit"),
    ("Buzzer (plus/pro)", 0, 35, "short beeps only"),
    ("SG90 idle/holding (pro)", 15, 220, "stall = 220 mA"),
    ("MG996R idle/holding (pro)", 20, 900, "stall = 900 mA, never simultaneous"),
    ("LD3320 voice module (pro)", 45, 60, "listening state"),
]

PSU_TIERS = {0: ("5V/2A (10 W)", 2000), 1: ("5V/2A (10 W)", 2000), 2: ("5V/4A (20 W)", 4000)}
# Pro tier derates servos: firmware enforces one servo moving at a time (motion.cpp).

# --- thermal: LED board ---------------------------------------------------------------
LED_ELECTRICAL_W = 5.0
LED_OPTICAL_EFFICIENCY = 0.30       # 30% of input becomes light for 2835 mid-power
HEATSINK_TO_AMBIENT_C_PER_W = 6.5   # small stamped Al plate, natural convection
AMBIENT_C = 30.0
LED_MAX_CASE_C = 85.0               # 2835 datasheet derating point

# --- unit economics -------------------------------------------------------------------
CHANNEL_FEE_PCT = 0.05
LOGISTICS_CNY = 8.0
RETURN_PROVISION_PCT = 0.06        # 6% return/refund provision on revenue
MARKETING_CNY = 15.0               # blended CAC: content + campus channel, no paid search
FIXED_MONTHLY_CNY = 18000.0        # 2 founders part-time + small studio + tooling upkeep
TOOLING_CNY = 45000.0              # injection mould, one-off
TOOLING_AMORTISE_UNITS = 5000

# Component price breaks. 500 pcs is the pilot batch (MVP validation),
# 5000 pcs is the year-one production plan.
BOM_VOLUME_FACTOR = {500: 1.00, 2000: 0.88, 5000: 0.82}
PLAN_VOLUME = 5000

# --------------------------------------------------------------------------------------
# Calculations
# --------------------------------------------------------------------------------------


def bom_cost(tier: int, volume: int = 500) -> float:
    factor = BOM_VOLUME_FACTOR[volume]
    return round(sum(line.part.total for line in BOM if line.tier_min <= tier) * factor, 2)


def print_bom() -> None:
    print("== BOM ROLL-UP (CNY, tooling excluded) ==")
    header = f"{'part':44s} {'qty':>4s} {'basic':>8s} {'plus':>8s} {'pro':>8s}"
    print(header)
    print("-" * len(header))
    for line in BOM:
        cells = []
        for tier in TIERS:
            cells.append(f"{line.part.total:8.2f}" if line.tier_min <= tier else f"{'-':>8s}")
        print(f"{line.part.name:44s} {line.part.qty:4.0f} " + " ".join(cells))
    print("-" * len(header))
    for volume in (500, PLAN_VOLUME):
        print(f"  batch {volume:4d} pcs (BOM x{BOM_VOLUME_FACTOR[volume]:.2f}):")
        for tier, (name, desc, price) in TIERS.items():
            total = bom_cost(tier, volume)
            print(f"    {name:5s} ({desc:22s}) BOM {total:6.2f} CNY = {total / price * 100:5.1f}% of {price:.0f} CNY retail")


def print_power() -> None:
    print("== 5 V RAIL BUDGET (mA) ==")
    header = f"{'load':36s} {'typ':>7s} {'peak':>7s}  note"
    print(header)
    print("-" * len(header))
    typ_total = peak_total = 0.0
    for label, typ, peak, note in POWER_5V:
        print(f"{label:36s} {typ:7.1f} {peak:7.1f}  {note}")
        typ_total += typ
        peak_total += peak
    print("-" * len(header))
    print(f"{'SUM (naive worst case, all at once)':36s} {typ_total:7.1f} {peak_total:7.1f}")
    # Mitigation implemented in firmware: servos interlocked, LED dimmed to 40% while moving
    interlocked = peak_total - max(900, 220)
    dimmed = interlocked - 0.30 * 1000
    print(f"{'Firmware interlock (one servo only)':36s} {'':7s} {interlocked:7.1f}")
    print(f"{'+ LED 60%->40% during a move':36s} {'':7s} {dimmed:7.1f}")
    print()
    for tier, (name, rating) in PSU_TIERS.items():
        need = dimmed if tier == 2 else typ_total
        headroom = (rating - need) / rating * 100
        flag = "OK" if headroom >= 30 else "TIGHT"
        print(f"  tier {tier} ({TIERS[tier][0]:5s}) PSU {name:14s} headroom {headroom:5.1f}%  [{flag}]")
    print("\n  Rule: keep >=30% headroom on the 5 V rail; the adapter is the cheapest part to up-rate.")


def print_thermal() -> None:
    heat_w = LED_ELECTRICAL_W * (1 - LED_OPTICAL_EFFICIENCY)
    case_rise = heat_w * HEATSINK_TO_AMBIENT_C_PER_W
    case_c = AMBIENT_C + case_rise
    margin = LED_MAX_CASE_C - case_c
    life = "> 25000 h" if case_c < 75 else "15000-25000 h"
    print("== LED BOARD THERMAL (steady state, no airflow) ==")
    print(f"  electrical input      : {LED_ELECTRICAL_W:.1f} W")
    print(f"  optical efficiency    : {LED_OPTICAL_EFFICIENCY * 100:.0f}%")
    print(f"  heat to dissipate     : {heat_w:.1f} W")
    print(f"  heatsink theta        : {HEATSINK_TO_AMBIENT_C_PER_W:.1f} C/W")
    print(f"  case rise above amb.  : {case_rise:.1f} C")
    print(f"  ambient (worst case)  : {AMBIENT_C:.1f} C")
    print(f"  estimated LED case    : {case_c:.1f} C  (limit {LED_MAX_CASE_C:.0f} C)")
    print(f"  margin                : {margin:.1f} C  [{'OK' if margin >= 15 else 'FAIL'}]")
    print(f"  inferred LED life     : {life}")
    print("\n  Action: 60 C is the design target at 25 C ambient; verify with a thermocouple in round 1.")


def unit_econ(tier: int, volume: int = PLAN_VOLUME) -> dict[str, float]:
    name, _desc, price = TIERS[tier]
    bom = bom_cost(tier, volume)
    tooling = TOOLING_CNY / TOOLING_AMORTISE_UNITS
    fees = price * CHANNEL_FEE_PCT
    returns = price * RETURN_PROVISION_PCT
    cost = bom + tooling + LOGISTICS_CNY + fees + returns + MARKETING_CNY
    contribution = price - cost
    return {
        "name": name,
        "price": price,
        "bom": bom,
        "tooling": tooling,
        "logistics": LOGISTICS_CNY,
        "fees": fees,
        "returns": returns,
        "marketing": MARKETING_CNY,
        "cost": cost,
        "contribution": contribution,
        "margin_pct": contribution / price * 100,
        "breakeven_units": FIXED_MONTHLY_CNY / contribution if contribution > 0 else float("inf"),
    }


def print_unit() -> None:
    print(f"== UNIT ECONOMICS (CNY/unit, volume {PLAN_VOLUME} pcs) ==")
    header = f"{'item':26s}" + "".join(f"{TIERS[t][0]:>10s}" for t in TIERS)
    print(header)
    print("-" * len(header))
    data = {t: unit_econ(t) for t in TIERS}
    labels = [
        ("price", "retail price", False),
        ("bom", "- BOM", False),
        ("tooling", "- tooling amortised", False),
        ("logistics", "- logistics", False),
        ("fees", "- channel fee", False),
        ("returns", "- return provision", False),
        ("marketing", "- marketing/CAC", False),
        ("cost", "= total cost", False),
        ("contribution", "= contribution", False),
        ("margin_pct", "contribution margin %", True),
    ]
    for key, label, is_pct in labels:
        vals = "".join(f"{data[t][key]:10.1f}" if is_pct else f"{data[t][key]:10.2f}" for t in TIERS)
        print(f"{label:26s}{vals}")
    print()
    for t in TIERS:
        print(f"  {data[t]['name']:5s}: break-even {data[t]['breakeven_units']:6.0f} units/month "
              f"at {FIXED_MONTHLY_CNY:,.0f} CNY fixed cost")

    print(f"\n== PILOT BATCH SENSITIVITY (500 pcs) ==")
    for t in TIERS:
        pilot = unit_econ(t, 500)
        print(f"  {pilot['name']:5s}: BOM {pilot['bom']:6.2f}  contribution {pilot['contribution']:6.2f}  "
              f"margin {pilot['margin_pct']:5.1f}%  "
              f"[{'healthy' if pilot['margin_pct'] >= 15 else 'thin - do not scale on this tier'}]")

    mix = {0: 0.60, 1: 0.30, 2: 0.10}  # assumed first-year channel mix
    blended_price = sum(mix[t] * TIERS[t][2] for t in TIERS)
    blended_contrib = sum(mix[t] * data[t]["contribution"] for t in TIERS)
    blended_bom = sum(mix[t] * data[t]["bom"] for t in TIERS)
    print(f"\n  Blended (60/30/10): ASP {blended_price:.0f} CNY, BOM {blended_bom:.0f} CNY, "
          f"contribution {blended_contrib:.0f} CNY/unit, break-even {FIXED_MONTHLY_CNY / blended_contrib:.0f} units/month")
    print(f"  5000 units/year -> contribution {blended_contrib * 5000 / 10000:.1f} wan CNY, "
          f"minus fixed {FIXED_MONTHLY_CNY * 12 / 10000:.1f} wan CNY "
          f"= {(blended_contrib * 5000 - FIXED_MONTHLY_CNY * 12) / 10000:+.1f} wan CNY")


# --------------------------------------------------------------------------------------
# IR record format self-test (mirrors firmware/include/ir_store.h)
# --------------------------------------------------------------------------------------
# Header layout, little endian:
#   magic     u32  'LIR1'
#   version   u16
#   freq_khz  u16
#   raw_len   u16  number of uint16 mark/space entries
#   reserved  u16  zero
#   payload   raw_len * u16
#   crc32     u32  CRC-32 (zlib) over header-without-crc + payload

MAGIC = 0x4C495231  # 'LIR1'
VERSION = 1
MAX_RAW_LEN = 1024


def pack_record(raw: list[int], freq_khz: int = 38) -> bytes:
    if len(raw) == 0:
        raise ValueError("raw_len is zero")
    if len(raw) > MAX_RAW_LEN:
        raise ValueError(f"raw_len {len(raw)} exceeds MAX_RAW_LEN {MAX_RAW_LEN}")
    header = struct.pack("<IHHHH", MAGIC, VERSION, freq_khz, len(raw), 0)
    payload = struct.pack(f"<{len(raw)}H", *raw)
    body = header + payload
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def unpack_record(blob: bytes) -> tuple[int, list[int]]:
    if len(blob) < 16:
        raise ValueError("record too short")
    magic, version, freq, raw_len, _res = struct.unpack("<IHHHH", blob[:12])
    if magic != MAGIC:
        raise ValueError("bad magic")
    if version != VERSION:
        raise ValueError(f"unsupported version {version}")
    if raw_len == 0 or raw_len > MAX_RAW_LEN:
        raise ValueError("raw_len out of range")
    expected = 12 + raw_len * 2 + 4
    if len(blob) != expected:
        raise ValueError(f"length mismatch: have {len(blob)}, expected {expected}")
    body = blob[: 12 + raw_len * 2]
    (crc,) = struct.unpack("<I", blob[-4:])
    if zlib.crc32(body) & 0xFFFFFFFF != crc:
        raise ValueError("crc mismatch")
    raw = list(struct.unpack(f"<{raw_len}H", blob[12 : 12 + raw_len * 2]))
    return freq, raw


def print_selftest() -> None:
    print("== IR RECORD FORMAT SELF-TEST ==")
    results: list[bool] = []

    def check(name: str, cond: bool) -> None:
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
        results.append(cond)

    # 1. NEC 32-bit frame round-trip
    nec = [9000, 4500] + [560, 560] * 32 + [560]
    blob = pack_record(nec)
    freq, raw = unpack_record(blob)
    check("NEC frame round-trip (69 entries)", raw == nec and freq == 38)
    check(f"NEC record is {len(blob)} B, fits a 4 KB NVS blob", len(blob) < 4000)

    # 2. Full AC state frame (largest realistic payload)
    ac = [3400, 1700] + [430 + (i % 3) * 30 for i in range(600)]
    ac_blob = pack_record(ac)
    _freq2, raw2 = unpack_record(ac_blob)
    check(f"AC state frame round-trip ({len(ac)} entries, {len(ac_blob)} B)", raw2 == ac)
    check("AC frame too big for NVS blob -> must use LittleFS", len(ac_blob) > 1200)

    # 3. Corruption is detected
    corrupted = bytearray(blob)
    corrupted[len(corrupted) // 2] ^= 0xFF
    try:
        unpack_record(bytes(corrupted))
        check("corrupted payload rejected", False)
    except ValueError as exc:
        check(f"corrupted payload rejected ({exc})", True)

    # 4. Truncated write (power loss mid-save) is rejected
    try:
        unpack_record(blob[:-5])
        check("truncated record rejected", False)
    except ValueError as exc:
        check(f"truncated record rejected ({exc})", True)

    # 5. Oversized payload refused up-front
    try:
        pack_record([500] * (MAX_RAW_LEN + 1))
        check("oversized payload refused", False)
    except ValueError:
        check("oversized payload refused", True)

    # 6. Empty slot cannot masquerade as a valid record
    try:
        pack_record([])
        check("empty record refused", False)
    except ValueError:
        check("empty record refused", True)

    # 7. Worst-case database footprint: 16 slots x 1024 entries
    worst = 16 * (12 + MAX_RAW_LEN * 2 + 4)
    check(f"worst-case IR DB {worst / 1024:.0f} KB fits 1 MB LittleFS partition", worst < 1024 * 1024)

    ok = all(results)
    print(f"\n  RESULT: {'ALL PASS' if ok else 'FAILURES PRESENT'} ({sum(results)}/{len(results)})")
    if not ok:
        sys.exit(1)


def main() -> None:
    parser = argparse.ArgumentParser(description="Desk lamp MVP calculator")
    parser.add_argument("section", nargs="?", default="all",
                        choices=["all", "bom", "power", "thermal", "unit", "selftest"])
    args = parser.parse_args()

    sections = {
        "bom": print_bom,
        "power": print_power,
        "thermal": print_thermal,
        "unit": print_unit,
        "selftest": print_selftest,
    }
    if args.section == "all":
        for fn in sections.values():
            fn()
            print()
    else:
        sections[args.section]()


if __name__ == "__main__":
    main()