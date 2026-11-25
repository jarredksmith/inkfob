# Inkfob

A keychain-size e-ink pager for the iPhone, and the iPhone app that goes with it.

- `firmware/` - the pager firmware for the Elecrow CrowPanel 2.13" ESP32-S3 e-ink board
  (PlatformIO). It shows iPhone notifications, a clock, now-playing, a built-in survival
  library and a set of small tools. See `firmware/README.md`.
- `ios/` - the Inkfob iPhone app (SwiftUI). It joins the pager's Bluetooth connection to send
  weather and calendar lines for the home screen, change pager settings, send notes and books,
  and install firmware updates.
- `case/` - CadQuery scripts for the 3D-printed case.

## Building the app

Pushing to `main` runs `.github/workflows/ios.yml` on a GitHub Mac. With the three App Store
Connect secrets set (`ASC_KEY_ID`, `ASC_ISSUER_ID`, `ASC_KEY_P8`) it uploads to TestFlight;
without them it only checks that the app compiles.

## Firmware updates from the app

The app's Update tab installs `firmware/release/pager-firmware.bin` from the `main` branch and
reads the version from `firmware/release/version.txt`. Commit a new build and version there to
make an update available.
