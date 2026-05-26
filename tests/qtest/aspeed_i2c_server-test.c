/*
 * QTests for the openprot i2c_server IPC service running on ast1060-evb.
 *
 * qtest-only coverage: drives the SoC and bus topology directly from
 * the host harness via the i2c-test-master (bus 3) and
 * aspeed-qtest-ctrl helper devices. No guest firmware required.
 *
 * Cases in this file (paths under /ast1060):
 *   - qtest_ctrl/scalar_rw, qtest_ctrl/result_buf_rw, qtest_ctrl/reset_clears
 *   - test_master/probe_ack, test_master/probe_nack
 *   - i2c/slave_config/addr_match
 *   - i2c/slave_config/dev_addr_change_redirects
 *   - i2c/slave_config/dev_addr_readback
 *   - i2c/slave_config/reset_disarms
 *   - i2c/init_speed/ac_timing_no_disrupt
 *   - i2c/master/write_then_read_pca9554
 *   - i2c/master/nack_clears_on_write
 *   - i2c/master/back_to_back
 *   - i2c/master/len_overflow_nacks
 *   - i2c/slave/dma_tx_bus3
 *   - i2c/slave/dma_tx_overread_returns_ff
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "hw/i2c/i2c-test-master.h"
#include "hw/misc/aspeed-qtest-ctrl.h"

/* Sysbus base addresses wired into hw/arm/aspeed_ast10x0_evb.c */
#define TEST_MASTER_BASE 0x7E7C0000
#define QTEST_CTRL_BASE  0x7E7D0000

/* Aspeed I2C controller MMIO map (matches aspeed_i2c-slave-test.c) */
#define I2C_BASE              0x7E7B0000
#define I2C_CTRL_GLOBAL       (I2C_BASE + 0x0c)
#define CTRL_GLOBAL_SRAM_EN   (1u << 0)
#define CTRL_GLOBAL_REG_MODE  (1u << 2)

/* Bus N register block at base + 0x80 * (N + 1) for the AST10x0. */
#define I2C_BUS_BASE(N)       (I2C_BASE + 0x80 * ((N) + 1))
#define BUS3_BASE             I2C_BUS_BASE(3)

/* New-mode register offsets within a bus block. */
#define R_I2CC_FUN_CTRL       0x00
#define R_I2CC_AC_TIMING      0x04
#define R_I2CS_CMD            0x28
#define R_I2CS_DMA_LEN        0x2c
#define R_I2CS_DMA_TX_ADDR    0x38
#define R_I2CS_DEV_ADDR       0x40
#define R_I2CS_DMA_LEN_STS    0x4c

#define FUN_CTRL_SLAVE_EN     (1u << 1)
#define I2CS_CMD_TX_DMA_EN    (1u << 8)

/* Known slaves on ast1060-evb after the AST10x0 I²C patch series */
#define BUS3_PCA9554_ADDR     0x20
#define BUS3_EMPTY_ADDR       0x55

/* Free slave addresses on bus 3, used to arm the Aspeed slave engine
 * without colliding with pca9554 @ 0x20. */
#define BUS3_FREE_SLAVE_ADDR  0x42
#define BUS3_FREE_SLAVE_HIGH  0x43

/* SRAM staging for slave-mode DMA. */
#define SRAM_BASE             0x00000000
#define DMA_OFFSET            0x00020000
#define PATTERN_LEN           16

/*
 * ---------------------------------------------------------------------------
 * aspeed-qtest-ctrl: passive scratchpad round-trip
 * ---------------------------------------------------------------------------
 */
