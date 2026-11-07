/*
 * RTL8238C PSE driver.
 *
 * The PSE has no firmware in its own flash: the vendor switch firmware keeps
 * the image in its flash and downloads it over I2C at every boot.
 * tools/poe_extract lifts that image out of the stock image and the build
 * links it in as poe_firmware[].
 *
 * The sequence follows the vendor firmware of the keepLiNK KP-9000-9XHPML-X
 * V1.2 (2M-PCB51-ML-V1.2, V100.9.25), which drives one RTL8238C at 0x20
 * (channels of ports 1-4) and 0x21 (ports 5-8) on I2C bus 0, and was run on
 * that board:
 *
 *   - Detect: register 0x1b, (value & 7) must be 1 or 2, else the vendor
 *     firmware leaves the chip alone.
 *   - Reset: 0x20 to 0x1a, wait, every channel off.
 *   - Arm: 03 00 00 00 to 0xec, wait, read 0xec; if bit 2 is clear,
 *     00 00 20 39 to 0x18, wait, 03 00 00 00 to 0xec again.
 *   - 0xe8 reads 2 when the chip wants the 8028-byte image, the one
 *     poe_extract takes for RTL8238C.
 *   - The whole image to 0xf4 in one I2C write; the vendor firmware ignores
 *     the ACKs of the data bytes. Wait, read 0xec: bit 5 set = accepted.
 *   - Channel on/off: 2-bit mode field in register 0x12 (3 = on) and the
 *     enable bit in 0x14, both read-modify-write of the 4-byte blocks.
 *
 * The image has to arrive in a single transfer, longer than the 16 bytes the
 * RTL837x I2C master takes, so the bus is bit-banged on its two GPIOs, open
 * drain, with clock stretching, as the vendor firmware does for the arm and
 * image writes (it uses the I2C master only for the 4-byte register
 * accesses). Bus 0 (GPIO46 SCL, GPIO47 SDA) is the only bus supported.
 *
 * Selected by MACHINE_POE_CHIP == RTL8238C in the Makefile.
 */
#pragma codeseg BANK3
#pragma constseg BANK3

#include <8051.h>
#include <stdint.h>
#include <stdbool.h>

#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_pins.h"
#include "rtl837x_common.h"
#include "machine.h"
#include "poe.h"

#define POE_PORTS	MACHINE_NUM_RJ45

// GPIO46 and GPIO47 are bits 14 and 15 of the GPIO 32-63 registers,
// bits 6 and 7 of sfr_data[2]
#define SCL_BIT		0x40
#define SDA_BIT		0x80

#define I2C_RETRIES	10

extern __xdata uint8_t sfr_data[4];

static __xdata uint8_t gpio_dir[4];
static __xdata uint8_t i2c_low;
static __xdata uint8_t i2c_ea;
static __xdata uint8_t i2c_busy;
static __xdata uint8_t pse_buf[4];
// Parameters of the register access functions; kept in xdata because the
// 8051 overlay segment has no room for more function parameters.
static __xdata uint8_t pse_dev_a;
static __xdata uint8_t pse_reg_a;
static __xdata uint8_t pse_dev_p;	// pse_port() result
static __xdata uint8_t pse_ch_p;


/* --- I2C on GPIO46/47, open drain: the latches stay 0, the direction bits pull low --- */

static void i2c_wait(void)
{
	__xdata volatile uint8_t i;

	for (i = 0; i < 4; i++)
		;
}

// Drive the lines in i2c_low low and release the others
static void i2c_lines(void)
{
	sfr_data[0] = gpio_dir[0];
	sfr_data[1] = gpio_dir[1];
	sfr_data[2] = (gpio_dir[2] & ~(SCL_BIT | SDA_BIT)) | i2c_low;
	sfr_data[3] = gpio_dir[3];
	reg_write_m(RTL837X_REG_GPIO_32_63_DIRECTION);
	i2c_wait();
}

static void scl_low(void)
{
	i2c_low |= SCL_BIT;
	i2c_lines();
}

