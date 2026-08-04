# Changelog

## v1.2
- Replaced long constant-velocity runs with short, bounded random walks.
- Randomized both the movement timing and step size without modulo bias.
- Returns the cursor to its starting position after each movement burst.
- Added an [OK] button control to pause and resume motion.
- Reduced unnecessary USB HID reports.

## v1.1
- Fixed crash when attempting to switch to HID mode fails.
- When the application is launched, an initial loading message is now shown.
- When switching to HID mode fails, a failure message is now shown.

## v1.0
- Initial release of Mouse Jiggler for Flipper Zero.