static void test_ctrl_scalar_rw(void)
{
    QTestState *s = qtest_init("-M ast1060-evb -nographic");

    qtest_writel(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_SCENARIO_ID,
                 0xdeadbeef);
    qtest_writel(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_READY, 1);
    qtest_writel(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_STATUS,
                 ASPEED_QTEST_CTRL_STATUS_PASS);
    qtest_writel(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_RESULT_LEN, 7);

    g_assert_cmphex(
        qtest_readl(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_SCENARIO_ID),
        ==, 0xdeadbeef);
    g_assert_cmpuint(
        qtest_readl(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_READY),
        ==, 1);
    g_assert_cmpuint(
        qtest_readl(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_STATUS),
        ==, ASPEED_QTEST_CTRL_STATUS_PASS);
    g_assert_cmpuint(
        qtest_readl(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_RESULT_LEN),
        ==, 7);

    qtest_quit(s);
}

static void test_ctrl_result_buffer_rw(void)
{
    QTestState *s = qtest_init("-M ast1060-evb -nographic");
    unsigned i;

    for (i = 0; i < ASPEED_QTEST_CTRL_RESULT_SIZE; i++) {
        qtest_writeb(s,
                     QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_RESULT + i,
                     (uint8_t)(0xC0 | (i & 0x1f)));
    }
    for (i = 0; i < ASPEED_QTEST_CTRL_RESULT_SIZE; i++) {
        uint8_t b = qtest_readb(s,
                                QTEST_CTRL_BASE +
                                    ASPEED_QTEST_CTRL_R_RESULT + i);
        g_assert_cmpuint(b, ==, (uint8_t)(0xC0 | (i & 0x1f)));
    }
    qtest_quit(s);
}

static void test_ctrl_reset_clears(void)
{
    QTestState *s = qtest_init("-M ast1060-evb -nographic");

    qtest_writel(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_SCENARIO_ID, 42);
    qtest_writel(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_READY, 1);
    qtest_writeb(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_RESULT + 0, 0xAA);

    qtest_system_reset(s);

    g_assert_cmpuint(
        qtest_readl(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_SCENARIO_ID),
        ==, 0);
    g_assert_cmpuint(
        qtest_readl(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_READY),
        ==, 0);
    g_assert_cmpuint(
        qtest_readb(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_RESULT + 0),
        ==, 0);
    qtest_quit(s);
}

/*
 * ---------------------------------------------------------------------------
 * Shared helpers for the i2c-test-master and bus-3 register windows
 * ---------------------------------------------------------------------------
 */
static uint32_t bus3_readl(QTestState *s, unsigned reg)
{
    return qtest_readl(s, BUS3_BASE + reg);
}

static void bus3_writel(QTestState *s, unsigned reg, uint32_t value)
{
    qtest_writel(s, BUS3_BASE + reg, value);
}

static void enable_new_mode(QTestState *s)
{
    qtest_writel(s, I2C_CTRL_GLOBAL,
                 CTRL_GLOBAL_REG_MODE | CTRL_GLOBAL_SRAM_EN);
}

static uint32_t master_status(QTestState *s)
{
    return qtest_readl(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_STATUS);
}

/* "write any to clear" per i2c-test-master.h:26. */
static void master_status_clear(QTestState *s)
{
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_STATUS, 1);
}

static void drive_probe(QTestState *s, uint8_t addr)
{
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_ADDR, addr);
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_LEN,  0);
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_CMD,
                 I2C_TEST_MASTER_CMD_WRITE);
}

static void drive_write_n(QTestState *s, uint8_t addr,
                          const uint8_t *buf, uint32_t len)
{
    uint32_t i;

    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_ADDR, addr);
    for (i = 0; i < len; i++) {
        qtest_writeb(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_BUF + i, buf[i]);
    }
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_LEN, len);
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_CMD,
                 I2C_TEST_MASTER_CMD_WRITE);
}

static void drive_read_n(QTestState *s, uint8_t addr, uint32_t len)
{
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_ADDR, addr);
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_LEN, len);
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_CMD,
                 I2C_TEST_MASTER_CMD_READ);
}

