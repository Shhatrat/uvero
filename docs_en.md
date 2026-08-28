# Uvero U1 — UART Reverse Engineering

## Hardware

| Component | Description |
|---|---|
| Treadmill | **Uvero U1** (Chinese OEM, similar to Sperax/Deerrun/PitPat) |
| Adapter (phase 1) | Silicon Labs **CP210x** (ID `10c4:ea60`) → `/dev/ttyUSB0` |
| Microcontroller | **ESP32 DevKit** (38-pin) — UART reading + BLE RSC + WiFi dashboard |
| ESP32 case | [MakerWorld: ESP32 case 38-pin](https://makerworld.com/pl/models/1029542-esp32-case-38-pin-customize-or-ready-print?from=search#profileId-1016748) — 3D printed |

## UART Parameters

| Parameter | Value |
|---|---|
| Baud rate | **2400** |
| Format | 8N1 |
| Voltage | TTL 3.3V |
| ESP32 pin | GPIO16 (Serial2 RX) |

## Protocol Architecture

- **Half-duplex** bus — single TX/RX line shared between console and motor
- Console and motor take turns (but their transmissions slightly overlap — collision)
- Packets sent every ~300 ms
- Metrics (time, distance, calories, steps) are computed internally by the console — UART carries only the current motor speed

## Packet Structure

Four formats: 15B (stopped), 16B, 17B, 18B:

### 15-byte — treadmill stopped

```
80 1E 80 00 00 00 00 00 00 FE 80 7E 60 80 F8
 0  1  2  3  4  5  6  7  8  9 10 11 12 13 14
```

Byte [4]=`00` → speed=0.

### 16-byte

```
80 1E 80 00 06 00 E6 [A] [B] 00 FE 80 [C] [D] [E] F8
 0  1  2  3  4  5  6  7   8  9 10 11  12  13  14  15
```

- `[0-3]` = `80 1E 80 00` — fixed header (sync)
- `[4]` = `06` — running
- `[5-6]` = `00 E6` — fixed
- `[7-8]` — console nibble bytes (speed command)
- `[9]` = `00` — separator (start of motor response), **collision byte**
- `[10-11]` = `FE 80` — fixed separator (after collision)
- `[12-14]` — motor nibble bytes (speed confirmation)
- `[15]` = `F8` — end byte

### 17-byte

```
80 1E 80 00 06 00 E6 [A] [B] [C] [D] FE 80 [E] [F] [G] F8
 0  1  2  3  4  5  6  7   8   9  10  11  12  13  14  15  16
```

- `[7-9]` — console nibble bytes (3 bytes)
- `[10]` — **collision byte** (for speeds ≥ 3.0 km/h)
- `[11-12]` = `FE 80` — separator (after collision)
- `[13-15]` — motor nibble bytes
- `[16]` = `F8` — end byte

### 18-byte (only 3.4 and 3.8 km/h)

```
80 1E 80 00 06 00 E6 [A] [B] [C] [D] 00 FE 80 [E] [F] [G] F8
 0  1  2  3  4  5  6  7   8   9  10  11 12  13  14  15  16  17
```

- `[7-10]` — console nibble bytes (4 bytes)
- `[11-13]` = `00 FE 80` — separator (shifted by 1 byte compared to 17B)
- `[14-16]` — motor nibble bytes
- `[17]` = `F8` — end byte

The 18B format **was unknown from CP210x captures** — discovered only through direct ESP32 GPIO reading.

## Byte Encoding — Nibble Encoding

Each data byte encodes a **4-bit nibble** in bits [7,6,4,2]:

```
nibble = bit7<<3 | bit6<<2 | bit4<<1 | bit2
```

Redundant copies for error detection (half-duplex collision detection):
- bit5 = bit6
- bit3 = bit4
- bit1 = bit2
- bit0 = always 0

Byte lookup table:
```
0x00→0   0x60→4   0x80→8   0xE0→12
0x06→1   0x66→5   0x86→9   0xE6→13
0x18→2   0x78→6   0x98→10  0xF8→14
0x1E→3   0x7E→7   0x9E→11  0xFE→15
```

---

## Key Finding: CP210x vs ESP32 — Collision Byte Differences

> **Note:** This problem has not been documented anywhere for this protocol before.
> This section is the result of empirical analysis during the ESP32 integration.

### Root Cause — Physical Position on the Half-Duplex Bus

The Uvero U1 protocol uses a half-duplex bus where the console and motor share a single transmission line. Their transmissions **overlap in time** — the console sends the end of its command while the motor begins its response. In the overlap window (collision), bytes on the line are a superposition of both signals.

**CP210x** is connected as a **passive listener** — attached to the treadmill TX line via a separate wire, at a physical point away from the console–motor junction. It sees the dominant signal (the console) before the collision has time to corrupt it.

**ESP32 GPIO16** is connected **directly to the same line** as both the console and motor — right in the middle of the bus. During the collision window, ESP32 sees the combined signal of both transmitters: the electrical superposition of console and motor voltages. This combined signal is **deterministic** (for a given speed the motor always responds with the same data), but **different** from what the CP210x sees from the side.

### Collision Zones by Packet Format

#### 16B format (speeds: 1.1, 1.2, 2.2–2.4, 2.8, 2.9, 3.6)

```
Bytes:  [7]  [8]  [9] [10] [11] [12] [13] [14] [15]
         A    B   COL  FE   80   C    D    E    F8
                  ^^^
                  collision — console ends, motor begins here
```

- Byte `[9]` = `0x00` (separator) is the collision byte. For most 16B speeds, CP210x saw it as `0x00`; ESP32 sees it inconsistently (unreliable).
- Bytes `[10-15]` = `FE 80 [C][D][E] F8` are **after the collision** — CP210x and ESP32 see the same values here.
- **Solution**: use `keyStart=10`, `keyLen=6` (bytes `[10..15]`), skipping `[7-9]`.

**Exception — 3.6 km/h**: collision also affects byte `[10]` and `[13]`:

| Byte | CP210x sees | ESP32 sees | Comment |
|------|-------------|------------|---------|
| [9]  | `1E` | (unstable) | console+motor collision |
| [10] | `00` | `18` | collision — motor drives `18` instead of `00` |
| [13] | `00` | `60` | collision — motor response byte |

For 3.6, keyLen=6 from position 10: `{0x18, 0xFE, 0x80, 0x60, 0x1E, 0xF8}`.

#### 17B format (speeds: 1.3–2.7)

```
Bytes:  [7]  [8]  [9]  [10] [11] [12] [13] [14] [15] [16]
        COL   B    C    00   FE   80   D    E    F    F8
        ^^^
        only byte [7] is in the collision zone
```

- Byte `[7]` — collision. CP210x sees the console's outgoing stream value; ESP32 sees the superposition.
- Bytes `[8-16]` — after collision, **identical** between CP210x and ESP32.
- **Solution**: use `keyStart=8`, `keyLen=9` (bytes `[8..16]`).

For speeds 1.3–2.7, LUT keys are **identical** to CP210x values (collision only touches byte[7]).

#### 17B format — speeds ≥ 3.0 km/h (secondary collision)

```
Bytes:  [7]  [8]  [9]  [10] [11] [12] [13] [14] [15] [16]
        COL   B    C   COL2  FE   80   D    E    F    F8
        ^^^            ^^^^
        collision 1   collision 2 — secondary superposition here
```

At higher speeds the console sends 3 speed nibble bytes (`[7-9]`) instead of 2, causing byte `[10]` (normally `0x00` of the separator) to **also fall inside the collision window**. ESP32 sees the superposition value of console+motor there; CP210x sees `0x00`.

Full difference table for 17B speeds ≥ 3.0 km/h:

| km/h | CP210x key (bytes[8..16]) | ESP32 key (bytes[8..16]) | differing byte |
|------|---------------------------|--------------------------|----------------|
| 3.0  | `00 1E 00 FE 80 1E 66 80 F8` | `00 1E 60 FE 80 1E 66 80 F8` | [10]: `00`→`60` |
| 3.2  | `00 00 00 FE 80 00 80 1E F8` | `00 00 06 FE 80 00 80 1E F8` | [10]: `00`→`06` |
| 3.3  | `00 18 00 FE 80 18 9E 80 F8` | `00 18 7E FE 80 18 9E 80 F8` | [10]: `00`→`7E` |
| 3.5  | `80 78 00 FE 80 7E 7E 80 F8` | `80 78 60 FE 80 7E 7E 80 F8` | [10]: `00`→`60` |
| 4.0  | `06 66 00 FE 80 1E 78 80 F8` | `80 06 66 FE 80 1E 78 80 F8` | [10]: `00`→`FE` + shift |

Pattern: byte `[10]` in CP210x is always `0x00` (sees the separator), in ESP32 it has a unique value tied to the motor's speed data (collision with the start of the motor response creates a speed-identifying byte).

**Speeds 3.1, 3.7, 3.9** — byte [10] does not enter the collision window (likely different console command length or different timing); ESP32 keys identical to CP210x.

#### 18B format — speeds 3.4 and 3.8 km/h (format unknown from CP210x)

Speeds 3.4 and 3.8 produce 18-byte packets. CP210x **never captured them** — either due to different timing, or because the collision completely destroyed the `F8` byte that CP210x was using as the 17B end marker.

From the ESP32 perspective:
- `byte[17]` = `F8` → 18B packet
- Separator shifted: `[11-13]` = `00 FE 80`
- `keyStart=12`, `keyLen=6` (bytes `[12..17]`)

| km/h | ESP32 key (bytes[12..17]) |
|------|---------------------------|
| 3.4  | `FE 80 98 9E 1E F8` |
| 3.8  | `FE 80 00 7E 80 F8` |

### Why CP210x Did Not See This Difference

CP210x as a passive listener on the TX line (not directly in the signal loop):
1. Higher input impedance — does not "enter" the collision, only observes
2. May be physically behind a resistor or on a branch of the line where the collision is attenuated
3. Sees the console signal dominating (because it is directly on the console TX) with minimal motor interference

ESP32 GPIO16 in the same loop as the motor:
1. Low impedance → actively receives the superposition of both signals
2. In the collision window — both transmitters (console + motor) are literally fighting over the line
3. The collision result is deterministic for a given speed, so it becomes a unique speed fingerprint

### Practical Implications

If you are building a similar integration with another microcontroller connected directly to a half-duplex bus:

1. **Do not trust keys derived from CP210x** — bytes in the collision zone will differ
2. **Identify collision zones** — look for bytes that change value between packets (not stable)
3. **Use bytes outside the collision zone** as keys, OR accept collision bytes and measure their actual values directly from the target device
4. **New packet formats may appear** — CP210x may miss them if the collision destroys the `F8` end byte
5. **Mismatch counter** — when detecting a stable packet, add tolerance (n=3 consecutive different packets before resetting the stability counter) — collision bytes may vary between packets if transmission is not perfectly synchronized

---

## Speed Decoding — Lookup Table

Speed encoding **is not a simple mathematical function** — the only reliable approach is matching selected packet bytes against a lookup table.

### Key Ranges (ESP32 values, post-collision)

| Format | keyStart | keyLen | Bytes |
|--------|----------|--------|-------|
| 15B (stopped) | — | — | byte[4]=`0x00` → speed=0 |
| 16B | 10 | 6 | `[10..15]` = `FE 80 [C][D][E] F8` |
| 17B | 8 | 9 | `[8..16]` = `[B][C][COL2][FE][80][D][E][F] F8` |
| 18B | 12 | 6 | `[12..17]` = `FE 80 [E][F][G] F8` |

### Full Table (ESP32 bytes → km/h)

Speeds 1.3–2.7 km/h (17B): keys identical to CP210x (collision only in byte[7]).  
Speeds 3.0+ (17B): keys differ from CP210x at byte[10].  
3.4, 3.8 (18B): format unknown from CP210x.

| km/h | Format | ESP32 key (hex) |
|------|--------|-----------------|
| 1.1 | 16B | `FE 80 66 98 80 F8` |
| 1.2 | 16B | `FE 80 E6 E0 1E F8` |
| 1.3 | 17B | `9E 98 00 FE 80 7E FE 1E F8` |
| 1.4 | 17B | `F8 E0 00 FE 80 98 80 1E F8` |
| 1.5 | 17B | `66 9E 00 FE 80 06 98 80 F8` |
| 1.6 | 17B | `60 E0 00 FE 80 06 66 80 F8` |
| 1.7 | 17B | `80 86 00 FE 80 66 E6 80 F8` |
| 1.8 | 17B | `60 98 00 FE 80 06 78 80 F8` |
| 1.9 | 17B | `66 9E 00 FE 80 18 98 80 F8` |
| 2.0 | 17B | `F8 00 00 FE 80 E0 F8 1E F8` |
| 2.1 | 17B | `E0 18 00 FE 80 98 9E 1E F8` |
| 2.2 | 16B | `FE 80 00 80 1E F8` |
| 2.3 | 16B | `FE 80 E0 18 1E F8` |
| 2.4 | 16B | `FE 80 FE 98 80 F8` |
| 2.5 | 17B | `9E 86 00 FE 80 98 66 80 F8` |
| 2.6 | 17B | `E0 E6 00 FE 80 9E FE 1E F8` |
| 2.7 | 17B | `1E F8 00 FE 80 18 78 80 F8` |
| 2.8 | 16B | `FE 80 00 E6 1E F8` |
| 2.9 | 16B | `FE 80 78 E6 80 F8` |
| 3.0 | 17B | `00 1E **60** FE 80 1E 66 80 F8` ← byte[10]=`60` instead of `00` |
| 3.1 | 17B | `00 7E 86 00 FE 80 7E 1E F8` |
| 3.2 | 17B | `00 00 **06** FE 80 00 80 1E F8` ← byte[10]=`06` instead of `00` |
| 3.3 | 17B | `00 18 **7E** FE 80 18 9E 80 F8` ← byte[10]=`7E` instead of `00` |
| 3.4 | 18B | `FE 80 98 9E 1E F8` ← 18B format, unknown from CP210x |
| 3.5 | 17B | `80 78 **60** FE 80 7E 7E 80 F8` ← byte[10]=`60` instead of `00` |
| 3.6 | 16B | `**18** FE 80 **60** 1E F8` ← byte[10]=`18`, byte[13]=`60` |
| 3.7 | 17B | `80 00 00 FE 80 06 86 1E F8` |
| 3.8 | 18B | `FE 80 00 7E 80 F8` ← 18B format, unknown from CP210x |
| 3.9 | 17B | `80 18 86 00 FE 80 60 1E F8` (variant A) |
| 3.9 | 17B | `06 18 86 00 FE 80 60 1E F8` (variant B — byte[8] `80`→`06`) |
| 4.0 | 17B | `80 06 66 FE 80 1E 78 80 F8` |

Missing from LUT: 4.1+.

**3.9 has two variants** — byte[8] alternates between `0x80` and `0x06`. Both added to the LUT.

### Nibble [7] Pattern (speed range)

| n7 | Byte[7] | Range |
|---|---|---|
| 0 | `0x00` | 1.0–2.9 km/h |
| 8 | `0x80` | 2.9–3.2 km/h |
| 9 | `0x86` | 3.3–3.6 km/h |
| 10 | `0x98` | 3.7–4.0 km/h |

## Packet Parser (ESP32 — logic)

Sync: search for header `80 1E 80 00` (4 bytes). Then:

```
if  (rxLen >= 18 && rxBuf[17] == 0xF8) → pktLen = 18
elif(rxLen >= 17 && rxBuf[16] == 0xF8) → pktLen = 17
elif(rxLen >= 16 && rxBuf[15] == 0xF8) → pktLen = 16
elif rxLen < 18                         → wait for more bytes

keyStart = (pktLen==18) ? 12 : (pktLen==17) ? 8 : 10
keyLen   = pktLen - keyStart
```

Check 18B **before** 17B — otherwise an 18B packet will be misidentified as 17B (byte[17]=F8 never gets checked).

## ESP32 Integration — Wiring

```
Treadmill VIN (7.5V) ────────────────► ESP32 VIN (not 3V3!)
Treadmill GND  ──────────────────────► ESP32 GND
Treadmill TX   ──────────────────────► ESP32 GPIO16 (RX2)
```

**Use VIN, not 3V3.** The treadmill's 3V3 pin cannot source enough current for WiFi+BLE (~300–500 mA peaks). VIN → ESP32's onboard regulator handles those peaks.

**When ESP32 is connected to both treadmill VIN and USB simultaneously** — disconnect the treadmill from mains before plugging in USB. After flashing: unplug USB, reconnect treadmill.

## BLE RSC — Firmware Configuration (Working)

```
RSC Service 0x1814
  ├── 0x2A54 RSC Feature = 0x0000 (READ)
  ├── 0x2A5D Sensor Location = 7 / foot (READ)
  └── 0x2A53 RSC Measurement (NOTIFY) — 4 bytes: flags=0x00, speed_L, speed_H, cadence
DIS 0x180A
  ├── 0x2A29 Manufacturer = "Shhatrat" (READ)
  ├── 0x2A24 Model Number = "U1" (READ)
  ├── 0x2A25 Serial Number = "001" (READ)
  └── 0x2A26 FW Revision = "1.0" (READ)
Advertising: UUID 0x1814, appearance 0x0481 (Running Sensor)
```

**Critical rules (reasons for Garmin disconnects):**
- UUID **must be 16-bit everywhere**: `NimBLEUUID((uint16_t)0x1814)` — not a string
- RSC Feature=`0x0000` → no SC Control Point → stable connection
- RSC Feature=`0x0003` → SC Control Point with INDICATE → Garmin bug → drops every ~10s
- Cadence must be non-zero when speed > 0

## WiFi Dashboard

ESP32 serves HTTP on port 80. Data from `/json`, distance reset via `/reset_dist`.

Displays: speed, distance, uptime, temperature, BLE clients, UART status, free RAM, CPU MHz.

Source: [github.com/Shhatrat/uvero](https://github.com/Shhatrat/uvero)

## Project Status (2026-08-28) — COMPLETE ✓

### Working ✓
- **UART reading** — live speed decoder, all speeds 1.1–4.0 km/h
- **Lookup table** — 31 entries (30 speeds + 3.9 variant B), zero conflicts
- **BLE RSC** — Fenix 7X Pro connects stably, shows pace and distance during activity
- **WiFi HTTP dashboard** — live data, distance reset, powered from treadmill VIN
- **Device Information** — Garmin shows "Shhatrat" / "U1" in foot pod settings
- **Case** — ESP32 in 3D-printed 38-pin enclosure

### Not Working / Abandoned
- **FTMS (0x1826)** — not natively supported by Fenix 7 for treadmills. FTMS CIQ app connects but does not subscribe to NOTIFY. Abandoned in favor of RSC.
- **RF 433MHz remote** — phase 2, not yet implemented
- **Speeds 4.1+** — not captured

### Bug History

| Problem | Cause | Fix |
|---------|-------|-----|
| Garmin drops every 10s | 128-bit UUID + SC Control Point with INDICATE | 16-bit UUID everywhere, Feature=0x0000 |
| LUT miss for 2.3, 2.4 (CP210x→ESP32) | Collision at byte[9] in 16B; CP210x saw different value | keyStart=10 instead of 7 |
| LUT miss for 3.0, 3.2, 3.3, 3.5, 4.0 | Collision at byte[10] in 17B for speeds ≥3.0 | Measured ESP32 byte[10] directly — unique value per speed |
| LUT miss for 3.6 | Collision at both byte[10] and byte[13] | Both bytes measured from ESP32 |
| 3.4, 3.8 missing from LUT | 18B format unknown from CP210x | Added 18B detection (rxBuf[17]==0xF8), new entries |
| 3.9 occasionally misses | byte[8] has 2 variants (0x80 and 0x06) | Two LUT entries for 3.9 |
| Dashboard not starting | WiFi draws too much current from treadmill 3V3 | Connect treadmill VIN to ESP32 VIN |
| "No serial data" when flashing | ESP32 powered from treadmill (not USB) | Manual BOOT+EN to enter bootloader |

## Tools

| File | Description |
|---|---|
| `esp32/src/main.cpp` | ESP32 firmware — UART parser + BLE RSC + WiFi dashboard |
| `esp32/src/lut.h` | Lookup table: 31 entries, speeds 1.1–4.0 km/h |
