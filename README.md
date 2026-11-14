# P4Box for Arduino IDE

Multi-app firmware for the **Waveshare ESP32-P4-86-Panel-ETH-2RO**: a 4" 720×720 touch panel with Ethernet, Wi-Fi (through the ESP32-C6), two relays, a speaker and a microSD slot.

Everything is in this sketch folder. LVGL 9.3 and the drivers are bundled in `src/`, so you **don't need to install any libraries**. You only need the ESP32 board package.

## Apps

| App | What it does |
|---|---|
| **Clock** | Digital or analog face, 12/24 h, seconds on/off. Automatic DST, every Canadian zone plus major world cities, and NTP time. **Alarms** with snooze: wake to the radio, beeps or a buzzer. |
| **Weather** | Current conditions and forecasts from OpenWeather for up to 8 places, in metric or imperial. Shows temperature, feels-like, humidex and wind chill, precipitation, humidity, dew point, wind and gusts, UV index, air quality (AQHI, US AQI or OpenWeather 1-5), pressure, visibility, sunrise and sunset, the moon and weather alerts. The current weather also shows on the home screen. |
| **Radio** | Internet radio on the panel speaker. Has 12 built-in stations, Radio Browser search, and add/edit/delete from the panel or the web page. Can resume after a power cut. |
| **Lock** | Relay 1 drives an electric strike (latched or momentary pulse); relay 2 is an on/off toggle. User codes are 4–6 digits (the default user code is 1234) and the admin code is 6 digits. Has a lockout after wrong codes and an event log. Relay states come back after a power loss. |
| **Calendar** | Month view with Canadian holidays for your province, plus federal statutory holidays and observances. |
| **Speed Test** | Cloudflare ping, jitter, download and upload over whichever connection is active (Ethernet or Wi-Fi). Keeps a history. |
| **Photos** | JPEG viewer for the microSD card: a thumbnail grid with folders, full-screen viewing, swipe for next/previous, and a slideshow. It can also act as a **photo-frame screensaver**. |
| **Settings** | Wi-Fi (scan + on-screen keyboard, or the setup hotspot), display, screensaver, night dimming, time zone, system info, Wi-Fi module update, restart and factory reset. |

Other behaviour:
- Ethernet is used automatically when a cable is plugged in; otherwise the panel uses Wi-Fi.
- A setup hotspot named **P4Box-Setup-XXXX** (a captive portal) starts if no network comes up within 30 seconds.
- A web page at **http://p4box.local** controls the radio, stations, lock codes, photos and all settings. It is protected by the admin code once you set one.
- The screensaver (clock or photo frame) starts after a timeout, and the screen dims at night.

## 1. Install the board package

1. Use Arduino IDE 2.x.
2. Open **File > Preferences > Additional boards manager URLs** and add:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. Open **Tools > Board > Boards Manager**, search **esp32 by Espressif Systems**, and install **3.3.12** or newer.
   - 3.3.4 to 3.3.11 also compile, but they lack Espressif's fast JPEG decoder, so photos open more slowly.

## 2. Tools menu settings

| Setting | Value |
|---|---|
| Board | **ESP32P4 Dev Module** |
| Chip Variant | **match your chip**: "Before v3.00" for revision v1.x chips, "v3.00 or newer" for v3.x (see below) |
| Flash Size | **32MB (256Mb)** |
| Partition Scheme | **32M Flash (13MB APP/6.75MB SPIFFS)** |
| PSRAM | **Enabled** |
| Upload Speed | 921600 |
| USB Mode / CDC On Boot | leave the defaults (USB CDC On Boot must stay **Disabled**: the OTG port is used for USB audio) |

**Which chip variant?** When you upload, the log prints a line like `Chip is ESP32-P4 (revision v1.3)` or `(revision v3.1)`. Pick the matching variant and upload again if needed. The wrong choice either fails at boot or won't upload.

## 3. Build and upload

1. Copy the `P4Box` folder into your sketchbook (for example `Documents/Arduino/P4Box`).
2. Open `P4Box.ino`.
3. Connect the panel's **USB-UART** Type-C port and press **Upload**.
   - If the upload doesn't start, hold **BOOT**, tap **RESET**, then release BOOT.
4. Open Serial Monitor at 115200 to see the boot log.