/* Arm the bus-3 slave engine with a TX-DMA pattern at SRAM[DMA_OFFSET]. */
static void arm_slave_tx(QTestState *s, uint8_t addr,
                         const uint8_t *pattern, uint32_t len)
{
    qtest_memwrite(s, SRAM_BASE + DMA_OFFSET, pattern, len);
    bus3_writel(s, R_I2CC_FUN_CTRL, FUN_CTRL_SLAVE_EN);
    bus3_writel(s, R_I2CS_DEV_ADDR, addr);
    bus3_writel(s, R_I2CS_DMA_TX_ADDR, DMA_OFFSET);
    /* TX_BUF_LEN encodes (N - 1); see aspeed_i2c.c:1441. */
    bus3_writel(s, R_I2CS_DMA_LEN, len - 1);
    bus3_writel(s, R_I2CS_CMD, I2CS_CMD_TX_DMA_EN);
}

/*
 * ---------------------------------------------------------------------------
 * i2c-test-master LEN=0 probe
 * ---------------------------------------------------------------------------
 */
static void test_probe_ack(void)
{
    /*
     * pca9554 is instantiated at bus 3 / 0x20 by ast1060_evb_i2c_init,
     * and the i2c-test-master on ast1060-evb is wired to bus 3. An
     * address-phase-only transaction to 0x20 should ACK, leaving STATUS=0.
     */
    QTestState *s = qtest_init("-M ast1060-evb -nographic");

    drive_probe(s, BUS3_PCA9554_ADDR);
    g_assert_cmpuint(master_status(s), ==, 0);
    qtest_quit(s);
}

static void test_probe_nack(void)
{
    /* 0x55 is unoccupied on bus 3. Probe should NACK. */
    QTestState *s = qtest_init("-M ast1060-evb -nographic");

    drive_probe(s, BUS3_EMPTY_ADDR);
    g_assert_cmpuint(master_status(s), ==, I2C_TEST_MASTER_STATUS_NACK);
    qtest_quit(s);
}

/*
 * ---------------------------------------------------------------------------
 * slave_config validation (Aspeed bus 3 as slave)
 * ---------------------------------------------------------------------------
 *
 * The Aspeed slave engine on bus 3 is armed at a free address (0x42)
 * with a TX DMA buffer. A master read from the configured address must
 * succeed; reads from neighbouring addresses must NACK; disabling
 * SLAVE_EN or resetting the SoC must silence the slave entirely.
 */
static void make_pattern(uint8_t *buf, uint8_t high_nibble)
{
    unsigned i;
    for (i = 0; i < PATTERN_LEN; i++) {
        buf[i] = (high_nibble & 0xf0) | (i & 0x0f);
    }
}

static void read_back_buf(QTestState *s, uint8_t *out, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        out[i] = qtest_readb(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_BUF + i);
    }
}

static void test_slave_config_addr_match(void)
{
    QTestState *s = qtest_init("-M ast1060-evb -nographic");
    uint8_t pattern[PATTERN_LEN];
    uint8_t received[PATTERN_LEN];

    make_pattern(pattern, 0xC0);

    enable_new_mode(s);
    arm_slave_tx(s, BUS3_FREE_SLAVE_ADDR, pattern, PATTERN_LEN);

    /* Configured address: ACK + pattern delivered. */
    drive_read_n(s, BUS3_FREE_SLAVE_ADDR, PATTERN_LEN);
    g_assert_cmpuint(master_status(s), ==, 0);
    read_back_buf(s, received, PATTERN_LEN);
    g_assert_cmpmem(received, PATTERN_LEN, pattern, PATTERN_LEN);

    /* Re-arm (TX_DMA_EN self-clears on FINISH), then probe one address
     * higher: nothing answers on 0x43, so the master must NACK. */
    arm_slave_tx(s, BUS3_FREE_SLAVE_ADDR, pattern, PATTERN_LEN);
    master_status_clear(s);
    drive_read_n(s, BUS3_FREE_SLAVE_HIGH, PATTERN_LEN);
    g_assert_cmpuint(master_status(s), ==, I2C_TEST_MASTER_STATUS_NACK);

    qtest_quit(s);
}

