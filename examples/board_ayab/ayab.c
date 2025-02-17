/*
	ayab.c

	Copyright 2008-2011 Michel Pollet <buserror@gmail.com>

 	This file is part of simavr.

	simavr is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	simavr is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <libgen.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <assert.h>

#include "sim_avr.h"
#include "sim_time.h"
#include "sim_core.h"
#include "avr_ioport.h"
#include "avr_adc.h"
#include "avr_twi.h"
#include "avr_extint.h"
#include "avr_uart.h"
#include "sim_elf.h"
#include "sim_hex.h"
#include "sim_gdb.h"
#include "sim_vcd_file.h"
#include "uart_pty.h"
#include "queue.h"

#include "ayab_display.h"

// Hardware interfaces
#include "button.h"
#include "machine.h"
#include "shield.h"

avr_t * avr = NULL;
elf_firmware_t firmware = {{0}};
int loglevel = LOG_ERROR;
int gdb = 0;
int gdb_port = 1234;
int trace_pc = 0;
int trace_machine = 0;
int vcd_enabled = 0;
int quiet = 0;
int msgtrace_enabled = 0;

uart_pty_t uart_pty;

typedef enum {
    TEST_STEP_INIT,
    TEST_STEP_WAITING_FOR_INDSTATE,
    TEST_STEP_KNITTING,
} TestStep;

machine_t machine;
shield_t shield;
int start_needle = 0;
int stop_needle = -1;
char pattern_src[201] = "";
char test_pattern[201] = "";
int test_enabled = 0;
TestStep test_step = TEST_STEP_INIT;
int test_row = 0;
int test_delay = 0;
button_t encoder_v1, encoder_v2;
button_t encoder_beltPhase;
avr_irq_t *adcbase_irq;
avr_irq_t *serial_in_irq;

extern avr_kind_t *avr_kind[];

// analogWrite() stores the PWM duty cycle for pin 9 in that register
#define OCR1A 0x88
#define PORTB 0x25

// half-"width" of the pushing-down part of the circular cams
const int PUSHER_HALFWIDTH_ON = 8;
const int PUSHER_HALFWIDTH_OFF = 16;

enum AYAB_API {
  reqStart = 0x01,
  cnfStart = 0xC1,
  reqLine = 0x82,
  cnfLine = 0x42,
  reqInfo = 0x03,
  cnfInfo = 0xC3,
  reqTest = 0x04,
  cnfTest = 0xC4,
  indState = 0x84,
  helpCmd = 0x25,
  sendCmd = 0x26,
  beepCmd = 0x27,
  setSingleCmd = 0x28,
  setAllCmd = 0x29,
  readEOLsensorsCmd = 0x2A,
  readEncodersCmd = 0x2B,
  autoReadCmd = 0x2C,
  autoTestCmd = 0x2D,
  stopCmd = 0x2E,
  quitCmd = 0x2F,
  reqInit = 0x05,
  cnfInit = 0xC5,
  testRes = 0xEE,
  debug = 0x9F
};

uint8_t CRC8(const uint8_t *buffer, size_t len) {
  uint8_t crc = 0x00U;

  while (len--) {
    uint8_t extract = *buffer;
    buffer++;

    for (uint8_t tempI = 8U; tempI; tempI--) {
      uint8_t sum = (crc ^ extract) & 0x01U;
      crc >>= 1U;

      if (sum) {
        crc ^= 0x8CU;
      }
      extract >>= 1U;
    }
  }
  return crc;
}

static void
list_cores()
{
	printf( "Supported AVR cores:\n");
	for (int i = 0; avr_kind[i]; i++) {
		printf("       ");
		for (int ti = 0; ti < 4 && avr_kind[i]->names[ti]; ti++)
			printf("%s ", avr_kind[i]->names[ti]);
		printf("\n");
	}
	exit(1);
}

static void
display_usage(
	const char * app)
{
	printf("Usage: %s [...] <firmware>\n", app);
	printf(
	 "       [--help|-h|-?]               Display this usage message and exit\n"
	 "       [--list-cores]               List all supported AVR cores and exit\n"
	 "       [-v]                         Raise verbosity level (can be passed more than once)\n"
	 "       [--freq|-f <freq>]           Sets the frequency for an .hex firmware (default 16000000)\n"
	 "       [--mcu|-m <device>]          Sets the MCU type for an .hex firmware (default atmega328)\n"
	 "       [--gdb|-g [<port>]]          Listen for gdb connection on <port> (default 1234)\n"
	 "       [--output|-o <file>]         VCD file to save signal traces\n"
	 "       [--start-vcd|-s              Start VCD output from reset\n"
	 "       [--pc-trace|-p               Add PC to VCD traces\n"
     "       [--machine-trace]            Add Machine states to VCD traces\n"
     "       [--machine <machine>]        Select KH910/KH930/KH270 machine (default=KH910)\n"
     "       [--carriage <carriage>]      Select K/L/G carriage (default=K)\n"
     "       [--beltphase <phase>]        Select Regular/Shifted (default=Regular)\n"
     "       [--startside <side>]         Select Left/Right side to start (default=Left)\n"
     "       [--left-sensor <start,end>]  Positions where left sensor is triggered (default -1,0)\n"
     "       [--right-sensor <start,end>] Positions where left sensor is triggered (default 199,200)\n"
     "       [--pattern <pattern>]        Test pattern (default=none)\n"
     "       [--start-needle <int>]       Start needle (default=0)\n"
     "       [--stop-needle <int>]        Stop needle (default=last)\n"
     "       [--msg-trace]                Trace messages (default=no)\n"
     "       [--quiet]                    Quiet mode (default=no)\n"
	 "       <firmware>                   HEX or ELF file to load (can include debugging syms)\n"
     "\n");
	exit(1);
}

int parse_int_pair(const char *str, int *first, int *second) {
    *first = atoi(str);
    
    const char *comma = strchr(str, ',');
    if (!comma) {
        return 0;
    }
    
    *second = atoi(comma + 1);
    
    return 1;
}

void
parse_arguments(int argc, char *argv[])
{
	if (argc == 1)
		display_usage(basename(argv[0]));

	for (int pi = 1; pi < argc; pi++) {
		if (!strcmp(argv[pi], "--list-cores")) {
			list_cores();
		} else if (!strcmp(argv[pi], "-v")) {
			loglevel++;
		} else if (!strcmp(argv[pi], "-h") || !strcmp(argv[pi], "--help")) {
			display_usage(basename(argv[0]));
		} else if (!strcmp(argv[pi], "-f") || !strcmp(argv[pi], "--freq")) {
			if (pi < argc-1) {
				firmware.frequency = atoi(argv[++pi]);
			} else {
				display_usage(basename(argv[0]));
			}
		} else if (!strcmp(argv[pi], "-m") || !strcmp(argv[pi], "--mcu")) {
			if (pi < argc-1) {
				snprintf(firmware.mmcu, sizeof(firmware.mmcu), "%s", argv[++pi]);
			} else {
				display_usage(basename(argv[0]));
			}
		} else if (!strcmp(argv[pi], "-g") ||
				   !strcmp(argv[pi], "--gdb")) {
			gdb++;
			if (pi < (argc-2) && argv[pi+1][0] != '-' )
				gdb_port = atoi(argv[++pi]);
		} else if (!strcmp(argv[pi], "-o") ||
				   !strcmp(argv[pi], "--output")) {
			if (pi + 1 >= argc) {
				fprintf(stderr, "%s: missing mandatory argument for %s.\n", argv[0], argv[pi]);
				exit(1);
			}
			snprintf(firmware.tracename, sizeof(firmware.tracename), "%s", argv[++pi]);
		} else if (!strcmp(argv[pi], "-s") ||
				   !strcmp(argv[pi], "--start-vcd")) {
            vcd_enabled = 1;
		} else if (!strcmp(argv[pi], "-p") ||
				   !strcmp(argv[pi], "--pc-trace")) {
            trace_pc = 1;
		} else if (!strcmp(argv[pi], "--machine-trace")) {
            trace_machine = 1;
		} else if (!strcmp(argv[pi], "--machine")) {
			if (pi < argc-1) {
                if (!strcmp(argv[++pi], "KH910")) {
                    machine.type = KH910;
                } else if (!strcmp(argv[pi], "KH930")) {
                    machine.type = KH930;
                } else if (!strcmp(argv[pi], "KH270")) {
                    machine.type = KH270;
                } else {
                    display_usage(basename(argv[0]));
                }
            } else {
				display_usage(basename(argv[0]));
            }
		} else if (!strcmp(argv[pi], "--carriage")) {
			if (pi < argc-1) {
                if (!strcmp(argv[++pi], "K")) {
                    machine.carriage.type = KNIT;
                } else if (!strcmp(argv[pi], "L")) {
                    machine.carriage.type = LACE;
                } else if (!strcmp(argv[pi], "G")) {
                    machine.carriage.type = GARTER;
                } else {
                    display_usage(basename(argv[0]));
                }
            } else {
				display_usage(basename(argv[0]));
            }
		} else if (!strcmp(argv[pi], "--startside")) {
			if (pi < argc-1) {
                if (!strcmp(argv[++pi], "Left")) {
                    machine.start_side = LEFT;
                } else if (!strcmp(argv[pi], "Right")) {
                    machine.start_side = RIGHT;
                } else {
                    display_usage(basename(argv[0]));
                }
            } else {
				display_usage(basename(argv[0]));
            }
		} else if (!strcmp(argv[pi], "--left-sensor")) {
			if (!(pi < argc-1 && parse_int_pair(argv[++pi], &machine.left_sensor_start, &machine.left_sensor_end))) {
				display_usage(basename(argv[0]));
			}
		} else if (!strcmp(argv[pi], "--right-sensor")) {
			if (!(pi < argc-1 && parse_int_pair(argv[++pi], &machine.right_sensor_start, &machine.right_sensor_end))) {
				display_usage(basename(argv[0]));
			}
		} else if (!strcmp(argv[pi], "--start-needle")) {
			if (pi < argc-1) {
				start_needle = atoi(argv[++pi]);
			} else {
				display_usage(basename(argv[0]));
			}
		} else if (!strcmp(argv[pi], "--stop-needle")) {
			if (pi < argc-1) {
				stop_needle = atoi(argv[++pi]);
			} else {
				display_usage(basename(argv[0]));
			}
		} else if (!strcmp(argv[pi], "--beltphase")) {
			if (pi < argc-1) {
                if (!strcmp(argv[++pi], "Regular")) {
                    machine.belt_phase = REGULAR;
                } else if (!strcmp(argv[pi], "Shifted")) {
                    machine.belt_phase = SHIFTED;
                } else {
                    display_usage(basename(argv[0]));
                }
            } else {
				display_usage(basename(argv[0]));
            }
		} else if (!strcmp(argv[pi], "--pattern")) {
			if (pi < argc-1) {
                strncpy(pattern_src, argv[++pi], sizeof(pattern_src) - 1);
                pattern_src[sizeof(pattern_src) - 1] = '\0';
                test_enabled = 1;
            } else {
				display_usage(basename(argv[0]));
            }
		} else if (!strcmp(argv[pi], "--quiet")) {
            quiet = 1;
		} else if (!strcmp(argv[pi], "--msg-trace")) {
            msgtrace_enabled = 1;
		} else if (argv[pi][0] != '-') {
            uint32_t loadBase = AVR_SEGMENT_OFFSET_FLASH;
			sim_setup_firmware(argv[pi], loadBase, &firmware, argv[0]);
        } else {
            display_usage(basename(argv[0]));
        }
    }
}

void beeper_history_add(char c) {
    memmove(shield.beeper_history, shield.beeper_history + 1, sizeof(shield.beeper_history) - 1);
    shield.beeper_history[sizeof(shield.beeper_history) - 1] = c;
}

void beeper_set_value(int value) {
    // fprintf(stderr, "beeper at %d\n", value);
    beeper_history_add(value == 0 ? '^' : (value == 255 ? ' ' : '_'));
}

void
beeper_write(
		struct avr_t * avr,
		avr_io_addr_t addr,
		uint8_t v,
		void * param)
{
    switch (addr) {
        case OCR1A:
            beeper_set_value(v);
            break;
        case PORTB:
            beeper_set_value((v & (1 << PIN_BEEPER_INDEX)) ? 255 : 0);
            break;

    }
}

/*
 * called when the AVR changes beeper pin status
 */
