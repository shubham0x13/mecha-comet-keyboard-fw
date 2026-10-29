# Physical Layout

`kle-layout.json` is the key layout of the keyboard, exported with Download JSON from
[keyboard-layout-editor.com](https://www.keyboard-layout-editor.com). Nothing in the build reads it.
It is the file that [keymap-layout-tools](https://nickcoutsos.github.io/keymap-layout-tools/)
imports.

The number on each key is its position, from 0 to 45, the one that the keymap bindings and combos
use. The same layout is in [`config/info.json`](../config/info.json) for the keymap editor, and in
the [board devicetree](../boards/mecha/mecha_comet_keyboard/mecha_comet_keyboard-layouts.dtsi) for
ZMK and ZMK Studio. When the layout changes, change all three.