static void test_slave_config_dev_addr_change_redirects(void)
{
    /*
     * In new mode the slave's bus address is propagated by writes to
     * I2CS_CMD (aspeed_i2c.c:797-818), not by FUN_CTRL or by writes to
     * DEV_ADDR alone. Writing a new DEV_ADDR and then re-issuing the
     * TX_DMA_EN command should move the slave: the new address must
     * ACK, the old one must NACK.
     */
    QTestState *s = qtest_init("-M ast1060-evb -nographic");
    uint8_t pattern[PATTERN_LEN];
    uint8_t received[PATTERN_LEN];

    make_pattern(pattern, 0xD0);

    enable_new_mode(s);
    arm_slave_tx(s, BUS3_FREE_SLAVE_ADDR, pattern, PATTERN_LEN);

    drive_read_n(s, BUS3_FREE_SLAVE_ADDR, PATTERN_LEN);
    g_assert_cmpuint(master_status(s), ==, 0);

    /* Move the slave: new DEV_ADDR is propagated when I2CS_CMD is
     * written again, so include the full arm sequence at 0x43. */
    arm_slave_tx(s, BUS3_FREE_SLAVE_HIGH, pattern, PATTERN_LEN);

    /* Old address must no longer answer. */
    master_status_clear(s);
    drive_read_n(s, BUS3_FREE_SLAVE_ADDR, PATTERN_LEN);
    g_assert_cmpuint(master_status(s), ==, I2C_TEST_MASTER_STATUS_NACK);

    /* New address must answer with the pattern. arm_slave_tx left the
     * DMA in a known state but I2CS_CMD has self-cleared and DMA was
     * already drained by the read above; re-arm before the final read. */
    arm_slave_tx(s, BUS3_FREE_SLAVE_HIGH, pattern, PATTERN_LEN);
    master_status_clear(s);
    drive_read_n(s, BUS3_FREE_SLAVE_HIGH, PATTERN_LEN);
    g_assert_cmpuint(master_status(s), ==, 0);
    read_back_buf(s, received, PATTERN_LEN);
    g_assert_cmpmem(received, PATTERN_LEN, pattern, PATTERN_LEN);

    qtest_quit(s);
}

static void test_slave_config_dev_addr_readback(void)
{
    /*
     * aspeed_i2c.c:797-799 stores R_I2CS_DEV_ADDR writes verbatim —
     * no field masking. The bus address that actually ends up on the
     * wire is the low 7 bits (i2c-core masks), but the MMIO register
     * round-trips exactly what was written. Assert direct equality so
     * any future model edit that adds reserved bits surfaces here
     * instead of being hidden by a defensive `& 0x7f`.
     */
    QTestState *s = qtest_init("-M ast1060-evb -nographic");

    enable_new_mode(s);

    bus3_writel(s, R_I2CS_DEV_ADDR, 0x42);
    g_assert_cmphex(bus3_readl(s, R_I2CS_DEV_ADDR), ==, 0x42);

    bus3_writel(s, R_I2CS_DEV_ADDR, 0x53);
    g_assert_cmphex(bus3_readl(s, R_I2CS_DEV_ADDR), ==, 0x53);

    qtest_quit(s);
}