void beeper_digital_write(struct avr_irq_t * irq, uint32_t value, void * _param)
{
    beeper_set_value(value ? 255: 0);
}

/* Callback for A-D conversion sampling. */
static void adcTriggerCB(struct avr_irq_t *irq, uint32_t value, void *param)
{
    union {
        avr_adc_mux_t request;
        uint32_t      v;
    }   u = { .v = value };

    switch (u.request.src) {
        case PIN_HALL_RIGHT:
            avr_raise_irq(adcbase_irq + 0, machine.hall_right);
            break;
        case PIN_HALL_LEFT:
            avr_raise_irq(adcbase_irq + 1, machine.hall_left);
            break;
        default:
            fprintf(stderr, "Unexpected ADC_IRQ_OUT_TRIGGER request [0x%04x]\n", u.v);
    }
}

void vcd_trace_enable(int enable) {
    if (avr->vcd) {
        enable ? avr_vcd_start(avr->vcd) : avr_vcd_stop(avr->vcd);
        printf("VCD trace %s (%s)\n", avr->vcd->filename, enable ? "enabled":"disabled");
    }
}

static void serial_inject(uint8_t b) {
    avr_raise_irq(serial_in_irq, b);
}

size_t slip_decode(const uint8_t *input, size_t input_len, uint8_t *output, size_t output_size) {
    size_t output_len = 0;
    while (input_len-- > 0 && ++output_len < output_size)
    {
        if (*input == 0xDB && input_len > 0) {
            input++;
            input_len--;
            *output++ = *input == 0xDC ? 0xC0 : (*input == 0xDD ? 0xDB : *input);
        } else {
            *output++ = *input;
        }
        input++;
    }
    return output_len;
}

