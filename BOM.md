# Bill of Materials - RC Ball-Launching Buggy

Contest BOM cap is **$250 USD**. The AliExpress order below totals $112.95 CAD,
roughly $80-83 USD at typical exchange rates - comfortably under the cap
either way, so the exact conversion rate doesn't matter here. This splits the
real AliExpress order (new spend) from parts already owned or salvaged, since
only Blueprint can confirm whether owned parts need to be counted at all.

## New spend — AliExpress order, Jul 16 2026

| Item | Store | Qty | Price (CAD) |
|---|---|---|---|
| HobbyWing QuicRun 1060 60A brushed ESC | XG FPV Store | 1 | $27.63 |
| OVONIC 3S 2200mAh 35C LiPo, 11.1V | GAONENG HOBBY Store | 1 | $34.35 |
| 30A battery main switch w/ XT60 | Menglai Technology Store | 1 | $12.18 |
| Silicone wire, 14AWG, 2m (1m red + 1m black) | Hobbymodel Factory Store | 1 | $3.13 |
| 10-way power distribution bus bar, 100A | Minespeed Store | 1 | $13.68 |
| XT60 male/female connectors, 5 pairs | Luyanmaoyi Store | 1 | $5.90 |
| RV2 ring terminals, 16-14AWG, 10pcs | Ali World Shopping Center Store | 1 | $1.72 |
| XT60 parallel Y-splitter (1 male to 2 female) | Hobbymodel Factory Store | 1 | $3.12 |
| Brushless 40A ESC w/ 5V 3A UBEC | ZDRACING RC MODEL Store | 1 | $8.11 |
| Heat shrink tube kit, 127pcs mixed size | DIY Maker Store | 1 | $3.13 |
| **Total** | | | **$112.95** |

## Owned / salvaged - no new spend

Prices intentionally left blank rather than guessed. Fill in only if Blueprint
confirms owned parts need a value against the cap.

| Item | Source |
|---|---|
| Freenove ESP32-WROOM DevKit | owned |
| PCA9685 16-channel PWM driver board | owned |
| 2x 1300kv brushless flywheel motors | owned (prior RC plane build) |
| Flywheel ESC (existing, ~30A, flywheel motor 1) | salvaged from a prior RC plane project |
| 2x RC plane landing gear wheels (flywheel grip wheels) | salvaged from a prior build |
| Toy ride-on car chassis (front-steering + rear-drive halves) | salvaged, already gutted |
| Gate servo | owned |
| 5V 1A portable USB power bank (logic power - see Power below) | owned, not on the AliExpress order |
| Deck rail beams | cut from a broken bed frame |
| Ball-storage net | pulled from an old hockey net |
| Tower wood - 2x diagonally braced 1x3 poplar columns + 1x2 poplar bracing/net-frame | new, Home Depot (not on the AliExpress order above) |

## Power (two separate rails, not one)

- **Motor/ESC power:** 3S LiPo -> physical inline kill switch -> 100A bus bar -> all 3 ESCs (2x flywheel + drive).
- **Logic power:** a small 5V 1A USB power bank feeds the PCA9685's V+ terminal, which also supplies the ESP32 - not a UBEC. Keeping these on two separate batteries, rather than tapping logic power off the ESC/motor rail, is what avoids motor-current voltage sag ever reaching the ESP32.

## Notes

- Both flywheel ESCs together draw well under either unit's rated current on a
  ~25g pickleball. The mismatched 30A/40A pair is fine as-is, each is
  calibrated independently rather than assuming identical PWM means identical RPM.
- The 100A bus bar and 30A main switch are sized for headroom, not because the
  buggy's real combined draw approaches those numbers. Bench testing put
  actual combined peak around 50-90A across all 3 ESCs.