The **first** compile takes several minutes because LVGL is about 300 source files. Later builds are quick.

> **Pre-built binaries.** `prebuilt/` in the zip holds ready-to-flash images. Write `P4Box_v3_full.bin` (v3.x chips) or `P4Box_v1_full.bin` (v1.x chips) at address **0x0** (see prebuilt/FLASHING.txt) with esptool or the Espressif Flash Download Tool, for example `esptool --chip esp32p4 write_flash 0x0 P4Box_v1_full.bin`. Use the `*_app.bin` files for web-page updates.

## First start

1. The panel starts in Ethernet mode if a cable is connected; otherwise open **Settings > Network** and pick your Wi-Fi.
2. If nothing connects within 30 seconds, join the **P4Box-Setup-XXXX** hotspot from a phone and the settings page opens by itself.
3. Open **Lock** once to create the 6-digit **admin code**. The web page asks for it from then on.
4. Browse to **http://p4box.local**, or to the IP shown in Settings.

## Photos

- **Card:** a microSD card formatted **FAT32**. exFAT isn't supported, so cards over 32 GB must be reformatted to FAT32 (on Windows use a tool such as *guiformat*).
- **Adding photos:** copy `.jpg`/`.jpeg` files anywhere on the card, in any folders. You can also upload them from the **Photos** section of the web page, which also lets you create folders, delete files and view them.
- **Supported files:** baseline JPEG up to 20 MB, which covers phone and camera photos. **Progressive JPEGs are not supported**; re-save them as "baseline" if one shows an error. The EXIF orientation is honoured.
- **Browsing:** the grid shows 16 tiles per page. Tap a folder to open it, ↑ goes up, and you can swipe or use the arrows to change page.
  - Thumbnails come from the preview image that cameras embed, and are cached in a hidden `/.p4thumbs` folder on the card.
- **Viewer:** swipe left/right for next/previous and swipe down (or ✕) to close. Tap to show or hide the controls: ◀ ▶ ⏯ and **Fill/Fit** (crop to fill the screen, or show the whole photo).
- **Options (⚙):** slideshow interval, shuffle, fit/fill, include sub-folders, "use this folder as photo frame", clear the thumbnail cache, re-scan the card.
- **Photo frame:** set **Settings > Display > Screensaver shows: Photo frame**. When the screensaver starts, the panel shows a slideshow of the chosen folder with the time and date in the corner, and still dims at night. Tap to wake. If no card or photos are found it falls back to the clock.

## Weather

The Weather app uses your own **OpenWeather API key**. The app shows QR codes for these pages:
1. Create a free account at <https://home.openweathermap.org/users/sign_up>.
2. Copy your key from <https://home.openweathermap.org/api_keys>. New keys can take up to 2 hours before OpenWeather accepts them, and until then the panel says "API key not accepted".
3. Enter the key, either:
   - on the panel: Weather > **Enter API key**, or ⚙ > Change; or
   - on the web page, in the **Weather** section (easier, because you can paste it).
4. Add a place: search by town ("Wasaga Beach, ON") or by postal / ZIP code (L9Z 1A1 or 90210). Canadian postal codes are matched on their first three characters. You can save up to 8 places; switch between them with the arrows or by tapping the place name.

**What a free key gives:** current weather, a 5-day forecast in 3-hour steps, air quality and place search. These are all in OpenWeather's free plan, which needs no payment card.