static void test_slave_config_reset_disarms(void)
{
    /*
     * qtest_system_reset invokes aspeed_i2c_bus_reset
     * (aspeed_i2c.c:1607-1614), which memsets the bus register block.
     * Pin the actual register-clear effects: FUN_CTRL, DEV_ADDR, and
     * I2CS_CMD all return to zero. Do this without a prior read so
     * TX_DMA_EN is still set pre-reset — otherwise the post-reset
     * "TX_DMA_EN == 0" assertion is satisfied trivially by the
     * I2C_FINISH self-clear (aspeed_i2c.c:1447-1449) and the reset
     * has no observable effect.
     *
     * The corroborating NACK probe at the end relies on
     * aspeed_i2c_bus_new_slave_event returning -1 on I2C_START_RECV
     * when TX_DMA_EN is 0 (aspeed_i2c.c:1433-1436). Note: the
     * i2c-core slave's address binding (set via i2c_slave_set_address
     * at aspeed_i2c.c:817) is not cleared by the bus reset — the
     * slave is still bound to 0x42 on the bus core, but it NACKs at
     * the slave-event layer.
     */
    QTestState *s = qtest_init("-M ast1060-evb -nographic");
    uint8_t pattern[PATTERN_LEN];

    make_pattern(pattern, 0xE0);

    enable_new_mode(s);
    arm_slave_tx(s, BUS3_FREE_SLAVE_ADDR, pattern, PATTERN_LEN);

    /* Pre-reset: arming visible in the register block. */
    g_assert_cmphex(bus3_readl(s, R_I2CC_FUN_CTRL) & FUN_CTRL_SLAVE_EN,
                    ==, FUN_CTRL_SLAVE_EN);
    g_assert_cmphex(bus3_readl(s, R_I2CS_DEV_ADDR),
                    ==, BUS3_FREE_SLAVE_ADDR);
    g_assert_cmphex(bus3_readl(s, R_I2CS_CMD) & I2CS_CMD_TX_DMA_EN,
                    ==, I2CS_CMD_TX_DMA_EN);

    qtest_system_reset(s);

    /* Post-reset: arming registers cleared. */
    g_assert_cmphex(bus3_readl(s, R_I2CC_FUN_CTRL), ==, 0);
    g_assert_cmphex(bus3_readl(s, R_I2CS_DEV_ADDR), ==, 0);
    g_assert_cmphex(bus3_readl(s, R_I2CS_CMD), ==, 0);

    /* Corroborating: probe at the previously-armed address NACKs. */
    master_status_clear(s);
    drive_probe(s, BUS3_FREE_SLAVE_ADDR);
    g_assert_cmpuint(master_status(s), ==, I2C_TEST_MASTER_STATUS_NACK);

    qtest_quit(s);
}

/*
 * ---------------------------------------------------------------------------
 * init_speed: AC_TIMING register
 * ---------------------------------------------------------------------------
 *
 * The model does not simulate I²C bit-timing, but firmware programs
 * the clock-divider register during bring-up. The contract is: writing
 * a divider value must not corrupt subsequent transactions on the bus.
 */
static void test_init_speed_ac_timing_no_disrupt(void)
{
    QTestState *s = qtest_init("-M ast1060-evb -nographic");
    /* Three reference divider words, one per common speed preset. The
     * exact bit-encoding is implementation-defined; values here only
     * need to round-trip through the register without breaking ACK. */
    static const uint32_t dividers[] = {
        0x77743335u,
        0x77743315u,
        0x77743305u,
    };
    unsigned i;

    enable_new_mode(s);
    for (i = 0; i < ARRAY_SIZE(dividers); i++) {
        bus3_writel(s, R_I2CC_AC_TIMING, dividers[i]);
        master_status_clear(s);
        drive_probe(s, BUS3_PCA9554_ADDR);
        g_assert_cmpuint(master_status(s), ==, 0);
    }

    qtest_quit(s);
}

/*
 * ---------------------------------------------------------------------------
 * master ACK / NACK — extended
 * ---------------------------------------------------------------------------
 */
static void test_master_write_then_read_pca9554(void)
{
    /*
     * Standard pca9554 protocol: write 1 byte = command-register index,
     * then read N bytes from that register. pca9554's INPUT register
     * (index 0) resets to 0xFF (hw/gpio/pca9554.c:236).
     */
    QTestState *s = qtest_init("-M ast1060-evb -nographic");
    uint8_t cmd_input[] = { 0x00 };

    drive_write_n(s, BUS3_PCA9554_ADDR, cmd_input, ARRAY_SIZE(cmd_input));
    g_assert_cmpuint(master_status(s), ==, 0);

    master_status_clear(s);
    drive_read_n(s, BUS3_PCA9554_ADDR, 1);
    g_assert_cmpuint(master_status(s), ==, 0);
    g_assert_cmphex(
        qtest_readb(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_BUF), ==, 0xFF);

    qtest_quit(s);
}

