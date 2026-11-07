/*
 * PoE startup for boards with a PSE controller.
 *
 * The controller has no firmware of its own: the stock switch firmware keeps
 * the PSE image in its flash and downloads it over I2C after reset. The build
 * extracts that image from stock_fw/<machine>.bin (see tools/poe_extract.c),
 * so this file only has to hand the image to the chip driver selected by
 * MACHINE_POE_CHIP.
 */
#pragma codeseg BANK3
#pragma constseg BANK3

#include <stdint.h>
#include <stdbool.h>

#include "machine.h"
#include "rtl837x_common.h"

#ifndef POE_PRESENT
void poe_init(void) __banked
{
	print_string("\nNo PoE Support.\n");
}
#endif

#ifdef POE_PRESENT
/* Inputs and outputs of the driver's poe_get_port(), see poe.h */
__xdata uint8_t poe_port_num;
__xdata uint8_t poe_st_admin;
__xdata uint8_t poe_st_on;
__xdata uint8_t poe_st_class;
__xdata uint8_t poe_st_volt;
__xdata uint16_t poe_st_ma;

/* `poe show`: one line per port from the driver's normalized status */
void poe_show(void) __banked
{
	__xdata uint32_t mw, total = 0;

	print_string("Port Admin Power Class Volt   mA     mW\n");
	for (poe_port_num = 0; poe_port_num < MACHINE_NUM_RJ45; poe_port_num++) {
		poe_get_port();
		mw = (uint32_t)poe_st_volt * poe_st_ma;
		total += mw;
		write_char(' ');
		itoa(poe_port_num + 1);
		print_string(poe_st_admin ? "    on  " : "   off  ");
		print_string(poe_st_on ? " yes  " : "  no  ");
		if (poe_st_class == 0xff)
			print_string("  -  ");
		else {
			print_string("  ");
			itoa(poe_st_class);
			print_string("  ");
		}
		itoa(poe_st_volt);
		print_string("  ");
		itoa_short(poe_st_ma);
		print_string("  ");
		itoa_short((uint16_t)mw);
		write_char('\n');
	}
	print_string("Total: ");
	itoa_short((uint16_t)(total / 1000));
	print_string(" W\n");
}
#endif
