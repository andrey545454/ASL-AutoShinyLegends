# ASL 2.0.0 architecture

ASL 2.0.0 is one raw 3GX with a common runtime layer and two independent game backends.

```text
                   default.3gx
                        |
              framebuffer / HID hooks
                        |
                  asl_gate router
                  /             \
          Gen I backend       Gen II backend
          R/B/Y host hooks    Crystal guest gate
```

## Backend selection

At startup ASL first validates the known Gen I `F0`/`E0` ARM call sites. On the supported Red/Blue/Yellow VC emulator these are valid ARM `BL` instructions, so ASL installs the original Gen I backend.

Crystal's GBC VC emulator does not match those host sites. In that case ASL leaves them untouched and incrementally scans readable process memory for the verified Crystal Rev 1 ROM shadow. A writable `PM_CRYSTAL` mapping with the expected `LoadEnemyMon.GenerateDVs` signature selects the Gen II backend.

This separation is intentional: the Crystal implementation does not alter the validated Gen I timing path.

## Gen I path

The Gen I backend is split out of the original monolithic gate but retains the same mechanism:

- host `F0` read hook observes `LDH A,(a8)`;
- host `E0` write hook provides a validated SCX/VBlank marker and guest reset detection;
- the exact final `DelayFrame` is selected by guest PC + stack return address + ROM bank;
- DIV phase and `hRandomAdd` predict the two future DV RNG bytes;
- a rejected candidate changes only the observed `hVBlankOccurred` read result from 0 to 1;
- the game's existing `DelayFrame` performs another real HALT/VBlank;
- a shiny-compatible prediction passes the real 0 through;
- after release the generated DVs are read from WRAM and compared to the prediction.

## Crystal path

Crystal uses different host-side emulator internals. Instead of depending on unverified GBC VC helper addresses, the Gen II backend modifies only the writable guest-ROM shadow after strict signature checks.

### Patched site

Crystal Rev 1 `LoadEnemyMon.GenerateDVs` contains:

```text
call BattleRandom
ld b,a
call BattleRandom
ld c,a
```

ASL redirects only the first `CALL` into an injected ROM0 gate. The second call remains stock.

### Rejected candidate

For the special Celebi battle the gate executes the game's stock `DelayFrame`, waits for the `rDIV` edge used by the calibrated model, reads `hRandomAdd` / `hRandomSub`, predicts the two future DV bytes, and loops if they are not shiny.

A rejected candidate therefore performs a real VBlank and does **not** call `BattleRandom`.

### Accepted candidate

When the predicted pair is shiny, the gate tail-jumps to the original `BattleRandom`. Its normal `RET` returns to `LoadEnemyMon`, which stores the first result in B and executes the untouched second `BattleRandom` for C.

No shiny values are written into `wEnemyMonDVs` and no RNG state is modified by ASL.

### Calibrated divider model

For the tested English Crystal Rev 1 VC path, the four `rDIV` samples used by the two future `Random` calls are modeled relative to the detected edge as:

```text
base+3, base+3, base+3, base+5
```

The final `base+5` was established from repeatable `PRED ?A AA -> REAL ?A A9` results and then confirmed by a successful `PRED 7A AA / REAL 7A AA / VERIFIED` encounter.

Do not change instruction timing in the accepted path casually. The telemetry store is cycle-compensated as part of the calibrated gate.

### Telemetry

Crystal uses `$D2A9` (`wOTPartyMon1Unused`) only to transfer the predicted Attack/Defense byte to the host HUD. It does not participate in Celebi RNG or enemy DVs. The host clears it before a run.

## Fail-open principles

- Gen I validates host ARM call sites before patching.
- Gen II validates the ROM header, writable mapping, exact GenerateDVs bytes and zero cave before patching.
- Abort/disable releases an active Gen II gate through a control byte before executable guest bytes are restored.
- Prediction failure on Gen I releases rather than trapping `DelayFrame`.