static void test_master_nack_clears_on_write(void)
{
    /*
     * NACK sets STATUS bit 0. A subsequent write to STATUS clears it.
     * After clearing, a probe to a populated address must leave STATUS=0.
     */
    QTestState *s = qtest_init("-M ast1060-evb -nographic");

    drive_probe(s, BUS3_EMPTY_ADDR);
    g_assert_cmpuint(master_status(s), ==, I2C_TEST_MASTER_STATUS_NACK);

    master_status_clear(s);
    g_assert_cmpuint(master_status(s), ==, 0);

    drive_probe(s, BUS3_PCA9554_ADDR);
    g_assert_cmpuint(master_status(s), ==, 0);

    qtest_quit(s);
}

static void test_master_back_to_back(void)
{
    /*
     * Three sequential probes (ACK / NACK / ACK) on bus 3 — STATUS
     * must reflect each transaction independently with no residual
     * state from the previous one (after explicit clear between).
     */
    QTestState *s = qtest_init("-M ast1060-evb -nographic");

    drive_probe(s, BUS3_PCA9554_ADDR);
    g_assert_cmpuint(master_status(s), ==, 0);

    master_status_clear(s);
    drive_probe(s, BUS3_EMPTY_ADDR);
    g_assert_cmpuint(master_status(s), ==, I2C_TEST_MASTER_STATUS_NACK);

    master_status_clear(s);
    drive_probe(s, BUS3_PCA9554_ADDR);
    g_assert_cmpuint(master_status(s), ==, 0);

    qtest_quit(s);
}

static void test_master_len_overflow_nacks(void)
{
    /*
     * i2c-test-master.c:50-57 rejects LEN > 64 by setting STATUS=NACK
     * without ever calling i2c_start_transfer. Pin that contract.
     */
    QTestState *s = qtest_init("-M ast1060-evb -nographic");

    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_ADDR,
                 BUS3_PCA9554_ADDR);
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_LEN, 128);
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_CMD,
                 I2C_TEST_MASTER_CMD_READ);
    g_assert_cmpuint(master_status(s), ==, I2C_TEST_MASTER_STATUS_NACK);

    qtest_quit(s);
}

/*
 * ---------------------------------------------------------------------------
 * slave-mode DMA TX on bus 3 — mirror of bus-0 aspeed_i2c-slave-test.c
 * ---------------------------------------------------------------------------
 */
static void test_slave_dma_tx_bus3(void)
{
    QTestState *s = qtest_init("-M ast1060-evb -nographic");
    uint8_t pattern[PATTERN_LEN];
    uint8_t received[PATTERN_LEN];
    uint32_t tx_len_sts;

    make_pattern(pattern, 0xB0);

    enable_new_mode(s);
    arm_slave_tx(s, BUS3_FREE_SLAVE_ADDR, pattern, PATTERN_LEN);

    drive_read_n(s, BUS3_FREE_SLAVE_ADDR, PATTERN_LEN);
    g_assert_cmpuint(master_status(s), ==, 0);
    read_back_buf(s, received, PATTERN_LEN);
    g_assert_cmpmem(received, PATTERN_LEN, pattern, PATTERN_LEN);

    /* TX_LEN status counter equals the bytes shipped. */
    tx_len_sts = bus3_readl(s, R_I2CS_DMA_LEN_STS) & 0x1fff;
    g_assert_cmpuint(tx_len_sts, ==, PATTERN_LEN);

    /* TX_DMA_EN must self-clear via the I2C_FINISH handler. */
    g_assert_cmpuint(bus3_readl(s, R_I2CS_CMD) & I2CS_CMD_TX_DMA_EN, ==, 0);

    qtest_quit(s);
}

