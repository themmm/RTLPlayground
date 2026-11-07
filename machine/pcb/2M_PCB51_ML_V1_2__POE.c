#include "2M_PCB51_ML_V1_2.c"


// One RTL8238C on the StandardPOE8-SKY-V1.3 daughterboard, at 0x20 (ports 1-4)
// and 0x21 (ports 5-8); channel order from the vendor firmware's init table
__code const struct poe_cfg poe_cfg = {
	.i2c_bus = I2CBUS(GPIO47_I2C_SDA0, GPIO46_I2C_SCL0),
	.i2c_dev_addr_base = 0x20,
	.port_map = {
		POE_PORT(0, 3), POE_PORT(0, 2), POE_PORT(0, 1), POE_PORT(0, 0),
		POE_PORT(1, 0), POE_PORT(1, 1), POE_PORT(1, 2), POE_PORT(1, 3),
	},
};
