#ifndef _RTL8238C_H_
#define _RTL8238C_H_

#include <stdint.h>

/* Registers of the PSE, one byte address each, auto-incrementing; the vendor
 * firmware always moves four of them, least significant first. */
#define PSE_REG_CLASS		0x0c	/* high nibble of byte ch: class of channel ch */
#define PSE_REG_STATUS		0x10	/* byte 0 bit 4+ch: powered; byte 2: 2-bit mode per channel */
#define PSE_REG_ENABLE		0x14	/* byte 0 bit ch: channel enabled */
#define PSE_REG_ARM2		0x18
#define PSE_REG_RESET		0x1a
#define PSE_REG_TYPE		0x1b
#define PSE_REG_MEAS		0x30	/* + 4 * ch: bits 0-15 current, bits 16-30 voltage */
#define PSE_REG_CHIPSET		0xe8	/* 2: the 8028-byte image (RTL8238C) */
#define PSE_REG_ARM		0xec	/* control/status of the download */
#define PSE_REG_DOWNLOAD	0xf4	/* the image goes here in one write */

/* poe_cfg.port_map: front port (0-based) -> controller (offset from
 * i2c_dev_addr_base) and channel. Ports without an entry use the default
 * map, base + port / 4, channel port % 4. */
#define POE_PORT(dev, ch)	(0x80 | ((dev) << 2) | (ch))

struct poe_cfg {
	uint8_t i2c_bus;		// I2C bus, I2CBUS() encoded (SCL/SDA pin)
	uint8_t i2c_dev_addr_base;	// 7-bit address of the first controller
	uint8_t port_map[8];		// POE_PORT() per front port, see above
};

/* Firmware image generated from the stock firmware by tools/poe_extract and
 * linked into BANK3 together with the driver. */
extern __code const uint8_t poe_firmware[];
extern __code const uint16_t poe_firmware_len;
extern __code const struct poe_cfg poe_cfg;

#endif
