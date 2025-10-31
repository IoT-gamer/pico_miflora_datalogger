# Pico-W MiFlora BLE Datalogger

[![Work in Progress](https://img.shields.io/badge/status-work%20in%20progress-yellow)](https://example.com/your-project-status-page)

This project turns a Raspberry Pi Pico W into a datalogger for a Xiaomi Miflora plant sensor. It operates in two modes:

1. **Client Mode:** (After time-sync) Scans for the sensor via BLE, reads its data (temperature, moisture, light, conductivity, and battery), and logs it to a text file on an SD card with a timestamp.

2. **Server Mode:** On boot (and before the clock is set), it advertises as "**MiFlora Logger**" in 30-second intervals. After its clock is synced, it switches to advertising for a long duration (e.g., 15 minutes) between sensor readings. Datalogging will not begin until the time is synced.

## Key Features

* Scans for a specific MiFlora sensor via Bluetooth LE.
* Reads temperature, moisture, light, conductivity, and battery level.
* Saves data to daily log files (e.g., `2025-10-30.txt`) on an SD card.
* Adds an ISO 8601 timestamp (e.g., `2025-10-23T20:30:00`) to each reading using the Pico's internal Real-Time Clock (RTC).
* Acts as a BLE peripheral (server) to allow remote time-syncing of the RTC. **This step is mandatory before logging will start**.
* Exposes a BLE service to read log files directly from the SD card.

## Hardware Required

* Raspberry Pi Pico W or Pico 2 W
* Xiaomi MiFlora plant sensor
* MicroSD card module (SPI)
* MicroSD card (formatted as FAT32)
* A smartphone with an example companion app: [Flutter MiFlora Companion App](https://github.com/IoT-gamer/flutter_miflora_companion_app/tree/main)

## Reading Log Data

In addition to logging, the device can act as a file server over BLE.
The [companion app](https://github.com/IoT-gamer/flutter_miflora_companion_app) can request log files by date.

The device exposes two new characteristics on its `0xAAA0` service:
* **Command Characteristic (`0xAAA2`):** A `WRITE` characteristic. The app writes a command like `GET:2025-10-31.txt` to it.
* **Data Characteristic (`0xAAA3`):** A `NOTIFY` characteristic. The Pico reads the file from the SD card and streams its contents back to the app in chunks.

## Wiring

Use `hw_config.c` to configure the pins for your SD card module.
The default setting is configured to use `spi1` for the SD card .

Connect your SD card reader to the following `spi1` pins:

| SD Card Pin |	Pico Pin (GPIO) |
|-------------|------------------|
| SCK | `GPIO 10` |
| MOSI  | `GPIO 11` |
| MISO | `GPIO 12` |
| CS | `GPIO 13` |
| VCC | `3V3_OUT` (3.3V) or `VBUS` (5V)* |
| GND | GND |
*Note: Some SD card modules require 5V power. Check your module's specifications.

## How to Use

### **Configure Sensor MAC Address**

You must update `main.c` with your specific Miflora sensor's MAC address.

Find this line in `main.c`:

```c
static const char * target_mac_string = "5C:85:7E:13:17:F9"; // <-- CHANGE THIS
```

Replace the address with your sensor's MAC address.

### **Set the Time (Mandatory)**

The Pico's internal RTC does not have a battery and will reset every time it loses power. You must set the correct time via BLE before the device will begin datalogging.

1. Power on or reset your Pico W.

2. Open the [Flutter MiFlora Companion App](https://github.com/IoT-gamer/flutter_miflora_companion_app/tree/main) on your smartphone.

3. The Pico will advertise as "**MiFlora Logger**". Before the time is synced, it will do this in 30-second cycles. If you miss the window, it will time out and start advertising again, so you don't need to rush.

4. Tap the "Scan" icon in the app.

5. Your "MiFlora Logger" should appear..

6. Tap on it to connect.

7. Once connected, tap the "Sync Current Time" button.

8. Disconnect from the device. The Pico will now detect that its clock is synced. It will enter its long-interval logging cycle (e.g., 15 minutes), after which it will perform its first scan for the MiFlora sensor and begin datalogging.

### **Build and Flash**

1. Ensure you have the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) installed and configured.

2. Create a `build` directory:
    ```bash
    mkdir build
    cd build
    ```
3. Run CMake to pull dependencies and configure the project:
    ```bash
    cmake ..
    ```
4. Run Make to build the project:
    ```bash
    make
    ```

5. Put your Pico W into BOOTSEL mode (hold the BOOTSEL button while plugging it in).

6. Copy the generated `.uf2` file to the Pico:
    ```bash
    cp pico_miflora_datalogger.uf2 /media/user/RPI-RP2
    ```

**Note:** Use VSCode with the official [Pico extension](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico) for easier building and flashing.

**Note:** Ensure your SD card is inserted into the SD card module before powering on the Pico W.

## Log File Output
After booting and syncing the time, the Pico will scan for the sensor. Once it takes a reading, it will create or append to a file on the SD card named after the **current date**.

For example, all readings taken on October 30, 2025, will be saved in a file named `2025-10-30.txt`.

The output inside the file will look like this:
```
2025-10-30T08:30:05,Temp:28.5,Light:150,Moisture:45,Conductivity:350,Battery:88
2025-10-30T09:00:12,Temp:28.5,Light:152,Moisture:45,Conductivity:350,Battery:88
2025-10-30T09:30:07,Temp:28.4,Light:149,Moisture:45,Conductivity:349,Battery:88
```

## Dependencies & Acknowledgements

This project relies on several key libraries and examples:

* **Raspberry Pi Pico SDK**

* [**pico-examples/pico_w/bt/standalone/**](https://github.com/raspberrypi/pico-examples/tree/master/pico_w/bt/standalone)

* **BTstack** for Bluetooth LE functionality on the Pico W.

* [**no-OS-FatFS-SD-SDIO-SPI-RPi-Pico**](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico) library by Carlk3 for the SD card and FatFs driver

* [**miflora**](https://github.com/basnijholt/miflora) Bas Nijholt's MiFlora BLE protocol documentation and Python library.