size_t slip_encode(const uint8_t *input, size_t input_len, uint8_t *output, size_t output_size) {
    size_t output_len = 0;
    while (input_len-- > 0 && ++output_len < output_size) {
       if (*input == 0xC0 && ++output_len < output_size) {
           *output++ = 0xDB;
           *output++ = 0xDC;
       } else if (*input == 0xDB && ++output_len < output_size) {
           *output++ = 0xDB;
           *output++ = 0xDD;
       } else {
           *output++ = *input;
       }
       input++;
   }
   return output_len;
}

static void slip_inject(const uint8_t *msg, int len) {
    size_t encoded_size = 2 * len;
    uint8_t encoded[encoded_size];
    size_t encoded_len = slip_encode(msg, len, encoded, encoded_size);
    serial_inject(0xc0);
    for (int i = 0; i < encoded_len; i++)
    {
        serial_inject(encoded[i]);
    }
    serial_inject(0xc0);
}

static void check_pattern()
{
    int success = memcmp(test_pattern + start_needle, machine.needles + start_needle, stop_needle - start_needle + 1) == 0;
    if (!success)
    {
        fprintf(stderr, "FAIL on pass %d (going %s)\n", test_row, machine.carriage.direction == LEFTWARDS ? "left" : "right");
        fprintf(stderr, "expected=%.*s\n", stop_needle - start_needle + 1, test_pattern + start_needle);
        fprintf(stderr, "actual  =");
        for (int i = start_needle; i <= stop_needle; i++)
        {
            fprintf(stderr, "%s%c\x1b[0m", test_pattern[i] != machine.needles[i] ? "\x1b[7m" : "", machine.needles[i]);
        }
        fprintf(stderr, "\n");
        exit(1);
    }
}

static void display() {
    fprintf(stderr, "S=[");
    for (int solenoid_index=0; solenoid_index<machine.num_solenoids;solenoid_index++) {
        if ((machine.solenoid_states ^ machine.previous_solenoid_states) & (1 << solenoid_index)) {
            fprintf(stderr, "\x1b[7m");
        }
        fprintf(stderr, "%c\x1b[0m", machine.solenoid_states & (1<<solenoid_index) ? '.' : '|');
    }
    fprintf(stderr, "], Pos = %6.2f, EP = %2d, BP = %d, Sensors = (%4d, %4d) B=%.*s\n",
            machine.carriage.position + (machine.encoder_phase % 4) / 4.0,
            machine.encoder_phase,
            machine.belt_phase_signal, machine.hall_left, machine.hall_right, (int)sizeof(shield.beeper_history), shield.beeper_history);
    if (shield.beeper_history[sizeof(shield.beeper_history) - 1] == ' ')
        beeper_history_add(' ');
    fprintf(stderr, "A=[");
    for (int armature_index=0; armature_index<machine.num_solenoids; armature_index++) {
        if ((machine.armature_states ^ machine.previous_armature_states) & (1 << armature_index)) {
            fprintf(stderr, "\x1b[7m");
        }
        if (machine.selecting_armature == armature_index) {
            fprintf(stderr, "\x1b[1m");
        }

        int angle_from_pusher = abs(((int)machine.encoder_phase + (machine.num_solenoids - armature_index) * 4)
                                        % (machine.num_solenoids * 4)
                                        - (machine.num_solenoids * 2));

        if (angle_from_pusher <= PUSHER_HALFWIDTH_OFF) {
            fprintf(stderr, angle_from_pusher <= PUSHER_HALFWIDTH_ON ? "\x1b[9m" : "\x1b[4m");
        }
        fprintf(stderr, "%c\x1b[0m", machine.armature_states & (1 << armature_index) ? '.' : '|');
    }
    fprintf(stderr, "] %.*s\n", (int)sizeof(shield.slip_history), shield.slip_history);
    machine.previous_armature_states = machine.armature_states;
    machine.previous_solenoid_states = machine.solenoid_states;

    char needle_buffer[MARGIN_NEEDLES + machine.num_needles + MARGIN_NEEDLES];
    char carriage_buffer[MARGIN_NEEDLES + machine.num_needles + MARGIN_NEEDLES];
    memset(carriage_buffer, ' ', sizeof(carriage_buffer));
    memset(needle_buffer, ' ', sizeof(needle_buffer));

    if ((machine.carriage.selected_needle >= -MARGIN_NEEDLES) && (machine.carriage.selected_needle < machine.num_needles + MARGIN_NEEDLES)) {
        carriage_buffer[machine.carriage.selected_needle + MARGIN_NEEDLES] = 'x';
    }
    if ((machine.carriage.position >= -MARGIN_NEEDLES) && (machine.carriage.position < machine.num_needles + MARGIN_NEEDLES)) {
        carriage_buffer[machine.carriage.position + MARGIN_NEEDLES] = '^';
    }

    int half_num_needles = machine.num_needles / 2;
    for (int i=0; i < machine.num_needles; i++) {
        needle_buffer[MARGIN_NEEDLES + i] = machine.needles[i];
    }
    fprintf(stderr, "<- %.*s\n   %.*s\n",
            half_num_needles + MARGIN_NEEDLES, needle_buffer,
            half_num_needles + MARGIN_NEEDLES, carriage_buffer);
    fprintf(stderr, "-> %.*s\n   %.*s\n",
            half_num_needles + MARGIN_NEEDLES, needle_buffer + half_num_needles + MARGIN_NEEDLES,
            half_num_needles + MARGIN_NEEDLES, carriage_buffer + half_num_needles + MARGIN_NEEDLES);
}

