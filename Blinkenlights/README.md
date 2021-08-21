# Working with firmware

## Compiling with `arduino-cli`

First make sure you have everything setup.

```
$ ESP32_BM_INDEX="https://dl.espressif.com/dl/package_esp32_index.json"
$ arduino-cli --additional-urls=${ESP32_BM_INDEX} core install esp32:esp32
$ arduino-cli lib install FastLed
```

The `ESP32` core package comes with a lot of board definitions, among which is
`esp32:esp32:esp32` a.k.a. `ESP32 Dev Module` and that's what we're going to
use.

```
$ arduino-cli compile -b esp32:esp32:esp32 --output-dir=./ ./Blinkenlights.ino
```

This should produce multiple files, out of which only `Blinkenlights.ino.bin` is
relevant most of the time.

> NOTE: On every firmware change, please copy the compiled binary to:
> `Blinkenlights.ino.esp32.bin` 

## Uploading

The board is represented as a serial device on your computer. On Linux, that's
likely going to be `/dev/ttyUSB0` or `/dev/ttyUSB0` (probably the only one with
`USB` in the name).

> NOTE: In order to upload the resulting binary to the board you'll need to be
> able to write to the `/dev/tty*` device that your board is behind. You can do
> this either as `root` (e.g. with `sudo`) or by adding yourself to a group
> with write access. That's likely `dialout` (Debian, Ubuntu) or `uucp` (Arch).

### With `arduino-cli`

```
$ arduino-cli upload -b esp32:esp32:esp32 -p /dev/ttyUSB0 -i Blinkenlights.ino.esp32.bin --verify
```

### With `esptool`

```
$ esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 115200 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m --flash_size detect 0x10000 Blinkenlights.ino.esp32.bin
```
