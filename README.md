<div class="sag">
    <th><img alt="GitHub License" src="https://img.shields.io/github/license/suleymangunel/ePaperV2?style=plastic&label=License"></th>
    <th><img alt="Static Badge" src="https://img.shields.io/badge/Type-Firmware-gold?style=plastic"></th>
    <th><img alt="Static Badge" src="https://img.shields.io/badge/Language-C-red?style=plastic"></th>
    <th><img alt="Static Badge" src="https://img.shields.io/badge/Platform-ESP32%2FS3-blue?style=plastic"></th>
    <th><img alt="Static Badge" src="https://img.shields.io/badge/Framework-ESP%E2%94%80IDF-white?style=plastic"></th>
    <th><img alt="Static Badge" src="https://img.shields.io/badge/OS-FreeRTOS-black?style=plastic"></th>
</div>

# ePaperV2


## Description
> Minimal ESP-IDF + LVGL driver demo for Waveshare 1.54"v2/2.13"v4 E-Paper (SSD1680) display.

This project demonstrates how to integrate the **LVGL graphics library** with a **Waveshare 1.54"/2.13" e-paper display (SSD1680 controller)** using **ESP-IDF** on an **ESP32-S3 DevKitC-1**.  
It handles full SPI communication, monochrome buffer rotation, LVGL framebuffer flushing, and custom font rendering.
<div>
    <th>
        https://github.com/user-attachments/assets/17ffaf29-3256-4fb5-a992-ef43f3f81d72
    </th>
    <th>
        https://github.com/user-attachments/assets/652cde6e-650d-42f4-9aed-f15a2d02f8eb
    </th>
</div>
---

## 🖥️ Features

- 💡**Update**  
  - Dual support for Waveshare 1.54″v2 and 2.13″v4 (EPAPER_TYPE:WS_154_V2/WS_213_V4)
  - The new rotation feature (ROTATE:0/1)

- 🧠 **SSD1680 Register-Level Driver**  
  Full initialization and control sequence implemented from the official datasheet.

- 🖼️ **LVGL Integration**  
  Converts RGB565 buffers from LVGL into 1-bit monochrome frames with rotation and mirror control.

- 🖋️ **Custom Font Support**  
  Includes `Ink Free` TrueType font converted with `lv_font_conv`.

- 🔁 **Full E-Paper Refresh Handling**  
  Proper busy-pin synchronization and master activation cycle for clean screen updates.

- ⚡ **Optimized SPI Transfer**  
  Chunked DMA-safe transfers for large frame buffers (up to 4 KB per transaction).

---

## 🧩 Hardware Setup

| Signal | ESP32-S3 GPIO | Description        |
|--------|----------------|--------------------|
| `EPD_DC` | 8  | Data/Command select |
| `EPD_RST` | 9  | Hardware reset pin  |
| `EPD_CS` | 10 | SPI chip select     |
| `EPD_BUSY` | 13 | Busy signal (active LOW) |
| `EPD_MOSI` | 11 | SPI MOSI           |
| `EPD_SCLK` | 12 | SPI Clock          |

> ⚠️ Tested with **Waveshare 2.13" V4** (SSD1680).  
> Adjust `EPD_WIDTH`, `EPD_HEIGHT`, and offsets in `main.c` if using another model.

---

## 🧱 Directory Structure
```
📁 root/
│
├── 📁 main/
│   ├── 📄 main.c                # App, LVGL flush, SPI + SSD1680 sequence
│   ├── 📄 ssd1680_regs.h        # SSD1680 register definitions
│   ├── 📁 fonts/
│   │   ├── 📄 ink_free_12.c     # LVGL font (generated)
│   │   └── 📄 ink_free_12.h
│   └── 📄 CMakeLists.txt        # Component registration (LVGL, drivers)
│
└── 📄 README.md
```

## ⚙️ Build Instructions

### 1. Prerequisites
- ESP-IDF v5.2 or newer  
- LVGL v9.x (via ESP-IDF component manager)  
- Python 3.8+  
- ESP32-S3 DevKitC-1 board

### 2. Clone & Configure
```
git clone https://github.com/suleymangunel/ePaperV2.git
cd ePaperV2
idf.py set-target esp32s3
```

### 3. Build & Flash
```
idf.py build
idf.py flash monitor
```


## 🧠 Technical Notes
The rgb565_to_mono() function performs on-the-fly rotation (90° clockwise) and X-mirroring during LVGL buffer conversion.
All pixel data is sent as 1-bit per pixel (monochrome) using MSB-first alignment.
Display update sequence (epd_display_frame()) triggers a full hardware refresh and waits for the busy signal.

🖋️ Font Conversion Command
You can regenerate the included font using:

lv_font_conv --font Inkfree.ttf --size 20 --bpp 4 --format lvgl --no-compress \
  --range 0x20-0x7F,0xC7,0xE7,0x286,0x287,0x304,0x305,0x214,0x246,0x350,0x351,0x220,0x252 \
  -o ink_free_12.c


## 📄 License
```
This project is licensed under the MIT License.
See the LICENSE file for details.
```
## 📚 References
```
LVGL Documentation,
Waveshare 1.54" V2 E-Paper Datasheet (SSD1680)
Waveshare 2.13" V4 E-Paper Datasheet (SSD1680)
Espressif ESP-IDF
```


