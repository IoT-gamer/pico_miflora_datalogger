# Pico-W MiFlora BLE Datalogger

This project turns a Raspberry Pi Pico W into a datalogger for a Xiaomi Miflora plant sensor. It scans for the sensor via BLE, reads its data, and logs it to a text file on an SD card with a timestamp.

## Key Features

* Scans for a specific MiFlora sensor via Bluetooth LE.
* Reads temperature, moisture, light, conductivity, and battery level.
* Saves data to a `miflora_log.txt` file on an SD card.
* Adds an ISO 8601 timestamp (e.g., `2025-10-23T20:30:00`) to each reading using the Pico's internal Real-Time Clock (RTC).

## Hardware Required

* Raspberry Pi Pico W or Pico 2 W
* Xiaomi MiFlora plant sensor
* MicroSD card module (SPI)
* MicroSD card (formatted as FAT32)

## Wiring

This is critical! The Pico W's onboard CYW43 wireless chip uses `spi0`. To avoid conflicts, this project is configured to use `spi1` for the SD card .

Connect your SD card reader to the following `spi1` pins:

| SD Card Pin |	Pico Pin (GPIO) |
|-------------|------------------|
| SCK | `GPIO 10` |
| MOSI  | `GPIO 11` |
| MISO | `GPIO 12` |
| CS | `GPIO 13` |
| VCC | `3V3_OUT` (3.3V) or `VBUS` (5V) |
| GND | GND |

## How to Use

### **Configure Sensor MAC Address**

You must update `main.c` with your specific Miflora sensor's MAC address.

Find this line in `main.c`:

```c
static const char * target_mac_string = "5C:85:7E:13:17:F9"; // <-- CHANGE THIS
```

Replace the address with your sensor's MAC address.

### **Set the Time**

The Pico's internal RTC does not have a battery and will reset every time it loses power. You must set the correct time in `main.c` before flashing the code.

Find this block in the `main()` function:

```c
    datetime_t t = {
        .year  = 2025,
        .month = 10,
        .day   = 23,
        .dotw  = 4,    // Thursday
        .hour  = 20,   // 8 PM
        .min   = 30,
        .sec   = 0
    };
```

Update these values to the current date and time. For example, for October 23, 2025, 8:30 PM

**Note:** For a long-term solution, you would need to add an external battery-backed RTC (like a DS3231) or implement NTP time-sync over Wi-Fi.

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
After booting, the Pico will scan for the sensor. Once it connects and takes a reading, it will write the data to the `miflora_log.txt` file on the SD card.

The output will look like this:
```
2025-10-23T20:30:05,Temp:28.5,Light:150,Moisture:45,Conductivity:350,Battery:88
2025-10-23T20:30:35,Temp:28.5,Light:152,Moisture:45,Conductivity:350,Battery:88
2025-10-23T20:31:04,Temp:28.4,Light:149,Moisture:45,Conductivity:349,Battery:88
```

## Dependencies & Acknowledgements

This project relies on several key libraries and examples:

* **Raspberry Pi Pico SDK**

* [**pico-examples/pico_w/bt/standalone/**](https://github.com/raspberrypi/pico-examples/tree/master/pico_w/bt/standalone)

* **BTstack** for Bluetooth LE functionality on the Pico W.

* [**no-OS-FatFS-SD-SDIO-SPI-RPi-Pico**](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico) library by Carlk3 for the SD card and FatFs driver