static void test_slave_dma_tx_overread_returns_ff(void)
{
    /*
     * The slave engine arms R_I2CC_DMA_LEN to TX_BUF_LEN + 1 on the
     * I2C_START_RECV event (aspeed_i2c.c:1440-1441), so TX_BUF_LEN=0
     * encodes a 1-byte budget — not a zero-length transfer. If the
     * master keeps clocking past that budget, the underflow guard at
     * aspeed_i2c.c:1542-1546 returns 0xFF with LOG_GUEST_ERROR rather
     * than crashing or reading past the DMA buffer.
     *
     * Stage one sentinel byte (0xAA) and over-read by one: the master
     * must observe [0xAA, 0xFF].
     */
    QTestState *s = qtest_init("-M ast1060-evb -nographic");
    uint8_t sentinel[1] = { 0xAA };

    qtest_memwrite(s, SRAM_BASE + DMA_OFFSET, sentinel, 1);

    enable_new_mode(s);
    bus3_writel(s, R_I2CC_FUN_CTRL, FUN_CTRL_SLAVE_EN);
    bus3_writel(s, R_I2CS_DEV_ADDR, BUS3_FREE_SLAVE_ADDR);
    bus3_writel(s, R_I2CS_DMA_TX_ADDR, DMA_OFFSET);
    bus3_writel(s, R_I2CS_DMA_LEN, 0);   /* TX_BUF_LEN=0 → 1-byte budget */
    bus3_writel(s, R_I2CS_CMD, I2CS_CMD_TX_DMA_EN);

    drive_read_n(s, BUS3_FREE_SLAVE_ADDR, 2);
    g_assert_cmpuint(master_status(s), ==, 0);
    g_assert_cmphex(
        qtest_readb(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_BUF + 0),
        ==, 0xAA);
    g_assert_cmphex(
        qtest_readb(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_BUF + 1),
        ==, 0xFF);

    qtest_quit(s);
}

/*
 * ---------------------------------------------------------------------------
 * Entrypoint
 * ---------------------------------------------------------------------------
 */
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/ast1060/qtest_ctrl/scalar_rw",     test_ctrl_scalar_rw);
    qtest_add_func("/ast1060/qtest_ctrl/result_buf_rw", test_ctrl_result_buffer_rw);
    qtest_add_func("/ast1060/qtest_ctrl/reset_clears",  test_ctrl_reset_clears);

    qtest_add_func("/ast1060/test_master/probe_ack",    test_probe_ack);
    qtest_add_func("/ast1060/test_master/probe_nack",   test_probe_nack);

    qtest_add_func("/ast1060/i2c/slave_config/addr_match",
                   test_slave_config_addr_match);
    qtest_add_func("/ast1060/i2c/slave_config/dev_addr_change_redirects",
                   test_slave_config_dev_addr_change_redirects);
    qtest_add_func("/ast1060/i2c/slave_config/dev_addr_readback",
                   test_slave_config_dev_addr_readback);
    qtest_add_func("/ast1060/i2c/slave_config/reset_disarms",
                   test_slave_config_reset_disarms);

    qtest_add_func("/ast1060/i2c/init_speed/ac_timing_no_disrupt",
                   test_init_speed_ac_timing_no_disrupt);

    qtest_add_func("/ast1060/i2c/master/write_then_read_pca9554",
                   test_master_write_then_read_pca9554);
    qtest_add_func("/ast1060/i2c/master/nack_clears_on_write",
                   test_master_nack_clears_on_write);
    qtest_add_func("/ast1060/i2c/master/back_to_back",
                   test_master_back_to_back);
    qtest_add_func("/ast1060/i2c/master/len_overflow_nacks",
                   test_master_len_overflow_nacks);

    qtest_add_func("/ast1060/i2c/slave/dma_tx_bus3",
                   test_slave_dma_tx_bus3);
    qtest_add_func("/ast1060/i2c/slave/dma_tx_overread_returns_ff",
                   test_slave_dma_tx_overread_returns_ff);

    return g_test_run();
}
