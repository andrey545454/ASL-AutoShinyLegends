# Architecture

This document describes the parts of ASL-AutoShinyLegends that are easy to break when changing emulator addresses, hook code or battle timing.

## Runtime flow

1. `bootloader.s` preserves the title thread registers and CPSR, clears the plugin BSS and calls `main()`.
2. `main.c` locates the title code mapping.
3. `asl_runtime_hooks.c` installs the framebuffer-present and HID shared-memory hooks.
4. `asl_gate.c` validates and patches the two emulator `LDH` call sites.
5. ROM detection happens lazily from the active emulator callbacks because raw 3GX startup can occur before the Game Boy ROM header is initialized.
6. The framebuffer-present callback acts as the lightweight per-frame scheduler: it scans input, advances UI-side gate state, snapshots the gate and renders the overlay.

## Why the gate uses branch islands

The F0/E0 hook sites are near `0x001Axxxx`, while the raw 3GX image is mapped near `0x07000000`. ARM `B`/`BL` has a range of roughly ±32 MiB, so the hook site cannot branch directly to the plugin.

ASL searches the title executable mapping for a sufficiently large zero-filled padding region and places two small absolute-jump islands there:

```text
original call site -> ARM B -> nearby island
nearby island      -> ldr pc, [pc, #-4]
                      .word plugin_bridge
```

The bridge then reproduces the original `BL` behavior by calling the original emulator helper and returning to the instruction immediately after the patched call site.

The cave search intentionally requires a 32-byte zero run even though the two islands use only 16 bytes. This is a conservative sanity measure to reduce the chance of treating an isolated literal/data area as padding.

## F0 gate

The F0 helper implements Game Boy `LDH A,(a8)`. That helper is used in many places, so ASL validates three independent properties before treating a read as the shiny gate origin:

- guest PC equals the immediate-byte PC of the final `LDH A,(hVBlankOccurred)`;
- the Game Boy return address on the guest stack matches the expected `DelayFrame` caller;
- the currently loaded ROM bank matches `PlayBattleMusic`.

Only a real `0` result is eligible for release/hold logic. Non-zero values already keep the game's original `DelayFrame` looping and are passed through unchanged.

When a candidate is not shiny-compatible, ASL returns `1` for that single read. It does not write `hVBlankOccurred` in guest memory.

## Real-VBlank tracking

While ASL is holding the final `DelayFrame`, other interrupts can wake the Game Boy HALT instruction. Because the guest `hVBlankOccurred` byte is not modified, such a wake can reach the F0 hook again before a real VBlank happened.

The E0 helper (`LDH (a8),A`) is used to count writes to `SCX`. On this target build, the existing project validated one relevant SCX write per VBlank. ASL therefore advances its VBlank marker on those writes and refuses to evaluate another candidate until that marker changes.

## Prediction model

The VC emulator exposes:

- the Game Boy `DIV` byte;
- a 1..64 countdown to its next increment;
- the emulator's current M-cycle offset;
- `hRandomAdd`.

`asl_predictor.c` reconstructs a 14-bit divider phase, advances it by the measured M-cycle distances to the two upcoming `BattleRandom` calls, then applies the validated Gen I RNG relation:

```text
next hRandomAdd = previous hRandomAdd + rDIV + 1
```

The two resulting bytes are interpreted as:

```text
dv1 = Speed / Special
dv2 = Attack / Defense
```

A Gen I DV pair is shiny-compatible in Gen II when Speed, Special and Defense are 10 and Attack is one of `2, 3, 6, 7, 10, 11, 14, 15`.

## Post-release verification

ASL does not trust prediction silently. After a shiny candidate is released, it waits for the next validated VBlank. There is no VBlank between the gate origin and the normal enemy DV generation on the supported path, so by then `wEnemyMonDVs` has been populated.

The generated two bytes are read from WRAM and compared with the prediction. The overlay reports `VERIFIED` or `MISMATCH`.

## Reset detection

### In-game soft reset

`A+B+START+SELECT` is detected from held 3DS key state. Because `SELECT` also toggles ASL, a standalone SELECT press is committed only on release. This prevents a staggered reset chord from changing the enable state.

### Virtual Console menu Reset

The VC menu can restart the emulated Game Boy without reloading the `.3gx`, so plugin globals survive the reset.

Red, Blue and Yellow begin their guest `Init` routine with the same ordered `LDH (a8),A` write sequence:

```text
rIF -> rIE -> rSCX -> rSCY -> rSB -> rSC -> rWX -> rWY
```

The E0 hook observes those writes. ASL resets the current run state only after the complete sequence matches. The user's ON/OFF choice is preserved.

## Failure behavior

The gate is designed to fail open:

- unexpected F0/E0 instructions: do not install the gate hooks;
- no suitable branch-island cave: do not install the gate hooks;
- invalid divider sample: release the game instead of holding forever;
- unsupported encounter species: pass the normal result through;
- disabled plugin: F0 is strict pass-through and no prediction is performed.

This is intentional. A missed shiny-search opportunity is preferable to trapping or corrupting the guest game.
