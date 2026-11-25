# Pager firmware for the Elecrow CrowPanel 2.13" e-ink board

Shows iPhone notifications on the e-ink screen over Bluetooth (Apple ANCS).

## Flash
Write `pager-crowpanel213.bin` at address `0x0` (ESP32-S3, 8MB flash).

## Build
PlatformIO: `pio run`. The merged image is made with `esptool.py merge_bin`
(bootloader 0x0, partitions 0x8000, boot_app0 0xe000, firmware 0x10000).

## Controls
- Home (clock): rocker up or press opens notifications, rocker down opens Now Playing, MENU opens the main menu, EXIT does a clean-up refresh
- Main menu: Notifications, Library, Now Playing, Tools, Settings, Sleep now
- More of everything (v1.7): Custom face with a chosen fact in each corner (`corners=` setting), Today screen fed by the `J` (countdown, reminder, ticker) and `Q` (line for the day) commands, Find my phone (`B` event to the app), Pomodoro, tally, tip split, converter, Morse, Hangman, Minesweeper, and flashcard decks (files ending `.cards`, one `question | answer` per line).
- Faces and games (v1.6): six home screen faces (Settings > Home face, or the app), a photo face sent as `face.img` (250 x 122, 1 bit, 1 = black), steps line, Snake and 2048 under Tools, hold the wheel to keep scrolling.
- App link (v1.5): a private Bluetooth service (1F0B0001-8A4E-4C1B-9E2D-5B7A1C3D9F10) for the
  Inkfob iPhone app. Control characteristic ...0002 takes one command per write (first byte):
  `I` home lines (weather\nevent\nevent), `S` key=value setting, `G` get settings, `L` list files,
  `N` + size(u32 LE) + name starts a file, `F` + size(u32 LE) + md5 hex starts a firmware update,
  `D` name deletes, `C` cancels. Bytes then go to ...0004; replies arrive as notifications on ...0003:
  `A` ready, `P` + bytes received, `K` done, `X` + reason, `g`/`h` settings, `l`/`m` file list,
  `R` "please send fresh weather and calendar". Files live in LittleFS on the 1.5 MB data partition
  and show under Main menu > My files. Firmware updates use the second app slot; after one, a cable
  flash at 0x10000 may not be the slot that boots, so use the full image at 0x0 for cable recovery.
- Tools (v1.1): Timer, Stopwatch, Camera remote (sends the Volume Up media key
  over the existing HID service), My card (QR code; edit `src/personal.h`),
  Oracle, Dice, Coin flip. Settings > "Sleep screen" picks Status or My card.
  A running timer or stopwatch holds off automatic sleep.
- Sleep (v1.0): Settings > "Sleep after" (Never, 1, 5, 15, 30 min, 1 hour) puts
  the chip into deep sleep after that long with no button press. Any button
  wakes it (it restarts, about 2-3 seconds). Settings > "Asleep, check phone"
  (Off, 5, 15, 30 min, 1 hour) makes it wake by itself, reconnect, and only
  light the screen up if a notification arrived since it went to sleep.
  The power-on shortcuts (hold MENU / EXIT) are ignored on a wake from sleep.
- Notifications: rocker moves, press opens, EXIT goes home
- Message: rocker scrolls, press or EXIT goes back, MENU dismisses it on the phone
- Now Playing: press is play/pause, rocker is volume (or skip tracks after MENU), EXIT goes home
- Library: rocker moves, press opens, EXIT goes up a level
- Reader: rocker turns pages (and runs on into the next entry), press or EXIT goes back to the list, MENU goes home. The place is saved as "Continue".
- Drawings: each has a page of its own. A tall one scrolls with the rocker before the page turns. Press zooms in (header shows "+" when a larger version exists); zoomed in, the rocker scrolls, MENU moves sideways, and press or EXIT zooms back out.
- Hold MENU while powering on: switch to the other screen type
- Hold EXIT while powering on: forget the paired phone

## Library content
`data/library.bin` is built by `tools/build_library.py` and embedded in the firmware by `src/library_blob.S`.

    git clone --depth 1 https://github.com/ligi/SurvivalManual.wiki.git ligi-wiki
    git clone --depth 1 https://github.com/everyday3d/esp32-cyd-survival-wiki cyd
    python3 tools/build_library.py ligi-wiki cyd/sdcard/wiki data/library.bin
    python3 tools/check_library.py data/library.bin show "Debris Hut"

Sources: the chaptered US Army Survival Manual text (FM 3-05.70 / FM 21-76, public domain) curated by the ligi SurvivalManual project, and 118 field guides from the esp32-cyd-survival-wiki project by Everyday3D (MIT licence). All 188 drawings from both sources are included as 1-bit bitmaps in two sizes.

The build needs Pillow (`pip install pillow`). `tools/pio_depends.py` makes PlatformIO rebuild the embedded copy whenever `data/library.bin` changes.

## Updating
Flash `firmware.bin` at `0x10000` to keep settings and pairing. The merged
image at `0x0` wipes them.

## Notes
- The board ships with one of two e-ink panels (SSD1680 on V1.0 hardware, a
  different controller on V1.2). The firmware detects which at boot.
- Serial log at 115200 baud on the USB-C port.
