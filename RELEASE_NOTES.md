# Release notes

## v0.1.2 - Installation-documentation correction

This documentation-only follow-up makes the installation example point to the
immutable release that contains it. Package metadata advances to `0.1.2`; all
files under `include/` remain byte-identical to `v0.1.0` and `v0.1.1`.

## v0.1.1 - MIT and metadata follow-up

This behavior-neutral follow-up releases the library under the MIT License,
records the canonical repository and maintainer, declares the public headers,
and excludes repository-only test scripts from the PlatformIO archive. The
implementation under `include/` is unchanged from `v0.1.0`; the existing
`v0.1.0` tag remains immutable.

## v0.1.0 - Stage C compatibility release

`v0.1.0` is the first hardware-accepted release of the neutral
`ModbusRTUStoreForwardBridge` core.

### Exact tested implementation

- Module source: `d15d9474be43205c4149a69c345d07c92cc2d098`
- OGM bridge consumer: `59311ac8027cd3a8f28ae40d6253a285c4b62224`
- Physical acceptance date: 2026-08-25
- Compatibility target: unchanged deployed OGM slave firmware

The release/tag commit may be newer than the module source above because this
closeout adds documentation only. The commit above remains the exact
behavior-bearing implementation exercised on hardware.

### Acceptance evidence

- Standalone native API and paired route-lookup performance gates passed.
- The OGM adapter's static/native characterization and dependent build gates
  inferred no register, ordering, scheduling or wire-behavior change before
  flashing.
- The corrected bridge-only soak covered 34.94 minutes and remained within
  established failure/timing variance; bridge-address failures did not rise
  against the earlier Sunday baseline.
- The user observed equivalent gameplay/output feel with the candidate bridge
  and existing deployed slaves.

This evidence accepts the extracted implementation for its existing OGM use.
It does not claim cycle-exact equivalence on every host or topology.

### Contract retained

This library is an allocation-free, asynchronous cached store-forward core.
It exposes a flattened local image upstream and dispatches immutable accepted
work downstream later under caller-owned scheduling and policy. It is **not**
a transparent or synchronous Modbus proxy.

OGM lifecycle, child/session generation, retry and recovery policy, public
ACK/fail debt, worker threads, serial ownership and RS485 timing remain in the
product adapter. Future behavior-bearing changes to either side require a new
validation decision; the `v0.1.0` result is scoped to the exact commits above.
