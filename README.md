# Mecha Comet Keyboard Extension - ZMK Firmware

[ZMK](https://zmk.dev) firmware for the **Mecha Comet Keyboard Extension**, the keyboard of the
Mecha Comet. It attaches to the Comet with magnetic pogo pins and also works on its own over
USB-C. It comes in QWERTY, QWERTZ and AZERTY layouts, with and without ZMK Studio, and has a
Mecha Pivot Pip pointing stick.

Full documentation is coming.

## Building

The firmware is built in a [west](https://docs.zephyrproject.org/latest/develop/west/index.html)
workspace. This repository is its manifest: `west update` fetches ZMK, Zephyr and the other modules
next to it, and `west patch apply` applies the patches listed in
[`zephyr/patches.yml`](zephyr/patches.yml).

You need the host tools of the
[Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)
and the Zephyr SDK 0.17.0 with the `arm-zephyr-eabi` toolchain. As in that guide, the Python
packages go into a [virtual environment](https://docs.python.org/3/library/venv.html), so they stay
apart from your system Python. Set up the workspace once:

```sh
mkdir mecha-comet-keyboard-ws
cd mecha-comet-keyboard-ws
python3 -m venv .venv
source .venv/bin/activate
pip install west
west init -m https://github.com/mecha-org/mecha-comet-keyboard-fw
west update
west packages pip --install
west patch apply
west zephyr-export
```

In every new terminal, activate the environment again with `source .venv/bin/activate` in the
workspace folder. Build from the workspace folder. The result is `build/zephyr/zmk.bin`:

```sh
west build -p -s zmk/app -d build -b mecha_comet_keyboard//zmk -- \
  -DZMK_CONFIG="$PWD/mecha-comet-keyboard-fw/config"
```

- For ZMK Studio, add `-S studio-rpc-usb-uart` before the `--` and `-DCONFIG_ZMK_STUDIO=y` after it.
- For another layout, add
  `-DKEYMAP_FILE="$PWD/mecha-comet-keyboard-fw/config/mecha_comet_keyboard_azerty.keymap"`, or
  `mecha_comet_keyboard_qwertz.keymap`.

[`build.yaml`](build.yaml) lists the six images that CI builds, and [`config`](config) has the
keymaps and the options. After `west update`, run `west patch clean` and `west patch apply` again.

### Local development

To change Pivot Pip together with the keyboard, work in `modules/lib/mecha-pivot-pip`. It is a
normal git checkout, and a build uses what is in it, committed or not. Use a branch there, and run
`west update -k` so that west keeps it instead of moving back to the revision of `west.yml`. To use
a clone somewhere else, add `-DZMK_EXTRA_MODULES=/path/to/mecha-pivot-pip-zephyr` to the build. It
replaces the copy in the workspace.

Licensed under the [BSD 3-Clause license](LICENSE).
