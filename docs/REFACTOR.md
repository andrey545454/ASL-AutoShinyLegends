# 1.x -> 2.0.0 refactor notes

The 1.x gate was intentionally not rewritten into one generation-agnostic timing engine. Gen I and Crystal depend on different emulator/guest assumptions, so 2.0.0 keeps the timing-critical implementations isolated and shares only lifecycle/UI/input.

## Old -> new files

```text
asl_gate.c       -> asl_gate.c router + asl_gen1.c + asl_gen2.c
asl_game.c       -> asl_gen1_game.c
asl_predictor.c  -> asl_gen1_predictor.c
                   + asl_gen2_predictor.c
```

New Crystal-only low-level files:

```text
asl_rom_probe.c
asl_guest_patch.c
asl_gen2_defs.c
```

The runtime framebuffer/HID code remains common.

## Gen I preservation

The Gen I backend retains the 1.x fixed host addresses, Red/Blue/Yellow layouts, bird timing adjustments, F0/E0 hook strategy, SCX VBlank marker and post-release verification path. Splitting it into `asl_gen1.c` is organizational; the intended game behavior is unchanged.

## Crystal calibration lock

The byte sequence in `k_celebi_vblank_gate` is timing-critical. The 2.0.0 tree keeps it byte-identical to the tested v0.7.2 calibration that produced:

```text
PRED: 7A AA
REAL: 7A AA
VERIFIED
```

Changes to guest instructions should be treated as timing changes even when they appear logically equivalent.
