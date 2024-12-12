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
#include <libgen.h>
#include <string.h>
#include <ctype.h>

#include "sim_avr.h"
#include "sim_time.h"
#include "sim_core.h"
#include "avr_ioport.h"
#include "avr_adc.h"
#include "avr_twi.h"
#include "avr_extint.h"
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

uart_pty_t uart_pty;

machine_t machine;
shield_t shield;
button_t encoder_v1, encoder_v2;
button_t encoder_beltPhase;
avr_irq_t *adcbase_irq;

extern avr_kind_t *avr_kind[];

// analogWrite() stores the PWM duty cycle for pin 9 in that register
#define OCR1A 0x88
#define PORTB 0x25

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
	 "       [--help|-h|-?]      Display this usage message and exit\n"
	 "       [--list-cores]      List all supported AVR cores and exit\n"
	 "       [-v]                Raise verbosity level (can be passed more than once)\n"
	 "       [--freq|-f <freq>]  Sets the frequency for an .hex firmware\n"
	 "       [--mcu|-m <device>] Sets the MCU type for an .hex firmware\n"
	 "       [--gdb|-g [<port>]] Listen for gdb connection on <port> (default 1234)\n"
	 "       [--output|-o <file>] VCD file to save signal traces\n"
	 "       [--start-vcd|-s     Start VCD output from reset\n"
	 "       [--pc-trace|-p      Add PC to VCD traces\n"
     "       [--machine-trace]   Add Machine states to VCD traces\n"
     "       [--machine <machine>]   Select KH910/KH930/KH270 machine (default=KH910)\n"
     "       [--carriage <carriage>] Select K/L/G carriage (default=K)\n"
     "       [--beltphase <phase>]   Select Regular/Shifted (default=Regular)\n"
     "       [--startside <side>]    Select Left/Right side to start (default=Left)\n"
	 "       <firmware>          HEX or ELF file to load (can include debugging syms)\n"
     "\n");
	exit(1);
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
		} else if (argv[pi][0] != '-') {
            uint32_t loadBase = AVR_SEGMENT_OFFSET_FLASH;
			sim_setup_firmware(argv[pi], loadBase, &firmware, argv[0]);
            printf ("%s loaded (f=%d mmcu=%s)\n", argv[pi], (int) firmware.frequency, firmware.mmcu);
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
    unsigned encoder_phase = 0;

    char needles[machine.num_needles];
    memset(needles, '.', machine.num_needles);

	while (*run && (state != cpu_Done) && (state != cpu_Crashed))
    {
        // Limit event/interrupt rate towards avr
        if (avr_cycles_to_usec(avr, avr->cycle - lastChange)  > 10000) {
            lastChange = avr->cycle;

            enum event_type event;
            int value;
            if (queue_pop(&event_queue, &event, &value))
            {
                unsigned new_phase = encoder_phase;
                switch (event)
                {
                    case RESET_ARDUINO:
                        fprintf(stderr, "Resetting Arduino\n");
                        avr_reset(avr);
                        break;
                    case CARRIAGE_LEFT:
                        new_phase = (encoder_phase-1)%64;
                        if ((new_phase%4) == 3) {
                            if (machine.carriage.position > -MARGIN_NEEDLES) {
                                machine.carriage.position--;
                            } else {
                                machine.carriage.position = -MARGIN_NEEDLES;
                                new_phase = encoder_phase;
                            }
                        }
                        break;
                    case CARRIAGE_RIGHT:
                        new_phase = (encoder_phase+1)%64;
                        if ((new_phase%4) == 0) {
                            if (machine.carriage.position < (machine.num_needles + MARGIN_NEEDLES - 1)) {
                                machine.carriage.position++;
                            } else {
                                machine.carriage.position = (machine.num_needles + MARGIN_NEEDLES - 1);
                                new_phase = encoder_phase;
                            }
                        }
                        break;
                    case VCD_DUMP:
                        vcd_enabled = ! vcd_enabled;
                        vcd_trace_enable(vcd_enabled);
                        continue;
                        break;
                    default:
                        fprintf(stderr, "Unexpect event from graphic thread\n");
                        break;
                }
                encoder_phase = new_phase;

                machine.hall_left = 1650;
                machine.hall_right = 1650;
                uint16_t solenoid_states = (shield.mcp23008[1].reg[MCP23008_REG_OLAT] << 8) + shield.mcp23008[0].reg[MCP23008_REG_OLAT]; 
                int selected_needle;
                int select_offset = 0;
                switch (machine.carriage.type) {
                    case KNIT:
                        // Handle hall sensors
                        if (machine.carriage.position == 0) {
                            machine.hall_left = 2200; //TBC North
                        } else if (machine.carriage.position == (machine.num_needles - 1)) {
                            machine.hall_right = 2200; //TBC North
                            if(machine.type == KH910) { // Shield error
                                machine.hall_right = 0; // Digital low
                            }
                        }
                        // Handle solenoids
                        select_offset = 24;
                        if (event == CARRIAGE_RIGHT) {
                            select_offset = -24;
                        }
                        break;
                    case LACE:
                        if (machine.carriage.position == 0) {
                            machine.hall_left = 100; //TBC South
                        } else if (machine.carriage.position == (machine.num_needles - 1)) {
                            machine.hall_right = 100; //TBC South
                            if(machine.type == KH910) { // Shield error
                                machine.hall_right = 1650; // HighZ
                            }
                        }
                        // Handle solenoids
                        select_offset = 12;
                        if (event == CARRIAGE_RIGHT) {
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
                            case 187:
                            case 211:
                                machine.hall_right = 100; //TBC South
                                if(machine.type == KH910) { // Shield error
                                    machine.hall_right = 1650; // HighZ
                                }
                                break;
                            case 189:
                            case 209:
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
                        switch (machine.carriage.position) {
                            case -6:
                            case  0:
                                machine.hall_left = 2200; //TBC North
                                break;
                            case 111:
                            case 117:
                                machine.hall_right = 2200; //TBC North
                                break;
                            default:
                                break;
                        }
                        // Handle solenoids
                        select_offset = 15;
                        if (event == CARRIAGE_RIGHT) {
                            select_offset = -15;
                        }
                        break;
                    default:
                        fprintf(stderr, "Unexpected carriage type (%d)\n", machine.carriage.type);
                        break;
                }

                selected_needle = machine.carriage.position + select_offset;
                if ((selected_needle < machine.num_needles) && (selected_needle >= 0)) {
                    int solenoid_index;
                    switch (machine.type) {
                        case KH270:
                            solenoid_index = selected_needle + 4;
                            // K270 solenoid to needle mapping is direction-dependent
                            if (event == CARRIAGE_LEFT) {
                                solenoid_index += machine.num_solenoids >> 1;
                            }
                            // 12 solenoids mapped to position [3-14]
                            solenoid_index = 3 + solenoid_index % machine.num_solenoids; 
                            break;
                        default:
                            solenoid_index = selected_needle;
                            // Solenoid to needle mapping is belt phase-dependent
                            if (machine.belt_phase == SHIFTED) {
                                solenoid_index += machine.num_solenoids >> 1;
                            }
                            // LACE solenoid to needle mapping is direction-dependent
                            if ((machine.carriage.type == LACE) && (event == CARRIAGE_LEFT)) {
                                solenoid_index += machine.num_solenoids >> 1;
                            }
                            solenoid_index = solenoid_index % machine.num_solenoids; 
                            break;
                    }

                    needles[selected_needle] = solenoid_states & (1<< solenoid_index) ? '.' : '|';
                }

                avr_raise_irq(encoder_v2.irq + IRQ_BUTTON_OUT, (phase_map[encoder_phase % 4] & 1) ? 1 : 0);
                avr_raise_irq(encoder_v1.irq + IRQ_BUTTON_OUT, (phase_map[encoder_phase % 4] & 2) ? 1 : 0);

                // Belt phase signal is slightly early compared to encoder phase
                int beltPhase = ((encoder_phase + BELT_PHASE_ADVANCE) % (machine.num_solenoids * 4) > (machine.num_solenoids * 2)) ? 0 : 1;
                avr_raise_irq(encoder_beltPhase.irq + IRQ_BUTTON_OUT, beltPhase);

                uint16_t previous_solenoid_states = machine.solenoid_states;
                machine.solenoid_states = solenoid_states;

                fprintf(stderr, "S=[");
                for (int i=(machine.num_solenoids == 12 ? 3 : 0); i<=(machine.num_solenoids == 12 ? 14 : 15);i++) {
                    if ((solenoid_states ^ previous_solenoid_states) & (1 << i)) {
                        fprintf(stderr, "\x1b[7m");
                    }
                    fprintf(stderr, "%c\x1b[0m", solenoid_states & (1<<i) ? '.' : '|');
                }
                fprintf(stderr, "], Ph = %d, Pos = %3.2f, BP = %d, Sensors = (%4d, %4d) B=%.*s\n",
                        encoder_phase,
                        machine.carriage.position + (encoder_phase % 4) / 4.0,
                        beltPhase, machine.hall_left, machine.hall_right, (int)sizeof(shield.beeper_history), shield.beeper_history);
                if (shield.beeper_history[sizeof(shield.beeper_history) - 1] == ' ')
                    beeper_history_add(' ');
                fprintf(stderr, "%.*s\n", (int)sizeof(shield.slip_history), shield.slip_history);

                char needle_buffer[MARGIN_NEEDLES + machine.num_needles + MARGIN_NEEDLES];
                char carriage_buffer[MARGIN_NEEDLES + machine.num_needles + MARGIN_NEEDLES];
                memset(carriage_buffer, ' ', sizeof(carriage_buffer));
                memset(needle_buffer, ' ', sizeof(needle_buffer));

                if ((selected_needle >= -MARGIN_NEEDLES) && (selected_needle < machine.num_needles + MARGIN_NEEDLES)) {
                    carriage_buffer[selected_needle + MARGIN_NEEDLES] = 'x';
                }
                if ((machine.carriage.position >= -MARGIN_NEEDLES) && (machine.carriage.position < machine.num_needles + MARGIN_NEEDLES)) {
                    carriage_buffer[machine.carriage.position + MARGIN_NEEDLES] = '^';
                }

                int half_num_needles = machine.num_needles / 2;
                for (int i=0; i < machine.num_needles; i++) {
                    needle_buffer[MARGIN_NEEDLES + i] = needles[i];
                }
                fprintf(stderr, "<- %.*s\n   %.*s\n",
                        half_num_needles + MARGIN_NEEDLES, needle_buffer,
                        half_num_needles + MARGIN_NEEDLES, carriage_buffer);
                fprintf(stderr, "-> %.*s\n   %.*s\n",
                        half_num_needles + MARGIN_NEEDLES, needle_buffer + half_num_needles + MARGIN_NEEDLES,
                        half_num_needles + MARGIN_NEEDLES, carriage_buffer + half_num_needles + MARGIN_NEEDLES);

                // Trigger IRQ for machine internal data
                if (trace_machine) {
                    avr_raise_irq(&vcd_irq_hall_left, machine.hall_left);
                    avr_raise_irq(&vcd_irq_hall_right, machine.hall_right);
                    avr_raise_irq(&vcd_irq_carriage_position, machine.carriage.position);
                    avr_raise_irq(&vcd_irq_solenoids_state, solenoid_states);
                }
            }
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

static void slip_print(slipmsg_t *msg);

static void slip_history_add(char c) {
	memmove(shield.slip_history, shield.slip_history + 1, sizeof(shield.slip_history) - 1);
	shield.slip_history[sizeof(shield.slip_history) - 1] = c;
}

static void slip_print(slipmsg_t *msg) {
	slip_history_add(msg->prefix);
	slip_history_add(' ');
	for (int i = 0; i < msg->len; i++)
	{
		if (isprint(msg->buf[i]))
		{
			slip_history_add(msg->buf[i]);
		}
		else
		{
			char s[5];
			snprintf(s, sizeof(s), "\\%02x", msg->buf[i]);
			slip_history_add(s[0]);
			slip_history_add(s[1]);
			slip_history_add(s[2]);
		}
	};
	slip_history_add(' ');
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
            slip_print(msg);
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

	printf (
        "---------------------------------------------------------\n"
        "AYAB shield emulation \n"
        "- Use left/right arrows to move the carriage\n"
        "- 'v' to start/stop VCD traces"
        "- 'q' or ESC to quit\n"
        "----------------------------------------------------------\n"
    );

    parse_arguments(argc, argv);

    if (machine.type == KH270) {
        machine.num_needles = 112;
        machine.num_solenoids = 12;
        machine.carriage.type = KNIT270;
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
            machine.carriage.position = 15;
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
	uart_pty_init(avr, &uart_pty);
	uart_pty_connect(&uart_pty, '0');

    // Serial (SLIP) monitor
    shield.slip_msg_in.prefix = '<';
    shield.slip_msg_out.prefix = '>';
	memset(shield.slip_history, ' ', sizeof(shield.slip_history));

    avr_irq_register_notify(uart_pty.irq + IRQ_UART_PTY_BYTE_IN, slip_add_byte, &shield.slip_msg_in);
    avr_irq_register_notify(uart_pty.irq + IRQ_UART_PTY_BYTE_OUT, slip_add_byte, &shield.slip_msg_out);

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
    printf( "\nsimavr launching\n");

    ayab_display(argc, argv, avr_run_thread, &machine, &shield);

    uart_pty_stop(&uart_pty);
    printf( "\nsimavr done:\n");
}