static void scl_high(void)
{
	__xdata uint16_t t = 1000;

	i2c_low &= ~SCL_BIT;
	i2c_lines();
	while (!gpio_pin_test(GPIO46_I2C_SCL0) && --t)	// clock stretching
		;
}

static void sda_set(uint8_t v)
{
	if (v)
		i2c_low &= ~SDA_BIT;
	else
		i2c_low |= SDA_BIT;
	i2c_lines();
}

// START, also used as repeated START. Interrupts stay off until the STOP, as
// in the vendor firmware's bit-banged transfers.
static void i2c_start(void)
{
	if (!i2c_busy) {
		i2c_ea = EA;
		EA = 0;
		i2c_busy = 1;
	}
	sda_set(1);
	scl_high();
	sda_set(0);
	scl_low();
}

static void i2c_stop(void)
{
	sda_set(0);
	scl_high();
	sda_set(1);
	i2c_busy = 0;
	EA = i2c_ea;
}

// Returns true if the slave acknowledged the byte
static bool i2c_write_byte(uint8_t b)
{
	__xdata uint8_t i;
	__xdata bool ack;

	for (i = 0; i < 8; i++) {
		sda_set(b & 0x80);
		b <<= 1;
		scl_high();
		scl_low();
	}
	sda_set(1);
	scl_high();
	ack = !gpio_pin_test(GPIO47_I2C_SDA0);
	scl_low();
	return ack;
}

static uint8_t i2c_read_byte(bool ack)
{
	__xdata uint8_t i;
	__xdata uint8_t b = 0;

	sda_set(1);
	for (i = 0; i < 8; i++) {
		scl_high();
		b <<= 1;
		if (gpio_pin_test(GPIO47_I2C_SDA0))
			b |= 1;
		scl_low();
	}
	sda_set(!ack);
	scl_high();
	scl_low();
	sda_set(1);
	return b;
}

// GPIO46/47 as GPIO, latches 0, both released. Run before every access: the
// pins may have been left in another state (e.g. the I2C master function).
static bool i2c_setup(void)
{
	if (((poe_cfg.i2c_bus >> RTL837X_REG_I2C_SCL_SHIFT) & RTL837X_REG_I2C_SCL_MASK)
	    || ((poe_cfg.i2c_bus >> RTL837X_REG_I2C_SDA_SHIFT) & RTL837X_REG_I2C_SDA_MASK)) {
		print_string("PoE: only I2C bus 0 (GPIO46/47) is supported\n");
		return false;
	}
	reg_read_m(RTL837X_PIN_MUX_1);
	sfr_mask_data(0, 0x80, 0x00);	// bits 7-8: SCL bus 0 function off
	sfr_mask_data(1, 0x07, 0x00);	// bits 9-10: SDA bus 0 function off
	reg_write_m(RTL837X_PIN_MUX_1);

	reg_bit_clear(RTL837X_REG_GPIO_32_63_OUTPUT, 14);
	reg_bit_clear(RTL837X_REG_GPIO_32_63_OUTPUT, 15);
	reg_bit_clear(RTL837X_REG_GPIO_32_63_DIRECTION, 14);
	reg_bit_clear(RTL837X_REG_GPIO_32_63_DIRECTION, 15);
	reg_read_m(RTL837X_REG_GPIO_32_63_DIRECTION);
	gpio_dir[0] = sfr_data[0];
	gpio_dir[1] = sfr_data[1];
	gpio_dir[2] = sfr_data[2];
	gpio_dir[3] = sfr_data[3];
	i2c_low = 0;
	i2c_lines();
	return true;
}


/* --- Register access, 7-bit address, one register byte --- */

// START, device pse_dev_a and register pse_reg_a, with the vendor firmware's retries
static bool pse_begin(void)
{
	__xdata uint8_t i;

	for (i = 0; i < I2C_RETRIES; i++) {
		i2c_start();
		if (i2c_write_byte(pse_dev_a << 1) && i2c_write_byte(pse_reg_a))
			return true;
		i2c_stop();
	}
	return false;
}

