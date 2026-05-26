# Aspeed qtests

QEMU-side integration tests for the Aspeed AST10x0 / AST2600 / AST2700 machine
models. Run against the patched `qemu-system-arm` binary built from this tree.

## What's here

| Test binary | Target machine | What it exercises |
|---|---|---|
| `aspeed_gpio-test`   | ast2600-evb | GPIO register behaviour, input/output pins |
| `aspeed_hace-test`   | ast1030-evb | HACE hash engine (SHA-256/384/512) |
| `aspeed_i2c-slave-test`   | ast1030-evb | I²C slave-mode DMA TX (exercises the AST10x0 I²C patch series) |
| `aspeed_i2c_server-test`  | ast1060-evb | `openprot` i2c_server qtest suite (bus 3): qtest-ctrl scratchpad + `i2c-test-master` probe, slave_config validation (addr match / dev_addr redirect / dev_addr readback / reset disarm), init-speed AC_TIMING smoke, master ACK/NACK + status clearing + LEN overflow, slave-mode DMA TX on bus 3 + over-read guard |
| `aspeed_scu-test`    | ast2600-evb | SCU clock/reset register wiring |
| `aspeed_smc-test`    | ast1030-evb | SMC / FMC SPI flash controller |

Source files are `tests/qtest/aspeed_*.c`. The test list is wired into
`qtests_aspeed` in `tests/qtest/meson.build`.

## Test-only devices wired onto the boards

Two synthetic devices exist solely to give qtests a way to drive I²C traffic
and exchange scenario IDs with firmware under test. They are compiled in
under `CONFIG_ASPEED_SOC` and mounted at fixed MMIO addresses.

### `i2c-test-master` — synthetic I²C master

Source: `hw/i2c/i2c-test-master.c`, header `include/hw/i2c/i2c-test-master.h`.

Driven from qtest via MMIO, issues raw `i2c_start_transfer` /
`i2c_send` / `i2c_recv` on a bus it is pinned to. Supports:

- `CMD=READ`  (`1`): N-byte read into BUF
- `CMD=WRITE` (`2`): N-byte write from BUF, or address-phase-only probe when `LEN=0`

### `aspeed-qtest-ctrl` — scratchpad for firmware coordination

Source: `hw/misc/aspeed-qtest-ctrl.c`, header `include/hw/misc/aspeed-qtest-ctrl.h`.

Passive MMIO registers shared between qtest and guest firmware. Used by the
forthcoming `openprot` i2c_server test client to surface scenario ID, ready
flag, status, and a 32-byte result buffer. Performs no interpretation of
register values.

### Mount points per machine

| Base | Machine | Device | I²C bus (if any) |
|---|---|---|---|
| `0x7E7C_0000` | ast1030-evb | `i2c-test-master` | 0 |
| `0x7E7D_0000` | ast1030-evb | `aspeed-qtest-ctrl` | — |
| `0x7E7C_0000` | ast1060-evb | `i2c-test-master` | 3 |
| `0x7E7D_0000` | ast1060-evb | `aspeed-qtest-ctrl` | — |

The bus assignment differs because the existing `aspeed_i2c-slave-test`
targets bus 0 on ast1030-evb, while the i2c_server qtest suite reserves
bus 3 on ast1060-evb to avoid collision with the board's pre-populated
slaves.

## Build

```sh
mkdir -p build-arm && cd build-arm
../configure --target-list=arm-softmmu --enable-debug
ninja
```

Incremental builds after editing an aspeed source or qtest finish in
seconds; full build is ~5–10 min first time.

## Run

All Aspeed qtests at once (also runs non-Aspeed arm qtests):

```sh
cd build-arm
make check-qtest-arm
```

One suite:

```sh
cd build-arm
QTEST_QEMU_BINARY=./qemu-system-arm ./tests/qtest/aspeed_i2c_server-test
```

One case within a suite (glib test selector):

```sh
QTEST_QEMU_BINARY=./qemu-system-arm ./tests/qtest/aspeed_i2c_server-test \
  -p /arm/ast1060/qtest_ctrl/scalar_rw
```

List cases without running:

```sh
QTEST_QEMU_BINARY=./qemu-system-arm ./tests/qtest/aspeed_i2c_server-test -l
```

TAP output for CI consumption:

```sh
QTEST_QEMU_BINARY=./qemu-system-arm ./tests/qtest/aspeed_i2c_server-test --tap
```

Loop over just the Aspeed binaries without firing the full arm suite:

```sh
cd build-arm
for t in tests/qtest/aspeed_*; do
  [ -x "$t" ] || continue
  QTEST_QEMU_BINARY=./qemu-system-arm "$t" --tap
done
```

### `meson test`

Works once too, but the build dir ships with a stale `meson-private/build.dat`
on some trees. If it errors on first use, reconfigure:

```sh
cd build-arm && meson setup --reconfigure
meson test --suite qtest-arm -j 4
```

## Adding a new Aspeed qtest

1. Drop `tests/qtest/aspeed_<what>-test.c`. Use `libqtest-single.h`,
   follow the `qtest_init(...)` + `qtest_readl`/`writel` pattern from
   existing files.
2. Append the name (without `.c`) to `qtests_aspeed` in
   `tests/qtest/meson.build`.
3. If the test needs a bespoke helper device, put it in `hw/i2c/`,
   `hw/misc/`, etc. Add the source under the `CONFIG_ASPEED_SOC`
   stanza of the matching `meson.build`. Expose a `<dev>_create(base, …)`
   helper and call it from `hw/arm/aspeed_<soc>_evb.c`.
4. `ninja` — meson picks up new sources automatically.
5. Run your test and add the name to this README's table.

## Related out-of-tree material

- `/home/ferro/dev/ocp-emea-demo/plan-i2c-server-qtest.md` — planned
  firmware-side test client + full i2c_server scenario matrix that will
  sit on top of `aspeed_i2c_server-test.c`.
- `/home/ferro/dev/ocp-emea-demo/plan-spdm.md` — planned SPDM-over-MCTP
  coverage that layers above the I²C infrastructure.
- `/home/ferro/dev/ocp-emea-demo/ast10x0-i2c-patches/` — the three-patch
  series bringing slave-mode DMA TX and EVB slave population to mainline.
