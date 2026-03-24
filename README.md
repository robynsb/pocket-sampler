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
minicom -D /dev/tty.usbmodem11101
```

using screen
```bash
screen /dev/tty.usbmodem11101
```

# Preloaded filesystem

Following Mike Szczys' [blog](https://blog.golioth.io/how-to-flash-a-pre-loaded-filesystem-during-production/).


Get filesystem with:
```bash
picotool save -r 0x10100000 0x101FFFFF filesystem.bin
littlefs-python list filesystem.bin --block-size 4096
```

Create new filesystem with
```bash
littlefs-python create preloaded-samples-fs/ preloaded-fs.bin --block-size 4096 --block-count 256
```
to produce binary file containing all your samples.

```bash
littlefs-python list preloaded-fs.bin --block-size 4096
```
to see inspect your binary.

Load binary onto device:
```bash
picotool load -o 0x10100000 preloaded-fs.bin -v
```