static void * avr_run_thread(void * param)
{
	int state = cpu_Running;
    avr_cycle_count_t lastChange = avr->cycle;
    int *run = (int *)param;
    avr_irq_t vcd_irq_pc;
    avr_irq_t vcd_irq_carriage_position;
    avr_irq_t vcd_irq_solenoids_state;
    avr_irq_t vcd_irq_hall_left, vcd_irq_hall_right;
    
#if AVR_STACK_WATCH
    uint16_t SP_min = (avr->data[R_SPH]<<8) +  avr->data[R_SPL];
#endif

    // This makes emulation slightly faster (and more significantly, more regular)
    // by avoiding a repeated check for level-triggered interrupts
    // (which AYAB does't use anyway) when Arduino pins 2 or 3 are set to low.
    avr_extint_set_strict_lvl_trig(avr, 0, 0);
    avr_extint_set_strict_lvl_trig(avr, 1, 0);

    // Initialize VCD for PC-only traces
    if (!avr->vcd && (trace_pc || trace_machine)) {
        avr->vcd = malloc(sizeof(*avr->vcd));
        avr_vcd_init(avr,
            firmware.tracename[0] ? firmware.tracename: "simavr.vcd",
            avr->vcd,
            100000 /* usec */
        );
    }

    // Add machine data to vcd file
    if (trace_machine) {
        avr_vcd_add_signal(avr->vcd, encoder_v1.irq + IRQ_BUTTON_OUT, 1, "v1");
        avr_vcd_add_signal(avr->vcd, encoder_v2.irq + IRQ_BUTTON_OUT, 1, "v2");
        avr_vcd_add_signal(avr->vcd, encoder_beltPhase.irq + IRQ_BUTTON_OUT, 1, "BP");
        avr_vcd_add_signal(avr->vcd, &vcd_irq_hall_left, 16, "EOL");
        avr_vcd_add_signal(avr->vcd, &vcd_irq_hall_right, 16, "EOR");
        avr_vcd_add_signal(avr->vcd, &vcd_irq_carriage_position, 16, "Position");
        avr_vcd_add_signal(avr->vcd, &vcd_irq_solenoids_state, 16, "Solenoids");
    }

    // Add Program Counter (PC) to vcd file
    if (trace_pc) {
        avr_vcd_add_signal(avr->vcd, &vcd_irq_pc, 16, "PC");
    }

    vcd_trace_enable(vcd_enabled);

    // {v1, v2} encoding over 4 phases
    unsigned phase_map[4] = {0, 1, 3, 2};

    // encoder_phase has 4 states per solenoid, i.e. the camshaft
    // does a complete revolution in <solenoid count> * 4 steps.
    // We define encoder_phase to be 0 when the first cam (connected to
    // solenoid 0) is in side-pushing position.
    machine.encoder_phase = 0;

    memset(machine.needles, '.', machine.num_needles);
    machine.belt_phase_signal = 0;
    machine.carriage.direction = machine.start_side == LEFT ? RIGHTWARDS : LEFTWARDS;
    machine.carriage.selected_needle = machine.start_side == LEFT ? -MARGIN_NEEDLES - 1 : machine.num_needles + MARGIN_NEEDLES + 1;
    machine.dirty = 1;

	while (*run && (state != cpu_Done) && (state != cpu_Crashed))
    {
        // Limit event/interrupt rate towards avr
        if (avr_cycles_to_usec(avr, avr->cycle - lastChange) > 10000) {
            lastChange = avr->cycle;

            enum event_type event;
            int value;
            int event_available = 0;
            if (test_enabled) {
                if (test_delay > 0)
                {
                    test_delay--;
                }
                else
                {
                    switch(test_step) {
                    case TEST_STEP_INIT:
                    {
                        uint8_t buf[3] = {reqInit, machine.type };
                        buf[2] = CRC8(buf, 2);
                        slip_inject(buf, sizeof(buf));
                        test_step = TEST_STEP_WAITING_FOR_INDSTATE;
                        break;
                    }
                    case TEST_STEP_WAITING_FOR_INDSTATE:
                    case TEST_STEP_KNITTING:
                        event = ((machine.start_side == LEFT) ^ (test_row % 2 == 1)) ? CARRIAGE_RIGHT : CARRIAGE_LEFT;
                        event_available = 1;
                        break;
                    }
                }
            }
            else
            {
                event_available = queue_pop(&event_queue, &event, &value);
            }

            if (event_available)
            {
                unsigned new_phase = machine.encoder_phase;
                switch (event)
                {
                    case RESET_ARDUINO:
                        fprintf(stderr, "Resetting Arduino\n");
                        avr_reset(avr);
                        break;
                    case CARRIAGE_LEFT:
                        machine.carriage.direction = LEFTWARDS;
                        new_phase = (machine.encoder_phase + machine.num_solenoids * 4 - 1)%(machine.num_solenoids * 4);
                        if ((new_phase%4) == 3) {
                            if (machine.carriage.position > -MARGIN_NEEDLES) {
                                machine.carriage.position--;
                            } else {
                                machine.carriage.position = -MARGIN_NEEDLES;
                                new_phase = machine.encoder_phase;
                            }
                        }
                        break;
                    case CARRIAGE_RIGHT:
                        machine.carriage.direction = RIGHTWARDS;
                        new_phase = (machine.encoder_phase + 1)%(machine.num_solenoids * 4);
                        if ((new_phase%4) == 0) {
                            if (machine.carriage.position < (machine.num_needles + MARGIN_NEEDLES - 1)) {
                                machine.carriage.position++;
                            } else {
                                machine.carriage.position = (machine.num_needles + MARGIN_NEEDLES - 1);
                                new_phase = machine.encoder_phase;
                            }
                        }
                        break;
                    case VCD_DUMP:
                        vcd_enabled = ! vcd_enabled;
                        vcd_trace_enable(vcd_enabled);
                        break;
                    default:
                        fprintf(stderr, "Unexpect event from graphic thread\n");
                        break;
                }
                machine.encoder_phase = new_phase;

                machine.hall_left = 1650;
                machine.hall_right = 1650;

                uint16_t wire_states = (shield.mcp23008[1].reg[MCP23008_REG_OLAT] << 8) + shield.mcp23008[0].reg[MCP23008_REG_OLAT];
                
                // This is where we handle the weird wiring of the KH270's 12 solenoids
                machine.solenoid_states = machine.type == KH270 ? (wire_states >> 3) & 0x0fff : wire_states;

                for (int solenoid_index=0; solenoid_index<machine.num_solenoids; solenoid_index++) {
                    uint16_t solenoid_mask = 1 << solenoid_index;
                    uint16_t solenoid_state = machine.solenoid_states & solenoid_mask;
                    // Solenoid 0 is at angle 0 (side-pushing) when encoder_phase is 0
                    // -> angle from down-pusher is 32 (KH9xx) or 24 (KH270) (half period).
                    // Solenoid 1 is at angle 0 (side-pushing) when encoder_phase is 4.
                    int angle_from_pusher = abs(((int)machine.encoder_phase + (machine.num_solenoids - solenoid_index) * 4)
                                                 % (machine.num_solenoids * 4)
                                                 - (machine.num_solenoids * 2));
                    // The solenoid can:
                    //  - pull the armature down when angle from cam's down-pusher is small enough
                    //    (approx. 20 degrees on either side)
                    //  - release the armature at any point, but if more than ~100 degrees away from pusher,
                    //    the lever will not start riding the cam's side-pusher until the next cycle.
                    // In other conditions, the lever will behave as if the solenoid had not changed state.
                    if (angle_from_pusher <= PUSHER_HALFWIDTH_ON || (angle_from_pusher <= PUSHER_HALFWIDTH_OFF && !solenoid_state)) {
                        // fprintf(stderr, "phase=%d, copying bit %d\n", machine.encoder_phase, solenoid_index);
                        machine.armature_states = (machine.armature_states & ~solenoid_mask) | solenoid_state;
                    }
                }

                int select_offset = 0;
                switch (machine.carriage.type) {
                    case KNIT:
                        // Handle hall sensors
                        if (machine.carriage.position >= machine.left_sensor_start && machine.carriage.position <= machine.left_sensor_end) {
                            machine.hall_left = 2200; //TBC North
                        } else if (machine.carriage.position >= machine.right_sensor_start && machine.carriage.position <= machine.right_sensor_end) {
                            machine.hall_right = 2200; //TBC North
                            if(machine.type == KH910) { // Shield error
                                machine.hall_right = 0; // Digital low
                            }
                        }
                        // Handle solenoids
                        select_offset = 24;
                        if (machine.carriage.direction == RIGHTWARDS) {
                            select_offset = -24;
                        }
                        break;
                    case LACE:
                        if (machine.carriage.position >= machine.left_sensor_start && machine.carriage.position <= machine.left_sensor_end) {
                            machine.hall_left = 100; //TBC South
                        } else if (machine.carriage.position >= machine.right_sensor_start && machine.carriage.position <= machine.right_sensor_end) {
                            machine.hall_right = 100; //TBC South
                            if(machine.type == KH910) { // Shield error
                                machine.hall_right = 1650; // HighZ
                            }
                        }
                        // Handle solenoids
                        select_offset = 12;
                        if (machine.carriage.direction == RIGHTWARDS) {
                            select_offset = -12;
                        }
                        break;
                    case GARTER:
                        switch (machine.carriage.position) {
                            case -12:
                            case  12:
                                machine.hall_left = 100; //TBC South
                                break;
                            case -10:
                            case  10:
                                machine.hall_left = 2200; //TBC North
                                break;
                            case 199 - 12:
                            case 199 + 12:
                                machine.hall_right = 100; //TBC South
                                if(machine.type == KH910) { // Shield error
                                    machine.hall_right = 1650; // HighZ
                                }
                                break;
                            case 199 - 10:
                            case 199 + 10:
                                machine.hall_right = 2200; //TBC North
                                if(machine.type == KH910) { // Shield error
                                    machine.hall_right = 0; // Digital low
                                }
                                break;
                            default:
                                break;
                        }
                        // Handle solenoids
                        select_offset = 0;
                        break;
                    case KNIT270:
                        // Handle hall sensors
                        for (int magnet_offset = -3; magnet_offset <= 3; magnet_offset += 6)
                        {
                            if (machine.carriage.position + magnet_offset >= machine.left_sensor_start && machine.carriage.position + magnet_offset <= machine.left_sensor_end)
                            {
                                machine.hall_left = 2200; // TBC North
                            }
                            else if (machine.carriage.position + magnet_offset >= machine.right_sensor_start && machine.carriage.position + magnet_offset <= machine.right_sensor_end)
                            {
                                machine.hall_right = 2200; // TBC North
                            }
                        }
                        // Handle solenoids
                        select_offset = 15;
                        if (machine.carriage.direction == RIGHTWARDS) {
                            select_offset = -15;
                        }
                        break;
                    default:
                        fprintf(stderr, "Unexpected carriage type (%d)\n", machine.carriage.type);
                        break;
                }

                machine.carriage.selected_needle = machine.carriage.position + select_offset;
                if ((machine.carriage.selected_needle < machine.num_needles) && (machine.carriage.selected_needle >= 0)) {
                    int armature_index;
                    switch (machine.type) {
                        case KH270:
                            // Needle 0 (L56) is driven by solenoid 4 (5 in 1..12 numbering)
                            armature_index = machine.carriage.selected_needle + 4;
                            // K270 solenoid to needle mapping is direction-dependent
                            if (machine.carriage.direction == LEFTWARDS) {
                                armature_index += machine.num_solenoids / 2;
                            }
                            break;
                        default:
                            armature_index = machine.carriage.selected_needle;
                            // Solenoid to needle mapping is belt phase-dependent
                            if (machine.belt_phase == SHIFTED) {
                                armature_index += machine.num_solenoids / 2;
                            }
                            // LACE solenoid to needle mapping is direction-dependent
                            if ((machine.carriage.type == LACE) && (machine.carriage.direction == LEFTWARDS)) {
                                armature_index += machine.num_solenoids / 2;
                            }
                            break;
                    }
                    armature_index = armature_index % machine.num_solenoids;
                    machine.selecting_armature = armature_index;

                    machine.needles[machine.carriage.selected_needle] = machine.armature_states & (1 << armature_index) ? '.' : '|';
                }

                avr_raise_irq(encoder_v2.irq + IRQ_BUTTON_OUT, (phase_map[machine.encoder_phase % 4] & 1) ? 1 : 0);
                avr_raise_irq(encoder_v1.irq + IRQ_BUTTON_OUT, (phase_map[machine.encoder_phase % 4] & 2) ? 1 : 0);

                // Belt phase signal is slightly early compared to encoder phase
                machine.belt_phase_signal = ((machine.encoder_phase + BELT_PHASE_ADVANCE) % (machine.num_solenoids * 4) > (machine.num_solenoids * 2)) ? 0 : 1;
                avr_raise_irq(encoder_beltPhase.irq + IRQ_BUTTON_OUT, machine.belt_phase_signal);

                machine.dirty = 1;
            }
        }

        if (machine.dirty) {
            if (!quiet)
                display();

            // Trigger IRQ for machine internal data
            if (trace_machine) {
                avr_raise_irq(&vcd_irq_hall_left, machine.hall_left);
                avr_raise_irq(&vcd_irq_hall_right, machine.hall_right);
                avr_raise_irq(&vcd_irq_carriage_position, machine.carriage.position);
                avr_raise_irq(&vcd_irq_solenoids_state, machine.solenoid_states);
            }

            if (test_enabled) {
                if ((machine.carriage.direction == RIGHTWARDS && machine.carriage.selected_needle > stop_needle + (machine.carriage.type == GARTER ? 12 : 0)) ||
                    (machine.carriage.direction == LEFTWARDS && machine.carriage.selected_needle < start_needle - (machine.carriage.type == GARTER ? 12 : 0))) {
                    check_pattern();
                    if (test_step == TEST_STEP_KNITTING && test_row == 0)
                    {
                        test_row++;
                    }
                    else
                    {
                        exit(0);
                    }
                }
            }

            machine.dirty = 0;
        }

        // Trigger IRQ for a PC trace
        if (trace_pc && avr->vcd->output) {
            avr_raise_irq(&vcd_irq_pc, avr->pc);
        }

#if AVR_STACK_WATCH
        uint16_t SP = (avr->data[R_SPH]<<8) +  avr->data[R_SPL];
        if (SP < SP_min) {
            SP_min = SP;
            fprintf(stderr, "-- New SP minima (SP = 0x%04x) --\n", SP);
            DUMP_STACK();
        }
#endif
        // Run one AVR cycle
		state = avr_run(avr);
    }

	return NULL;
}