#define pse_set(a, b, c, d) \
	do { pse_buf[0] = (a); pse_buf[1] = (b); pse_buf[2] = (c); pse_buf[3] = (d); } while (0)

// Select device and register for the next pse_write()/pse_read()
#define pse_at(dev, reg) do { pse_dev_a = (dev); pse_reg_a = (reg); } while (0)

// Write the first n bytes of pse_buf, in that order
static bool pse_write(uint8_t n)
{
	__xdata uint8_t i;
	__xdata uint8_t cnt = n;

	if (!pse_begin())
		return false;
	for (i = 0; i < cnt; i++)
		i2c_write_byte(pse_buf[i]);
	i2c_stop();
	return true;
}

// Read 4 bytes into pse_buf, pse_buf[0] = register pse_reg_a
static bool pse_read(void)
{
	__xdata uint8_t i;

	pse_set(0xff, 0xff, 0xff, 0xff);
	if (!pse_begin())
		return false;
	i2c_start();
	if (!i2c_write_byte((pse_dev_a << 1) | 1)) {
		i2c_stop();
		return false;
	}
	for (i = 0; i < 4; i++)
		pse_buf[i] = i2c_read_byte(i < 3);
	i2c_stop();
	return true;
}


/* --- Ports --- */

// Controller address and channel of a front port (0-based) into pse_dev_p, pse_ch_p
static void pse_port(uint8_t port)
{
	__xdata uint8_t m = poe_cfg.port_map[port];

	if (!(m & 0x80))
		m = POE_PORT(port >> 2, port & 3);
	pse_dev_p = poe_cfg.i2c_dev_addr_base + ((m >> 2) & 0x1f);
	pse_ch_p = m & 3;
}

// Mode field (reg 0x12) and enable bit (reg 0x14) of one port, as the vendor
// firmware's per-port setter does
static void pse_port_cfg(uint8_t port, uint8_t on)
{
	__xdata uint8_t o = on;

	pse_port(port);
	pse_at(pse_dev_p, PSE_REG_STATUS);
	pse_read();
	pse_buf[2] &= ~(0x03 << (pse_ch_p << 1));
	if (o)
		pse_buf[2] |= 0x03 << (pse_ch_p << 1);
	pse_write(4);

	pse_at(pse_dev_p, PSE_REG_ENABLE);
	pse_read();
	pse_buf[0] &= ~(0x01 << pse_ch_p);
	if (o)
		pse_buf[0] |= 0x01 << pse_ch_p;
	pse_write(4);
}

void poe_port_set(uint8_t port_num, uint8_t port_on) __banked
{
	__xdata uint8_t p = port_num;
	__xdata uint8_t o = port_on;

	if (p >= POE_PORTS || !i2c_setup())
		return;
	pse_port_cfg(p, o);
}

void poe_global_set(uint8_t on) __banked
{
	__xdata uint8_t o = on;
	__xdata uint8_t p;

	if (!i2c_setup())
		return;
	for (p = 0; p < POE_PORTS; p++)
		pse_port_cfg(p, o);
}

void poe_get_port(void) __banked
{
	__xdata uint16_t raw, v512;
	__xdata uint8_t cls;

	poe_st_admin = 0;
	poe_st_on = 0;
	poe_st_class = 0xff;
	poe_st_volt = 0;
	poe_st_ma = 0;
	if (poe_port_num >= POE_PORTS || !i2c_setup())
		return;
	pse_port(poe_port_num);

	pse_at(pse_dev_p, PSE_REG_STATUS);
	if (!pse_read())
		return;
	poe_st_admin = ((pse_buf[2] >> (pse_ch_p << 1)) & 0x03) == 0x03;
	poe_st_on = (pse_buf[0] >> (4 + pse_ch_p)) & 1;

	// Class nibble: 1-8 = class 1-8, 0xc = class 0 (#265; class 4 seen on hardware)
	pse_at(pse_dev_p, PSE_REG_CLASS);
	pse_read();
	cls = pse_buf[pse_ch_p] >> 4;
	if (poe_st_on)
		poe_st_class = cls == 0x0c ? 0 : (cls >= 1 && cls <= 8 ? cls : 0xff);

	// Current in units of 125/4096 mA, voltage in 1/512 V
	pse_at(pse_dev_p, PSE_REG_MEAS + (pse_ch_p << 2));
	pse_read();
	raw = ((uint16_t)pse_buf[1] << 8) | pse_buf[0];
	poe_st_ma = (uint16_t)(((uint32_t)raw * 125UL) >> 12);
	v512 = (((uint16_t)pse_buf[3] << 8) | pse_buf[2]) & 0x7fff;
	poe_st_volt = v512 >> 9;
}


