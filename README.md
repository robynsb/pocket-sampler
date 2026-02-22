# Pocket Sampler

## Dependencies

Follow the [Zephyr getting started guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).

## How to build
For RP2040 Zero

```bash
west init -m git@github.com:robynsb/pocket-sampler.git pocketsampler
cd pocketsampler
west update
west build -S cdc-acm-console -b rp2040_zero app/
west flash
```

For Native Sim

```bash
./nsd.sh .
cd /app
west build -b native_sim/native/64 app -- -DEXTRA_CONF_FILE=debug.conf
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

using screen
```bash
screen /dev/tty.usbmodem11101
```