**What One Call adds:** the UV index, a true hourly forecast, an 8 to 10-day forecast, the moon phase and official weather alerts (for example from Environment Canada). One Call is a separate subscription on the same key, and the first 1,000 calls a day are free (<https://openweathermap.org/price#one_call>). OpenWeather may ask for a payment card. In its billing settings you can cap the daily calls at 1,000 so it never charges.

The panel detects what your key can use by itself. It tries **One Call 4.0** first, then **3.0**, and otherwise uses the free APIs, checking again every 6 hours. The footer of the Weather app shows which source is in use.

**Calls used:** the place on screen is refreshed every 15 minutes, which is about 300 calls a day: with One Call, about 290 One Call calls plus 190 free-plan calls. The ⟳ button refreshes right away.

**Display:**
- **Units:** metric shows °C, km/h, kPa, mm and km; imperial shows °F, mph, inHg, inches and miles.
- **Feels like:**
  - In metric, humidex is used in summer (from 20 °C and humidex 25) and wind chill in winter (0 °C or colder, with wind of 5 km/h or more), as Environment Canada reports them.
  - In imperial, the US heat index is used in place of humidex.
- **Air quality:** choose between the Canadian **AQHI** (1-10+), the **US AQI** (0-500) or OpenWeather's own 1-5 index. The AQHI and the US AQI are calculated on the panel from OpenWeather's pollutant readings, so they can differ slightly from the official stations.
- **Home screen:** the current temperature and conditions show under the date; tap them to open Weather. You can turn this off in Weather ⚙.

## Alarms

Open **Clock** and tap the alarm button at the bottom right. It shows the next alarm time, or "Alarms" when none is set.

- **Up to 6 alarms.** Each one has a time, repeat days (or **Once**, which switches itself off after ringing), a sound and an optional label. Tap an alarm to edit it, use its switch to turn it on or off, and use **Test** in the editor to hear it right away.
- **Sounds:**
  - **Radio** plays the last station you listened to, including stations played from search results.
  - **Beeps** is a classic beep-beep-beep-beep.
  - **Buzzer** is an old clock-radio style buzz.
  - If the radio stream hasn't started within 25 seconds, or drops out for 20 seconds, the alarm switches to beeps, so it always makes a sound.
- **Ringing screen:** a large **Snooze** button (1-10 minutes, set in the options), **Stop**, and for radio alarms **Keep listening**, which stops the alarm but leaves the music playing. The screen wakes from the screensaver and night dimming at full brightness.
- **Options:** snooze length, alarm volume (separate from the radio volume), gentle wake (the volume rises over the first 30 seconds), and stop ringing after 5-60 minutes.
- **Alarm icon:**
  - A bell in the status bar shows when an alarm is set: grey when armed, amber with a "z" while snoozing.
  - The home screen and the screensaver show the next alarm time under the date.
- **Web page:** the **Alarms** section edits the same alarms. While an alarm rings, it shows **Snooze** and **Stop** buttons.
- Alarms ring once the clock has its time, from the network or from the battery clock (below). They use the time zone and daylight-saving settings, so they stay right across the time change.

## Battery clock (RTC battery)

The panel has no separate clock chip. Its **RTC battery** header feeds the ESP32-P4's own backup-power pin (VBAT), which can keep the chip's low-power clock counter and a small block of memory alive while the main power is off. From 1.4.0 the firmware uses it so the clock, and your alarms, survive a power cut even with no network:

- At start-up it turns on the chip's automatic switch-over to the battery (**Settings > Time > Keep time on the RTC battery**, on by default).
- Every 30 seconds it writes "it was this time at counter value N" into that memory. When power comes back without the network, it works out the time from how far the counter moved.
- While online it measures the counter's real speed against network time, so later power cuts are counted more accurately. When the network comes back, network time takes over again.
- **Settings > Time > Battery clock** and the web page show what happened at the last start-up: *Restored after 2 h without power (off by +3 s)*, *Time carried over the last restart*, or *Last power cut: nothing was held*. It also shows how far off it was once the network time arrived.

Things to know:

- **Battery:** Waveshare says the header takes **rechargeable** cells only. Use a 3 V lithium rechargeable (ML1220, ML2032 or similar). Don't use a 3.6 V LIR cell (fully charged it is about 4.2 V, above the chip's 3.6 V limit) or a non-rechargeable CR cell.
- **Experimental:** Espressif marks VBAT on the ESP32-P4 as experimental and has only verified it in deep sleep. Whether it holds through a power cut while running is what the status line tells you. Test it once: let the clock sync, unplug power and network for a few minutes, then plug power back in (still no network).
- **Accuracy:** the counter runs from the chip's internal RC oscillator, not a crystal. Once tuned it is typically within a few seconds per hour, but temperature changes move it. After a long cut the time can be off by a minute or more until the network corrects it.
- **More accurate option:** a **DS3231** clock module (I2C address 0x68) on the panel's I2C bus (GPIO 7 SDA / GPIO 8 SCL, 3.3 V) is found automatically at start-up. It is read before anything else and set from network time at every sync. It keeps time to about a minute a year on its own coin cell.

## Bluetooth speakers and headphones (USB audio)

