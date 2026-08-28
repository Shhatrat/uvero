# Uvero U1 — UART Reverse Engineering

## Sprzęt

| Element | Opis |
|---|---|
| Bieżnia | **Uvero U1** (chiński OEM, podobny do Sperax/Deerrun/PitPat) |
| Adapter (faza 1) | Silicon Labs **CP210x** (ID `10c4:ea60`) → `/dev/ttyUSB0` |
| Mikrokontroler | **ESP32 DevKit** (38-pin) — UART odczyt + BLE RSC + WiFi dashboard |
| Obudowa ESP32 | [MakerWorld: ESP32 case 38-pin](https://makerworld.com/pl/models/1029542-esp32-case-38-pin-customize-or-ready-print?from=search#profileId-1016748) — druk 3D |

## Parametry UART

| Parametr | Wartość |
|---|---|
| Baud rate | **2400** |
| Format | 8N1 |
| Napięcie | TTL 3.3V |
| Pin ESP32 | GPIO16 (Serial2 RX) |

## Architektura protokołu

- Magistrala **half-duplex** — jedna linia TX/RX wspólna dla konsoli i silnika
- Konsola i silnik gadają na zmianę (ale ich transmisje lekko nachodzą na siebie — kolizja)
- Pakiety wysyłane co ~300 ms
- Metryki (czas, dystans, kalorie, kroki) liczone są przez konsolę wewnętrznie — UART niesie tylko aktualną prędkość silnika

## Struktura pakietu

Trzy formaty: 15B (stop), 16B, 17B, 18B:

### 15-bajtowy — bieżnia stoi

```
80 1E 80 00 00 00 00 00 00 FE 80 7E 60 80 F8
 0  1  2  3  4  5  6  7  8  9 10 11 12 13 14
```

Bajt [4]=`00` → prędkość=0.

### 16-bajtowy

```
80 1E 80 00 06 00 E6 [A] [B] 00 FE 80 [C] [D] [E] F8
 0  1  2  3  4  5  6  7   8  9 10 11  12  13  14  15
```

- `[0-3]` = `80 1E 80 00` — nagłówek stały (sync)
- `[4]` = `06` — jedzie
- `[5-6]` = `00 E6` — stałe
- `[7-8]` — nibble bytes konsoli (komenda prędkości)
- `[9]` = `00` — separator (start odpowiedzi silnika), **bajt kolizji**
- `[10-11]` = `FE 80` — stały separator (po kolizji)
- `[12-14]` — nibble bytes silnika (potwierdzenie prędkości)
- `[15]` = `F8` — end byte

### 17-bajtowy

```
80 1E 80 00 06 00 E6 [A] [B] [C] [D] FE 80 [E] [F] [G] F8
 0  1  2  3  4  5  6  7   8   9  10  11  12  13  14  15  16
```

- `[7-9]` — nibble bytes konsoli (3 bajty)
- `[10]` — **bajt kolizji** (dla wyższych prędkości ≥ 3.0 km/h)
- `[11-12]` = `FE 80` — separator (po kolizji)
- `[13-15]` — nibble bytes silnika
- `[16]` = `F8` — end byte

### 18-bajtowy (tylko 3.4 i 3.8 km/h)

```
80 1E 80 00 06 00 E6 [A] [B] [C] [D] 00 FE 80 [E] [F] [G] F8
 0  1  2  3  4  5  6  7   8   9  10  11 12  13  14  15  16  17
```

- `[7-10]` — nibble bytes konsoli (4 bajty)
- `[11-13]` = `00 FE 80` — separator (przesunięty o 1 bajt względem 17B)
- `[14-16]` — nibble bytes silnika
- `[17]` = `F8` — end byte

Format 18B **nie był znany z CP210x** — odkryty dopiero przez bezpośredni odczyt z ESP32.

## Enkodowanie bajtów — nibble encoding

Każdy bajt danych koduje **4-bitową nibblę** w bitach [7,6,4,2]:

```
nibble = bit7<<3 | bit6<<2 | bit4<<1 | bit2
```

Redundantne kopie dla detekcji błędów:
- bit5 = bit6
- bit3 = bit4
- bit1 = bit2
- bit0 = zawsze 0

Tabela bajtów:
```
0x00→0   0x60→4   0x80→8   0xE0→12
0x06→1   0x66→5   0x86→9   0xE6→13
0x18→2   0x78→6   0x98→10  0xF8→14
0x1E→3   0x7E→7   0x9E→11  0xFE→15
```

---

## Kluczowe odkrycie: CP210x vs ESP32 — różnice bajtów kolizji

> **Uwaga wstępna:** Nikt wcześniej nie dokumentował tego problemu dla tego protokołu.
> Ta sekcja jest wynikiem empirycznej analizy podczas integracji ESP32.

### Root cause — fizyczna pozycja na magistrali half-duplex

Protokół Uvero U1 używa magistrali half-duplex, gdzie konsola i silnik współdzielą jedną linię transmisji. Ich transmisje **nakładają się w czasie** — konsola wysyła koniec komendy jednocześnie gdy silnik zaczyna odpowiedź. W strefie nakładania (kolizji) bajty na linii są superpozycją obu sygnałów.

**CP210x** jest podłączony jako **pasywny słuchacz** — podpięty do linii TX bieżni osobnym kablem, w punkcie fizycznie odległym od styku konsola–silnik. Widzi sygnał dominujący (konsolę), zanim kolizja zdąży go zmienić.

**ESP32 GPIO16** jest podłączony **bezpośrednio do tej samej linii** co silnik i konsola — w samym centrum magistrali. W strefie kolizji ESP32 widzi wypadkowy sygnał obu nadajników: rezultat elektrycznej superpozycji napięć konsoli i silnika. Ten wypadkowy sygnał jest **deterministyczny** (dla danej prędkości silnik zawsze odpowiada tymi samymi danymi), ale **inny** niż to co widzi CP210x z boku.

### Strefy kolizji wg formatu pakietu

#### Format 16B (prędkości: 1.1, 1.2, 2.2–2.4, 2.8, 2.9, 3.6)

```
Bajty:  [7]  [8]  [9] [10] [11] [12] [13] [14] [15]
         A    B   COL  FE   80   C    D    E    F8
                  ^^^
                  kolizja — tu konsola kończy, silnik zaczyna
```

- Bajt `[9]` = `0x00` (separator) jest bajtem kolizji. Dla większości prędkości 16B CP210x widział go jako `0x00`, ESP32 widzi go zmiennie (nieczytelnie).
- Bajty `[10-15]` = `FE 80 [C][D][E] F8` są **po kolizji** — CP210x i ESP32 widzą te same wartości.
- **Rozwiązanie**: użyj `keyStart=10`, `keyLen=6` (bajty `[10..15]`), pomijając `[7-9]`.

**Wyjątek — prędkość 3.6 km/h**: kolizja obejmuje też bajt `[10]` i `[13]`:

| Bajt | CP210x widzi | ESP32 widzi | Komentarz |
|------|-------------|-------------|-----------|
| [9]  | `1E` | (niestabilny) | kolizja konsola+silnik |
| [10] | `00` | `18` | kolizja — silnik daje `18` zamiast `00` |
| [13] | `00` | `60` | kolizja — bajt odpowiedzi silnika |

Dla 3.6 keyLen=6 od pozycji 10: `{0x18, 0xFE, 0x80, 0x60, 0x1E, 0xF8}`.

#### Format 17B (prędkości: 1.3–2.7)

```
Bajty:  [7]  [8]  [9]  [10] [11] [12] [13] [14] [15] [16]
        COL   B    C    00   FE   80   D    E    F    F8
        ^^^
        tylko bajt [7] jest w strefie kolizji
```

- Bajt `[7]` — kolizja. CP210x widzi wartość z wychodzącego strumienia konsoli, ESP32 widzi superpozycję.
- Bajty `[8-16]` — po kolizji, **identyczne** między CP210x i ESP32.
- **Rozwiązanie**: użyj `keyStart=8`, `keyLen=9` (bajty `[8..16]`).

Dla prędkości 1.3–2.7 klucze w LUT są **identyczne** z wartościami CP210x (kolizja nie dotknęła używanych bajtów).

#### Format 17B — prędkości ≥ 3.0 km/h (dodatkowa kolizja)

```
Bajty:  [7]  [8]  [9]  [10] [11] [12] [13] [14] [15] [16]
        COL   B    C   COL2  FE   80   D    E    F    F8
        ^^^            ^^^^
        kolizja 1     kolizja 2 — tu powtórna superpozycja
```

Przy wyższych prędkościach konsola wysyła 3 bajty prędkości (`[7-9]`) zamiast 2, przez co bajt `[10]` (normalnie `0x00` separatora) **też wpada w okno kolizji**. ESP32 widzi tam wartość wypadkową konsoli+silnika, CP210x widzi `0x00`.

Kompletna tabela różnic dla 17B prędkości ≥ 3.0 km/h:

| km/h | klucz CP210x (bytes[8..16]) | klucz ESP32 (bytes[8..16]) | bajt różniący |
|------|-----------------------------|---------------------------|--------------|
| 3.0  | `00 1E 00 FE 80 1E 66 80 F8` | `00 1E 60 FE 80 1E 66 80 F8` | [10]: `00`→`60` |
| 3.2  | `00 00 00 FE 80 00 80 1E F8` | `00 00 06 FE 80 00 80 1E F8` | [10]: `00`→`06` |
| 3.3  | `00 18 00 FE 80 18 9E 80 F8` | `00 18 7E FE 80 18 9E 80 F8` | [10]: `00`→`7E` |
| 3.5  | `80 78 00 FE 80 7E 7E 80 F8` | `80 78 60 FE 80 7E 7E 80 F8` | [10]: `00`→`60` |
| 4.0  | `06 66 00 FE 80 1E 78 80 F8` | `06 66 FE 80 1E 78 80 F8` | [10]: `00`→`FE` + przesunięcie |

Wzorzec: bajt `[10]` w CP210x zawsze `0x00` (widzi separator), w ESP32 ma unikalną wartość powiązaną z danymi silnika (kolizja z początkiem odpowiedzi silnika daje bajt identyfikujący prędkość).

**Prędkości 3.1, 3.7, 3.9** — bajt [10] nie wchodzi w kolizję (prawdopodobnie inna długość komendy konsoli lub różne timing), klucze ESP32 identyczne z CP210x.

#### Format 18B — prędkości 3.4 i 3.8 km/h (format nieznany z CP210x)

Prędkości 3.4 i 3.8 generują pakiety 18-bajtowe. CP210x ich **nie nagrał** — albo z powodu innego timingu transmisji, albo dlatego że kolizja całkowicie zniszczyła bajt `F8` który CP210x brał za koniec 17B pakietu.

Z perspektywy ESP32:
- `byte[17]` = `F8` → pakiet 18B
- Separator przesunięty: `[11-13]` = `00 FE 80`
- `keyStart=12`, `keyLen=6` (bajty `[12..17]`)

| km/h | klucz ESP32 (bytes[12..17]) |
|------|----------------------------|
| 3.4  | `FE 80 98 9E 1E F8` |
| 3.8  | `FE 80 00 7E 80 F8` |

### Dlaczego CP210x nie widział tej różnicy

CP210x jako pasywny słuchacz na linii TX (nie bezpośrednio w pętli sygnałowej):
1. Ma wyższą impedancję wejściową — nie "wchodzi" w kolizję, tylko obserwuje
2. Może być fizycznie za rezystorem lub na odgałęzieniu linii, gdzie kolizja jest tłumiona
3. Widzi sygnał konsoli dominujący (bo jest bezpośrednio na TX konsoli) z minimalnymi zakłóceniami od silnika

ESP32 GPIO16 w tej samej pętli co silnik:
1. Niska impedancja → aktywnie odbiera superpozycję obu sygnałów
2. W oknie kolizji — oba nadajniki (konsola + silnik) faktycznie walczą o linię
3. Wynik kolizji jest deterministyczny dla danej prędkości, więc staje się unikalnym "fingerprinte" prędkości

### Praktyczne implikacje

Jeśli budujesz podobną integrację z innym mikroprocesorem podłączonym bezpośrednio do magistrali half-duplex:

1. **Nie ufaj keyom wyznaczonym przez CP210x** — bajty w strefie kolizji będą inne
2. **Identyfikuj strefy kolizji** — szukaj bajtów które zmieniają wartość między pakietami (nie są stabilne)
3. **Użyj bajtów poza strefą kolizji** jako kluczy, LUB zaakceptuj kolizyjne bajty i zmierz ich wartość bezpośrednio z docelowego układu
4. **Nowe formaty pakietów** mogą się pojawić — CP210x może ich nie widzieć jeśli kolizja maskuje bajt `F8`
5. **Mismatch counter** — przy wykrywaniu stabilnego pakietu dodaj tolerancję (n=3 kolejne różne pakiety zanim reset licznika stabilności) — bajty kolizji mogą się zmieniać między kolejnymi pakietami jeśli transmisja nie jest w pełni zsynchronizowana

---

## Dekodowanie prędkości — lookup table

Enkodowanie prędkości **nie jest prostą funkcją matematyczną** — jedyne niezawodne podejście: dopasowanie wybranych bajtów pakietu do tabeli.

### Zakresy kluczy (wartości ESP32, po kolizji)

| Format | keyStart | keyLen | Bajty |
|--------|----------|--------|-------|
| 15B (stop) | — | — | bajt[4]=`0x00` → prędkość=0 |
| 16B | 10 | 6 | `[10..15]` = `FE 80 [C][D][E] F8` |
| 17B | 8 | 9 | `[8..16]` = `[B][C][COL2][FE][80][D][E][F] F8` |
| 18B | 12 | 6 | `[12..17]` = `FE 80 [E][F][G] F8` |

### Tabela pełna (bajty ESP32 → km/h)

Prędkości 1.3–2.7 km/h (17B): klucze identyczne z CP210x (kolizja tylko w bajt[7]).  
Prędkości 3.0+ (17B): klucze różnią się od CP210x w bajt[10].  
3.4, 3.8 (18B): format nieznany z CP210x.

| km/h | Format | Klucz ESP32 (hex) |
|------|--------|-------------------|
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
| 3.0 | 17B | `00 1E **60** FE 80 1E 66 80 F8` ← bajt[10]=`60` zamiast `00` |
| 3.1 | 17B | `00 7E 86 00 FE 80 7E 1E F8` |
| 3.2 | 17B | `00 00 **06** FE 80 00 80 1E F8` ← bajt[10]=`06` zamiast `00` |
| 3.3 | 17B | `00 18 **7E** FE 80 18 9E 80 F8` ← bajt[10]=`7E` zamiast `00` |
| 3.4 | 18B | `FE 80 98 9E 1E F8` ← format 18B, nieznany z CP210x |
| 3.5 | 17B | `80 78 **60** FE 80 7E 7E 80 F8` ← bajt[10]=`60` zamiast `00` |
| 3.6 | 16B | `**18** FE 80 **60** 1E F8` ← bajt[10]=`18`, bajt[13]=`60` |
| 3.7 | 17B | `80 00 00 FE 80 06 86 1E F8` |
| 3.8 | 18B | `FE 80 00 7E 80 F8` ← format 18B, nieznany z CP210x |
| 3.9 | 17B | `80 18 86 00 FE 80 60 1E F8` (wariant A) |
| 3.9 | 17B | `06 18 86 00 FE 80 60 1E F8` (wariant B — bajt[8] `80`→`06`) |
| 4.0 | 17B | `80 06 66 FE 80 1E 78 80 F8` |

Brakuje w LUT: 4.1+.

**3.9 ma dwa warianty** — bajt[8] zmienia się między `0x80` i `0x06`. Obydwa dodane do LUT.

### Wzorzec nibble [7] (zakres prędkości)

| n7 | Bajt[7] | Zakres |
|---|---|---|
| 0 | `0x00` | 1.0–2.9 km/h |
| 8 | `0x80` | 2.9–3.2 km/h |
| 9 | `0x86` | 3.3–3.6 km/h |
| 10 | `0x98` | 3.7–4.0 km/h |

## Parser pakietów (ESP32 — logika)

Synchronizacja: szukaj nagłówka `80 1E 80 00` (4 bajty). Następnie:

```
if  (rxLen >= 18 && rxBuf[17] == 0xF8) → pktLen = 18
elif(rxLen >= 17 && rxBuf[16] == 0xF8) → pktLen = 17
elif(rxLen >= 16 && rxBuf[15] == 0xF8) → pktLen = 16
elif rxLen < 18                         → czekaj na więcej bajtów

keyStart = (pktLen==18) ? 12 : (pktLen==17) ? 8 : 10
keyLen   = pktLen - keyStart
```

Sprawdzaj 18B **przed** 17B — inaczej 18B pakiet zostanie błędnie zinterpretowany jako 17B (bajt[17]=F8 nigdy nie zostanie sprawdzony).

## Integracja ESP32 — schemat połączeń

```
Bieżnia VIN (7.5V) ──────────────────► ESP32 VIN (nie 3V3!)
Bieżnia GND  ────────────────────────► ESP32 GND
Bieżnia TX   ────────────────────────► ESP32 GPIO16 (RX2)
```

**Uwaga: VIN, nie 3V3.** Pin 3V3 bieżni nie daje wystarczającego prądu dla WiFi+BLE (~300–500 mA piki). VIN → wbudowany regulator ESP32 obsługuje te piki.

**Uwaga: gdy ESP32 podłączony do VIN bieżni i USB jednocześnie** — odłącz bieżnię od prądu przed podłączeniem USB (napięcia nie kolidują ale lepiej nie ryzykować). Po wgraniu firmware: odłącz USB, podłącz bieżnię.

## BLE RSC — konfiguracja firmware (działająca)

```
RSC Service 0x1814
  ├── 0x2A54 RSC Feature = 0x0000 (READ)
  ├── 0x2A5D Sensor Location = 7 / foot (READ)
  └── 0x2A53 RSC Measurement (NOTIFY) — 4 bajty: flags=0x00, speed_L, speed_H, cadence
DIS 0x180A
  ├── 0x2A29 Manufacturer = "Shhatrat" (READ)
  ├── 0x2A24 Model Number = "U1" (READ)
  ├── 0x2A25 Serial Number = "001" (READ)
  └── 0x2A26 FW Revision = "1.0" (READ)
Advertising: UUID 0x1814, appearance 0x0481 (Running Sensor)
```

**Krytyczne zasady (powody dropów na Garminie):**
- UUID **musi być 16-bit wszędzie**: `NimBLEUUID((uint16_t)0x1814)` — nie string
- RSC Feature=`0x0000` → brak SC Control Point → stabilne połączenie
- RSC Feature=`0x0003` → SC Control Point z INDICATE → Garmin bug → dropi co ~10s
- Cadence musi być niezerowe przy prędkości > 0

## WiFi dashboard

ESP32 serwuje HTTP na porcie 80. Odczyt z `/json`, reset dystansu przez `/reset_dist`.

Wyświetla: prędkość, dystans, uptime, temperatura, BLE clients, UART status, RAM, CPU MHz.

Źródło: [github.com/Shhatrat/uvero](https://github.com/Shhatrat/uvero)

## Status projektu (2026-08-28) — KOMPLETNY ✓

### Co działa ✓
- **UART odczyt** — live dekoder prędkości, wszystkie prędkości 1.1–4.0 km/h
- **Lookup table** — 31 wpisów (30 prędkości + 3.9 wariant B), zero konfliktów
- **BLE RSC** — Fenix 7X Pro łączy się stabilnie, pokazuje tempo i dystans podczas aktywności
- **WiFi HTTP dashboard** — live dane, reset dystansu, zasilanie z VIN bieżni
- **Device Information** — Garmin pokazuje "Shhatrat" / "U1" w ustawieniach foot poda
- **Obudowa** — ESP32 w drukowanej obudowie 38-pin

### Co nie działa / porzucone
- **FTMS (0x1826)** — nieobsługiwany natywnie przez Fenix 7 dla bieżni. FTMS CIQ app łączy się ale nie subskrybuje NOTIFY. Porzucone na rzecz RSC.
- **RF 433MHz pilot** — faza 2, jeszcze nie zrobiona
- **Prędkości 4.1+** — nie nagrane

### Historia błędów i napraw

| Problem | Przyczyna | Fix |
|---------|-----------|-----|
| Garmin dropi co 10s | 128-bit UUID + SC Control Point z INDICATE | 16-bit UUID wszędzie, Feature=0x0000 |
| LUT miss dla 2.3, 2.4 (CP210x→ESP32) | Kolizja w bajt[9] 16B, CP210x widział inaczej | keyStart=10 zamiast 7 |
| LUT miss dla 3.0, 3.2, 3.3, 3.5, 4.0 | Kolizja w bajt[10] 17B dla prędkości ≥3.0 | Pomiar ESP32 dla bajt[10] — unikalna wartość |
| LUT miss dla 3.6 | Kolizja w bajt[10] i bajt[13] jednocześnie | Oba bajty zmierzone z ESP32 |
| 3.4, 3.8 nie w LUT | Format 18B nieznany z CP210x | Detekcja 18B (rxBuf[17]==0xF8), nowe wpisy |
| 3.9 nieregularnie nie działa | Bajt[8] ma 2 warianty (0x80 i 0x06) | Dwa wpisy w LUT dla 3.9 |
| Dashboard nie startuje | WiFi pobiera za dużo prądu z 3V3 bieżni | Podłącz VIN bieżni do VIN ESP32 |
| "No serial data" przy wgrywaniu | ESP32 zasilany z bieżni (nie USB) | Manual BOOT+EN do trybu bootloader |

## Narzędzia

| Plik | Opis |
|---|---|
| `esp32/src/main.cpp` | Firmware ESP32 — UART parser + BLE RSC + WiFi dashboard |
| `esp32/src/lut.h` | Lookup table: 31 wpisów, prędkości 1.1–4.0 km/h |
