#ifndef __AYAB_SHIELD__
#define __AYAB_SHIELD__

#include "i2c_mcp23008_virt.h"

#define PIN_V1 2
#define PIN_V2 3
#define PIN_BP 4
#define PIN_LED_A 5
#define PIN_LED_B 6
#define PIN_BEEPER_PORT 'B'
#define PIN_BEEPER_INDEX 1

#define PIN_HALL_RIGHT ADC_IRQ_ADC0
#define PIN_HALL_LEFT  ADC_IRQ_ADC1

typedef struct {
	int len;
	char prefix;
	uint8_t buf[128];
} slipmsg_t;

typedef struct {
    int led[2];
    char beeper_history[16];
    i2c_mcp23008_t mcp23008[2];

	slipmsg_t slip_msg_in;
	slipmsg_t slip_msg_out;
	char slip_history[110];
} shield_t;

#endif