The panel's Wi-Fi chip (ESP32-C6) only has Bluetooth Low Energy, which can't carry music to speakers or headphones. Instead, the radio can play through a **USB Bluetooth audio transmitter**, a small dongle that looks like a USB sound card and does the Bluetooth pairing itself. The **Avantree C81** fits: Windows identifies it as a USB Audio 1.0 device. Other dongles that work plug-and-play on a Nintendo Switch or PS5 should also work, as should USB Audio 1.0 headphones and DACs.

- **Plug it into the USB-C OTG port**, not the USB-UART port, and pair it with your speaker or headphones as its manual describes (on the C81, hold its button). Keep the C81 in music mode: a double-click switches it to call mode, which is mono and low quality.
- **The radio switches to it automatically.** The ▶ row in Radio shows a Bluetooth button; tap it to choose the built-in speaker or the USB / Bluetooth device. The web page has the same switch under Radio, and the status bar shows a Bluetooth icon while audio goes to the dongle.
- **Full stereo:** stereo streams stay stereo, and mono streams play on both sides. If the dongle doesn't accept the stream's sample rate, the panel converts it (usually 44.1 to 48 kHz). The radio volume slider still works; you can also use the speaker's or headphones' own volume.
- **Unplugged or failing:** the radio carries on through the built-in speaker straight away, and goes back to the dongle when you start a station again with it plugged in.
- **Alarms always use the built-in speaker**, including wake-to-radio, so you are never woken through headphones on the nightstand. "Keep listening" on the alarm screen also stays on the speaker until you pick a station yourself.

**Power for the dongle:** according to Waveshare's schematic, the OTG port's 5 V line is an input (a plugged-in device can power the panel) and may not supply power to a dongle. So:
1. **Try it first.** Plug the dongle in. If its light comes on and the serial log shows `usb_audio: connected: ...`, nothing more is needed.
2. **If it stays dark,** the dongle needs 5 V from elsewhere. Use a *passive* USB-C OTG adapter or cable that injects power, not a USB hub. Espressif's USB host can't reach full-speed devices such as these dongles through a high-speed hub, so the dongle must be connected directly. The injected 5 V also feeds the panel through its input diode, which is harmless.

## Radio notes

- Plays **MP3** and **AAC/AAC+ (HE-AAC, ADTS)** streams over HTTP or HTTPS, including .pls/.m3u playlists, redirects and ICY titles.
- **Not supported:** OGG/Opus/FLAC and HLS (.m3u8) streams. The station search only lists stations the panel can play.

## Wi-Fi module (ESP32-C6) firmware

Wi-Fi runs on the C6 co-processor through ESP-Hosted. Boards often ship with an old C6 firmware that is too old to even report its version. Wi-Fi usually still works with it, so **only update if Wi-Fi scanning or connecting fails**.

There are two ways to update it:

- **From the panel:** Settings > System > **Update Wi-Fi module** downloads `esp32c6-v2.12.13.bin` from Espressif. If espressif.github.io fails, it tries raw.githubusercontent.com. Any error shows on the panel and in the serial log.
- **From the web page:** System > **Wi-Fi module (ESP32-C6) firmware**. Choose `prebuilt/esp32c6-v2.12.13.bin` from this zip and click **Update Wi-Fi module**. The panel restarts when it's done.

Writing takes about a minute, then the panel restarts. If it fails, the panel and the web page say at which step (start, write at byte N, or the final check), and the serial log has the same lines tagged `C6 OTA:`.

The file must match the board package: `v2.12.13` for esp32 core 3.3.12. If the C6 firmware is so old that it refuses an over-the-air update, the C6 can only be reflashed through its own UART pads with esptool.

## Memory use

Internal RAM is only about 300 KB, while the 32 MB PSRAM is mostly free, so the large or long-lived allocations go to PSRAM:

| What | Where |
|---|---|
| Radio stream buffer (512 KB) and decoder buffers | PSRAM |
| TLS / HTTPS sessions (radio, station search, speed test, updates) | PSRAM |
| All of LVGL's memory: widgets, styles, text, font glyph cache | PSRAM |
| Display frame buffers (2 × 1 MB) | PSRAM |
| Photo decoding buffers, thumbnails, file lists | PSRAM |
| Weather cache, downloaded forecasts, all JSON parsing (cJSON) | PSRAM |
| Stacks of the radio decoder, radio stream, photo decoder, SD and weather tasks | PSRAM |
| Startup task (24 KB) | freed after boot |