static void slip_process(slipmsg_t *msg);

static void slip_history_add(char c) {
	memmove(shield.slip_history, shield.slip_history + 1, sizeof(shield.slip_history) - 1);
	shield.slip_history[sizeof(shield.slip_history) - 1] = c;
}

static void slip_record(char prefix, const uint8_t *decoded, size_t decoded_len) {
	slip_history_add(prefix);
	slip_history_add(' ');
	for (int i = 0; i < decoded_len; i++)
	{
		if (isprint(decoded[i]))
		{
			slip_history_add(decoded[i]);
		}
		else
		{
			char s[5];
			snprintf(s, sizeof(s), "\\%02x", decoded[i]);
			slip_history_add(s[0]);
			slip_history_add(s[1]);
			slip_history_add(s[2]);
		}
	};
	slip_history_add(' ');
}

static uint64_t real_us() {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    return start.tv_sec * 1000000 + start.tv_nsec / 1000;
}

static uint32_t delta_avr() {
    static uint32_t last_us = 0;
    uint32_t us = avr_cycles_to_usec(avr, avr->cycle);
    uint32_t delta = us - last_us;
    last_us = us;
    return delta;
}

static uint32_t delta_real() {
    static uint64_t last_us = 0;
    uint64_t us = real_us();
    uint32_t delta = us - last_us;
    last_us = us;
    return delta;
}

