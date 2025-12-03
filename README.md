# Pocket Sampler

## How to build
For For RP2040 Zero

```
west build -S cdc-acm-console -b rp2040_zero  app/
west flash
```

## Access shell
using minicom
```bash
minicom -s
```
and set serial port to
```
/dev/tty.usbmodem11101
```