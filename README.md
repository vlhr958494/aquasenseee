# AQUASENSE – IoT water potability test logger (ESP32)

ESP32 device that measures **pH, TDS, turbidity and temperature**, derives **EC and dissolved oxygen** by
firmware correlation, scores the six parameters against WHO-based limits, shows the result on a 20x4 LCD and
logs **one row per test** to a Google Sheet.

> Screening tool only. It does not detect bacteria, heavy metals, nitrates or pesticides and is not a
> replacement for laboratory testing. EC and DO are estimates, not direct measurements.

## How a test works
1. Put the probes in the sample. The LCD shows live readings and `PRESS TO START TEST`.
2. Press the push button. The device measures for 60 s (first 15 s ignored, last 45 s averaged).
3. The result is saved in flash and uploaded once to Google Sheets. LCD shows `UPLOAD: COMPLETE` or `UPLOAD: FAILED`.
4. With no Wi-Fi the result stays in flash and is uploaded automatically later (duplicates are ignored by the sheet).
5. Verdict: **5–6 of 6 parameters acceptable = SAFE**, otherwise UNSAFE.

LCD corner indicator (row 1, right): `WF` = Wi-Fi connected, `XX` = not connected.

## Hardware / pins
| Part | ESP32 pin |
|---|---|
| DS18B20 temperature (4.7 kΩ pull-up to 3.3 V) | GPIO4 |
| SEN0244 TDS | GPIO35 |
| SEN0161 pH | GPIO34 |
| SEN0189 turbidity (**10 kΩ / 20 kΩ divider required**) | GPIO32 |
| 20x4 I²C LCD (SDA / SCL, powered from 5 V) | GPIO21 / GPIO22 |
| Start-test push button (other leg to GND) | GPIO27 |

Power: 12 V/10 W solar panel → charge controller → 3S Li-ion + BMS → master switch → LM2596 buck (5 V).

## Software setup (VS Code + PlatformIO)
1. Install VS Code and the **PlatformIO IDE** extension. Open this folder.
2. Copy `include/secrets.example.h` to `include/secrets.h` and fill in Wi-Fi, script URL and token.
3. **Google Sheet:** create a sheet → *Extensions → Apps Script* → paste `google_apps_script/Code.gs`,
   set `SECRET_TOKEN` → *Deploy → New deployment → Web app* (Execute as **Me**, access **Anyone**) → copy the URL
   into `secrets.h`. Results appear in a tab called **Tests**.
4. Build and upload with PlatformIO. Serial Monitor: 115200 baud.

## Calibrate before real use
Constants are at the top of `src/main.cpp`: `PH_SLOPE` / `PH_OFFSET` (pH 4 and 7 buffers), `TDS_CAL`
(known-ppm solution), turbidity divider ratio, `KE_FACTOR` (TDS→EC).

## Notes
- ESP32 supports **2.4 GHz** Wi-Fi only.
- Uploads use HTTPS but do not pin Google's certificate.
- Do not commit `include/secrets.h` or publish your Apps Script URL/token.