#define TRACE(format, ...) if (msgtrace_enabled) fprintf(stderr, "%+10d %+10d " format "\n", delta_avr(), delta_real(), ## __VA_ARGS__)

static void slip_process(slipmsg_t *msg)
{
    machine.dirty = 1;
    size_t decoded_size = 2 * msg->len;
    uint8_t decoded[decoded_size];
    size_t decoded_len = slip_decode(msg->buf, msg->len, decoded, decoded_size);
    if (decoded_len == 0) {
        return;
    }
    slip_record(msg->prefix, decoded, decoded_len);
    if (test_enabled)
    {
        switch (decoded[0])
        {
        case reqInfo:
            TRACE("reqInfo");
            break;
        case cnfInfo:
            TRACE("cnfInfo");
            break;
        case reqInit:
            TRACE("reqInit");
            break;
        case cnfInit:
            TRACE("cnfInit");
            assert(decoded_len > 1 && decoded[1] == 0);
            break;
        case indState:
            TRACE("indState");
            if (test_enabled && test_step == TEST_STEP_WAITING_FOR_INDSTATE)
            {
                uint8_t buf[5] = {
                    reqStart, start_needle, stop_needle, 
                    0x02 // 1 = continous reporting, 2 = enable hardware beep
                };
                buf[4] = CRC8(buf, 4);
                slip_inject(buf, sizeof(buf));
                test_step = TEST_STEP_KNITTING;
            }
            break;
        case reqStart:
            TRACE("reqStart");
            break;
        case cnfStart:
            TRACE("cnfStart(%d)", decoded[1]);
            assert(decoded_len > 1 && decoded[1] == 0);
            break;
        case reqLine:
            TRACE("reqLine(%d)", decoded[1]);
            assert(decoded_len > 1);
            if (test_enabled)
            {
                int requestedLine = decoded[1];
                int patlen = strlen(test_pattern);
                int bufLen = 4 + (patlen + 7) / 8 + 1;
                uint8_t buf[bufLen];
                memset(buf, 0, bufLen);
                buf[0] = cnfLine;
                buf[1] = requestedLine;
                for (int i=0; i < patlen; i++) {
                    if (test_pattern[i] == '|') {
                        buf[ 4 + i / 8 ] |= 1 << (i % 8);
                    }
                }
                buf[bufLen - 1] = CRC8(buf, bufLen - 1);
                slip_inject(buf, bufLen);
                test_delay = 10;
            }
            break;
        case cnfLine:
            assert(decoded_len > 1);
            TRACE("cnfLine(%d)", decoded[1]);
            break;
        case 0xff:
            TRACE("%.*s", (int)decoded_len - 1, decoded + 1);
            break;
        default:
            TRACE("msg %02x", decoded[0]);
        }
    }
}