/* --- Image download --- */

// poe_firmware[] to register 0xf4 in one transfer. Data bytes without an ACK
// don't stop it, as in the vendor firmware.
static bool pse_write_image(void)
{
	__xdata uint16_t i;

	pse_at(poe_cfg.i2c_dev_addr_base, PSE_REG_DOWNLOAD);
	if (!pse_begin())
		return false;
	for (i = 0; i < poe_firmware_len; i++)
		i2c_write_byte(poe_firmware[i]);
	i2c_stop();
	return true;
}

static bool poe_chip_init(void)
{
	__xdata uint8_t base = poe_cfg.i2c_dev_addr_base;
	__xdata uint8_t port;

	if (!i2c_setup())
		return false;

	// Detect
	pse_at(base, PSE_REG_TYPE);
	if (!pse_read()) {
		print_string("no answer, ");
		return false;
	}
	print_string("type 0x");
	print_byte(pse_buf[0]);
	print_string(", ");
	if ((pse_buf[0] & 0x07) != 1 && (pse_buf[0] & 0x07) != 2)
		return false;

	// Reset, then every port off
	pse_set(0x20, 0, 0, 0);
	pse_at(base, PSE_REG_RESET);
	pse_write(1);
	delay(20);				// vendor: 100 ms units
	for (port = 0; port < POE_PORTS; port++)
		pse_port_cfg(port, 0);

	// Arm the download
	pse_set(0x03, 0x00, 0x00, 0x00);
	pse_at(base, PSE_REG_ARM);
	pse_write(4);
	delay(1);
	pse_read();
	if (!(pse_buf[0] & 0x04)) {
		pse_set(0x00, 0x00, 0x20, 0x39);
		pse_at(base, PSE_REG_ARM2);
		pse_write(4);
		delay(2);
		pse_set(0x03, 0x00, 0x00, 0x00);
		pse_at(base, PSE_REG_ARM);
		pse_write(4);
	}

	// The chip says which image it wants; only the 8028-byte one is linked in
	pse_at(base, PSE_REG_CHIPSET);
	pse_read();
	if (pse_buf[0] != 2 || pse_buf[1] || pse_buf[2] || pse_buf[3]) {
		print_string("0xe8=0x");
		print_byte(pse_buf[0]);
		print_string(" wants another image, ");
		return false;
	}

	if (!pse_write_image())
		return false;
	delay(2);
	pse_at(base, PSE_REG_ARM);
	pse_read();
	if (!(pse_buf[0] & 0x20)) {
		print_string("status 0x");
		print_byte(pse_buf[0]);
		print_string(", ");
		return false;
	}
	delay(2);

	// All ports on, as the vendor firmware's default configuration
	for (port = 0; port < POE_PORTS; port++)
		pse_port_cfg(port, 1);
	return true;
}

void poe_bringup(void) __banked
{
	print_string("\nPoE: loading RTL8238C firmware (");
	itoa_short(poe_firmware_len);
	print_string(" bytes): ");

	if (poe_chip_init())
		print_string("ok, all ports on\n");
	else
		print_string("failed\n");
}

void poe_init(void) __banked
{
	poe_bringup();
}
