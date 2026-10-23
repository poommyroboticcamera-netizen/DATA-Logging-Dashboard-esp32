**Language:** English · [ไทย](README_TH.md)

The dashboard and its GitHub Pages demo start in English. Select **ไทย** in the header to switch to Thai; the browser remembers your choice.

# ESP32 Data Logging Dashboard · v2.0

<p align="center"><img src="dashboard/assets/somboon-logo.png" alt="Somboon Advance Technology" width="160"></p>

Dashboard for viewing sensor values ​​and saving data to SD card/CSV via ESP32 using 2.4 GHz Wi‑Fi. Can be viewed from a phone or computer on the same network. Web pages embedded in firmware: Upload both programs and web pages once.

[Try a sample web page](https://poommyroboticcamera-netizen.github.io/DATA-Logging-Dashboard-esp32/) · [How to install](#get-started) · [How to save data](#save-data) · [Solve basic problems](#solve-basic-problems)

> **The sample web page on GitHub Pages uses simulated data** The buttons on the page do not actually command the ESP32 to set up Wi‑Fi or save from the board. Open a web page from the ESP32's IP or setup point. `192.168.4.1`

## What's in v2.0

- View three channel INA226, DHT22, four DS18B20, IMU, RTC, Encoder and live recording status.
- Start/stop recording from web pages or board buttons. Set recording interval **100–60,000 ms** (default **1,000 ms**)
- Save CSV to SD card only while pressing START; When you press STOP, the system finishes writing the remaining data and turns off the SD.
- You can reset your Wi‑Fi from the web page. Even though you still can't connect to the original router, because the ESP32 opens its own Wi‑Fi setting point.
- Read signal **Z of Encoder at GPIO33**; The web page shows **WHEEL RPM (wheel revolutions)** which is calculated from the roller ratio.
- There are GA0–GA3 lights indicating the SD writing status and a line. `[ENC Z]` Serial for checking Encoder cables

The previous version is on the tag. [v1.0](https://github.com/poommyroboticcamera-netizen/DATA-Logging-Dashboard-esp32/tree/v1.0) This version is at [v2.0](https://github.com/poommyroboticcamera-netizen/DATA-Logging-Dashboard-esp32/tree/v2.0) Both models do not have a CAN bus.

## Get started

Must have ESP32 board, USB upload cable, [VS Code + PlatformIO](https://platformio.org/install/ide?install=vscode) or PlatformIO CLI and connecting sensors accordingly [table legs](#legs-and-main-equipment) If you want to save to SD, insert the **FAT16/FAT32** card before starting (exFAT doesn't work).

1. Download or clone a project Then create a personal Wi‑Fi file from the example file:

   ```powershell
   git clone https://github.com/poommyroboticcamera-netizen/DATA-Logging-Dashboard-esp32.git
   cd DATA-Logging-Dashboard-esp32
   Copy-Item include/wifi_config.example.h include/wifi_config.h
   ```

   On macOS/Linux use `cp include/wifi_config.example.h include/wifi_config.h` instead `Copy-Item` The Wi‑Fi 2.4 GHz name/password will be entered in `include/wifi_config.h` You can now, or leave the sample text and set it via the ESP32 settings after uploading the file. `wifi_config.h` It was ignored by Git and should not have been publicly pushed.

2. Connect the board via USB, select the correct port, then press **Upload** in PlatformIO or use the command:

   ```sh
   pio run -e esp32dev -t upload
   pio device monitor -b 115200
   ```

   Close other Serial Monitors before uploading. After uploading, there is no need to copy the HTML file to SD again.

3. Open the dashboard in one of the following ways:

   - **Through the router:** Connect the phone/computer to the same Wi‑Fi and open the IP that the board shows via Serial, e.g. `http://192.168.1.50/`
   - **Through the ESP32 setup point:** Connect Wi‑Fi name `ESP192.168.4.1` code `88888888` then open `http://192.168.4.1/` If the board can connect to the router The set point name may be changed to `ESP` Followed by the IP that the router gives out, for example: `ESP192.168.1.50`

4. To change the router, press the menu **Wi‑Fi Settings**, enter the SSID/password of the **2.4 GHz** network, and press **Save and reconnect** The board will restart; After that, open a new IP or return through the settings point.

> When connected to the ESP32's setup point, the phone may say “No Internet”, keep this connection open to open `192.168.4.1` If the web page after upload looks old Refresh without using the cache (`Ctrl+F5`)

## Save data

1. Make sure the web page shows the current settings and that a FAT16/FAT32 SD card is inserted. Set the **record interval** as desired; The default value is 1000 ms and the board remembers the set value. (The board will only detect the actual card when Start is pressed.)
2. Press **Start Recording** on the web or the START button on the board. The board will create a new recording cycle and begin writing the CSV to SD.
3. Press **Stop recording** on the web or STOP button, wait for the board to write the stuck row and turn off the SD before removing the card.
4. Files on SD are in folder `/logs/` such as `log_00001.csv` If you remove the card after turning off the SD card and can then open the file on your computer, the **Logs** page will only display the CSV copy your browser stored and allow you to download/delete that copy. Didn't delete files on SD

When not recorded The board still displays the current value but does not enable SD read/write. Setting the interval to 100 ms does not cause all sensors to read again every 100 ms; Which values ​​update more slowly will use the latest value with data age indicator.

### What do the GA0–GA3 lights mean?

| status | GA lights |
|---|---|
| The system starts up or starts recording but the SD has not yet written the first row. | All 4 are attached. |
| SD has successfully written the first row and is saving. | Running lights GA3 → GA0 according to the recording time. |
| Press stop and wait for SD to turn off. | All 4 are attached. |
| Recording stopped and SD turned off successfully. | All 4 lights go out. |
| SD write error or after reset that does not turn on the power again | Put on all 4 lights to warn. |

The warning light relies on the MCP23017 and its power supply. If this device or the I²C is inoperable, power status cannot be guaranteed.

## Understand Encoder and CSV values

- Signal wires **Z → GPIO33**, GND must be together and the signal entering the ESP32 pin must not exceed **3.3 V**. If the Encoder sends 5 V, it must convert the level first.
- The default value assumes that Z gives **1 pulse per 1 Encoder revolution** Please check the actual encoder specs. The system doesn't read A/B so it doesn't know the direction of rotation.
- Encoder roller diameter **43 mm** contact with wheels **63 mm** therefore `Wheel RPM = RPM Encoder × 43/63` When not slipping, the **WHEEL RPM** cards on the web use this value, not the motor's PWM value.
- In the CSV column `RPMencoder` and `shaft_rpm` is around the encoder axis; `wheel_rpm` is the calculated wheel circumference. If the roller slips Wheel rpm/speed numbers will be inaccurate.
- INA226: **Battery 0x44 → Vbat/Ibat**, **Rotary 0x45 → Vrotary/Irotary**, **Camera 0x40 → Vcamera/Icamera**; `Iwheel = Ibat - Irotary - Icamera` The default Rshunt for each channel is 1 mΩ and should be set to match the actual hardware.
- The CSV file also contains RTC time, temperature, IMU, sensor accuracy/lifetime status, pulse count, and recording interval. Start a new file `sequence` will start counting again

The Z signal at 1 pulse/cycle of a 43 mm roller has a distance of approximately 135 mm per pulse, making it more suitable for reading revolutions than fine distances. And at least two pulses must be obtained before the first RPM is estimated.

## Legs and main equipment

| equipment | ESP32 / I²C pin |
|---|---|
| I²C SDA / SCL | GPIO21 / GPIO22 |
| INA226 Camera / Rotary / Battery | `0x40` / `0x45` / `0x44` |
| IMU MPU6050/MPU6500 · RTC MCP7940N · MCP23017 | `0x68` or `0x69` · `0x6F` · `0x27` |
| Encoder Z | GPIO33 |
| DHT22 | GPIO4 |
| DS18B20 1–4 | GPIO27 / 16 / 13 / 14 (separate DATA cable) |
| SD CS / SCK / MISO / MOSI | GPIO5 / 18 / 19 / 23 |
| START / STOP button | GPIO36 / GPIO39, active LOW, requires external pull-up |
| Debug light | GPIO2 |

check [Hardware documentation](hardware/README.md) and the circuit diagram of the actual board before connecting power. Some old schematic versions specify START as GPIO34, but the default firmware v2.0 uses GPIO36.

## Solve basic problems

| symptom | Points that should be checked |
|---|---|
| ESP32's Wi‑Fi cannot be found. | Verify that v2.0 has been uploaded, power supply and Serial 115200; AP name may change from `ESP192.168.4.1` is `ESP` followed by the IP of the board |
| Can't press record on Wi‑Fi | The web page served by the ESP32 must be opened, not the demo page on GitHub or the file. `tmp/dashboard-redesign.html` |
| Encoder is `—` or not add pulses | Check Z at GPIO33, GND common, level 3.3 V, open to read Encoder; view line `[ENC Z]` via Serial `total` It should be added when rotating. |
| The wheel circumference isn't really straight. | Check Z pulse per revolution, 63/43 ratio, actual wheel size and roller slippage. |
| SD has no files/GA light stays on | Check FAT16/FAT32 card, SPI cable, `sd_status`/`sd_written`; Do not remove the card before STOP turns off the SD. |
| The board resets itself. | Read why `/api/health` and see [Troubleshooting guide](docs/troubleshooting.md) |

## For developers

```sh
node dashboard/core.test.cjs
node dashboard/imu-model.test.cjs
node scripts/test_ga_demo.cjs
python scripts/test_firmware_csv.py
pio run -e esp32dev
```

`src/main.cpp` is firmware, `dashboard/` is the web page embedded in the program, `scripts/build_dashboard_demo.py` Create a demo page, `hardware/` Documents circuits and PCBs. See API/logging for details. [dashboard/README.md](dashboard/README.md)

> **Security:** The setup point uses the default code specified above and there is no HTTP login on the board. It should only be used on trusted networks. Do not open the board port directly to the internet. If used with real equipment Test the hardware for accuracy and durability before continuing use.