static void slip_add_byte(
    	struct avr_irq_t *irq,
		uint32_t value,
		void * param)
{
    slipmsg_t *msg = (slipmsg_t *)param;
    if (value == 0xc0)
    {
        if (msg->len > 0)
        {
            slip_process(msg);
            msg->len = 0;
        }
    }
    else if (msg->len < sizeof(msg->buf))
    {
        msg->buf[msg->len++] = value;
    }
}

/*
 * called when the AVR change pins on port D
 */
void port_d_changed_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
    switch (irq->irq) {
        case PIN_LED_A: // LED_A
            shield.led[0] = shield.led[0] == 0 ? 1 : 0;
            break;
        case PIN_LED_B: // LED_B
            shield.led[1] = shield.led[1] == 0 ? 1 : 0;
            break;
    }
}

int main(int argc, char *argv[])
{
    machine.type = KH910;
    machine.num_needles = 200;
    machine.num_solenoids = 16;
    machine.carriage.type = KNIT;
    machine.belt_phase = REGULAR;
    machine.left_sensor_start = INT32_MIN;
    machine.left_sensor_end = 0;
    machine.right_sensor_start = INT32_MAX;
    machine.right_sensor_end = 200;

    strncpy(firmware.mmcu, "atmega328", sizeof(firmware.mmcu));
    firmware.frequency = 16000000;

    parse_arguments(argc, argv);

    if (machine.type == KH270) {
        machine.num_needles = 112;
        machine.num_solenoids = 12;
        machine.carriage.type = KNIT270;
    }

    if (machine.left_sensor_start == INT32_MIN) {
        machine.left_sensor_end = machine.type == KH270 ? -3 : 0;
        machine.left_sensor_start = machine.left_sensor_end - 1;
    }

    if (machine.right_sensor_start == INT32_MAX) {
        machine.right_sensor_start = machine.type == KH270 ? 114 : 199;
        machine.right_sensor_end = machine.right_sensor_start + 1;
    }

    assert(machine.left_sensor_start <= machine.left_sensor_end);
    assert(machine.left_sensor_end < machine.right_sensor_start);
    assert(machine.right_sensor_start <= machine.right_sensor_end);

    if (stop_needle < 0) {
        stop_needle = machine.num_needles - 1;
    }

    assert(start_needle >= 0 && start_needle < machine.num_needles);
    assert(stop_needle >= start_needle && stop_needle < machine.num_needles);

    if (test_enabled) {
        int pattern_src_len = strlen(pattern_src);
        assert(pattern_src_len > 0);

        memset(test_pattern, '.', sizeof(test_pattern) - 1);
        for (int i=start_needle; i<=stop_needle; i++) {
            test_pattern[i] = pattern_src[(i - start_needle) % pattern_src_len];
        }
    }

    if (!test_enabled) {
        printf (
            "---------------------------------------------------------\n"
            "AYAB shield emulation \n"
            "- Use left/right arrows to move the carriage\n"
            "- 'v' to start/stop VCD traces"
            "- 'q' or ESC to quit\n"
            "----------------------------------------------------------\n"
        );
    }

    // Set carriage position so that encoder phase is 0
    // when left needle selector is at needle 0
    // (in regular belt shift)
    switch (machine.carriage.type) {
        case KNIT:
            machine.carriage.position = 24;
            break;
        case LACE:
            machine.carriage.position = 12;
            break;
        case GARTER:
            machine.carriage.position = 0;
            break;
        case KNIT270:
            // According to the KH270 service manual, needle L48 is the leftmost one
            // that is driven by the first solenoid. We call needle L56 needle 0, so
            // needle L48 is needle 8.
            // Selectors on the KH270 carriage are 15 needles away from the center,
            // so the left needle selector is at a needle controlled by solenoid 0
            // when the center is at needle 23.
            machine.carriage.position = 23;
            break;
        default:
            abort();
    }
    // Shift carriage by half-solenoid count if alternate belt engagement is used
    if (machine.belt_phase == SHIFTED) {
        machine.carriage.position += machine.num_solenoids / 2;
    }
    
    // Move carriage to starting side, preserving phase
    int newpos;
    switch (machine.start_side) {
        case LEFT:
            while ((newpos = machine.carriage.position - machine.num_solenoids) >= -MARGIN_NEEDLES) {
                machine.carriage.position = newpos;
            }
            break;
        case RIGHT:
            while ((newpos = machine.carriage.position + machine.num_solenoids) < machine.num_needles + MARGIN_NEEDLES) {
                machine.carriage.position = newpos;
            }
            break;
        default:
            abort();
    }

	avr = avr_make_mcu_by_name(firmware.mmcu);
	if (!avr) {
        if (! strcmp(firmware.mmcu, "")) {
            fprintf(stderr, "%s: AVR mcu not defined\n", argv[0]);
        } else {
            fprintf(stderr, "%s: AVR '%s' not known\n", argv[0], firmware.mmcu);
        }
		exit(1);
	}

	avr_init(avr);
	avr->log = (loglevel > LOG_TRACE ? LOG_TRACE : loglevel);

	avr_load_firmware (avr, &firmware);
	if (firmware.flashbase) {
		printf("Attempt to load a bootloader at %04x\n", firmware.flashbase);
		avr->pc = firmware.flashbase;
	}

	// Enable gdb
    avr->gdb_port = gdb_port;
	if (gdb) {
		avr->state = cpu_Stopped;
		avr_gdb_init(avr);
	}

    // Connect uart 0 to a virtual pty
    char uart = '0';


    // Serial (SLIP) monitor
    shield.slip_msg_in.prefix = '<';
    shield.slip_msg_out.prefix = '>';
	memset(shield.slip_history, ' ', sizeof(shield.slip_history));

    avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUTPUT),
                            slip_add_byte, &shield.slip_msg_out);
    avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_INPUT),
                            slip_add_byte, &shield.slip_msg_in);

    if (test_enabled) {
        // disable the default stdio dump of avr_uart
        uint32_t f = 0;
        avr_ioctl(avr, AVR_IOCTL_UART_GET_FLAGS(uart), &f);
        f &= ~AVR_UART_FLAG_STDIO;
        avr_ioctl(avr, AVR_IOCTL_UART_SET_FLAGS(uart), &f);

        serial_in_irq = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_INPUT);
    } else {
        uart_pty_init(avr, &uart_pty);
        uart_pty_connect(&uart_pty, uart);
    }

    // System Hardware Description
    // mcp23008 at 0x20 & 0x21
    for(int i=0; i<2; i++) {
        i2c_mcp23008_init(avr, &shield.mcp23008[i], (0x20 + i)<<1, 0x01);
        i2c_mcp23008_attach(avr, &shield.mcp23008[i], AVR_IOCTL_TWI_GETIRQ(0));
        shield.mcp23008[i].verbose = loglevel > LOG_WARNING ? 1 : 0;
    }
    // LED_A
    shield.led[0] = 0;
    avr_irq_register_notify(
        avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('D'), PIN_LED_A),
        port_d_changed_hook, 
        NULL);
    // LED_B
    shield.led[0] = 1;
    avr_irq_register_notify(
        avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('D'), PIN_LED_B),
        port_d_changed_hook, 
        NULL);

    // BEEPER
    // - to work around simavr's lack of "phase correct PWM" support,
    //   we just watch the OCR1A register that analogWrite() sets
    avr_register_io_write(avr, OCR1A, beeper_write, NULL);
    // - analogWrite() does a digitalWrite() for values of 0 and 255
    //   so we also watch PORTB
    avr_register_io_write(avr, PORTB, beeper_write, NULL);

    memset(shield.beeper_history, '_', sizeof(shield.beeper_history));

    // Encoder v1 & v2
    button_init(avr, &encoder_v1, "Encoder v1");
    avr_connect_irq(
        encoder_v1.irq + IRQ_BUTTON_OUT,
        avr_io_getirq (avr, AVR_IOCTL_IOPORT_GETIRQ ('D'), PIN_V1)
    );
    button_init(avr, &encoder_v2, "Encoder v2");
    avr_connect_irq(
        encoder_v2.irq + IRQ_BUTTON_OUT,
        avr_io_getirq (avr, AVR_IOCTL_IOPORT_GETIRQ ('D'), PIN_V2)
    );
    // Belt Phase
    button_init(avr, &encoder_beltPhase, "Encoder v2");
    avr_connect_irq(
        encoder_beltPhase.irq + IRQ_BUTTON_OUT,
        avr_io_getirq (avr, AVR_IOCTL_IOPORT_GETIRQ ('D'), PIN_BP)
    );
    // ADC (hall sensors)
    adcbase_irq = avr_io_getirq(avr, AVR_IOCTL_ADC_GETIRQ, 0);
    avr_irq_register_notify(adcbase_irq + ADC_IRQ_OUT_TRIGGER, adcTriggerCB, NULL);

    // Start display 
    if(test_enabled) {
        int avr_thread_running = 1;
        avr_run_thread(&avr_thread_running);
    } else {
        ayab_display(argc, argv, avr_run_thread, &machine, &shield);
        uart_pty_stop(&uart_pty);
    }
}