These stay in internal RAM:
- Tasks that write to the flash: the LVGL/UI task (it saves settings), the web server (OTA, settings) and the speed-test tasks.
- The Wi-Fi/Ethernet network stack buffers, which are fixed by the board package.

Settings > System shows the free memory and the lowest it has been since boot.

## Web page API (for automation)

- `GET /api/status`, `GET/POST /api/settings`, `GET/POST /api/radio`, `POST /api/stations`, `GET/POST /api/lock`
- `GET /api/photos?dir=`, `GET /api/photo?path=`, `POST /api/photos/upload?dir=&name=` (raw JPEG body), `POST /api/photos/delete`, `POST /api/photos/mkdir`
- `GET /api/weather` (the selected place, metric), `GET /api/weather/search?q=`; the key, units and places are part of `/api/settings` (`wx_key` write-only, `wx_units`, `wx_aq_scale`, `wx_home`, `wx_locs`, `wx_loc_sel`).
- `POST /api/alarm {"action":"stop"|"snooze"}`; alarms themselves are part of `/api/settings` (`alarms` array, `alarm_snooze_min`, `alarm_volume`, `alarm_fade`, `alarm_ring_min`). `GET /api/status` includes `alarm.ringing` and `alarm.next`.
- `GET /api/status` also includes `rtc.status` (the battery-clock lines) and `rtc.restored`; the switch is `rtc_vbat` in `/api/settings`.
- `POST /api/ota`: firmware update with **P4Box.ino.bin** (Sketch > Export Compiled Binary)

Once an admin code exists, every call except status and login needs the `X-Auth` token returned by `POST /api/login {"code":"123456"}`.

## Customising

- **Pins:** `src/app/esp/board.h` (relays, I2C, backlight, amplifier), `src/app/esp/radio.c` (I2S) and `src/app/esp/sdcard.cpp` (SD).
- **Built-in stations:** `src/app/core/stations.c`. Stations you add on the panel or web page are kept in flash.
- **Web page:** edit `src/app/web/index.html`, then run `python3 tools/embed_html.py` to regenerate `index_html.h`.
- **Icons and fonts:** `tools/gen_assets.py` regenerates `src/app/assets/assets_gen.c`. It needs Python, Pillow and the Poppins fonts.
- **LVGL settings:** `src/lv_kconfig_values.h`.

## Folder layout

```
P4Box/
  P4Box.ino                 setup() -> p4box_main()
  src/app/core/             portable logic: settings, time zones, holidays, lock, stations
  src/app/ui/               LVGL screens (launcher, clock + alarms, weather, radio, lock, calendar, speed, photos, settings)
  src/app/esp/              hardware + network: board, net, web server, radio, speed test, photos, SD
  src/app/web/              settings web page
  src/drivers/              ST7703 MIPI-DSI panel, GT911 touch, esp_lvgl_port, usb_host_uac (USB audio),
                            dsi_cache_safe (display driver that keeps refreshing while settings are saved)
  src/audio/helix_aac/      HE-AAC decoder (MP3 uses the decoder built into the core)
  src/jpeg/                 TJpgDec (fallback JPEG decoder)
  src/lvgl/                 LVGL 9.3
  tools/                    asset and web-page generators
```

## Known limits

- RS485 (GPIO 47/48) is left free for later.
- USB audio has been checked off-device (driver build, rate conversion) but not yet with a dongle on the panel.
- The microphone is unused.
- The battery clock depends on the experimental VBAT support described above. The DS3231 path has been checked against the datasheet but not with a module.

## Licences

- LVGL: MIT.
- esp_lvgl_port, esp_lcd_touch, GT911 and ST7703 drivers: Apache-2.0 (Espressif / Waveshare).
- Helix AAC decoder: RealNetworks RPSL/RCSL.
- usb_host_uac USB audio driver: Apache-2.0 (Espressif).
- MIPI-DSI DPI panel and DW-GDMA drivers (src/drivers/dsi_cache_safe, copies of ESP-IDF v5.5.5): Apache-2.0 (Espressif).
- TJpgDec: © ChaN, free use.
- Poppins font: SIL OFL 1.1.
- P4Box code: free to use and modify.
