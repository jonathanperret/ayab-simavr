#ifndef __AYAB_MACHINE__
#define __AYAB_MACHINE__

#define HALL_VALUE_NORTH 700
#define HALL_VALUE_IDLE  400
#define HALL_VALUE_SOUTH 100

#define BELT_PHASE_ADVANCE 8

#define MARGIN_NEEDLES 28
#define MAX_NEEDLES 200

enum side {LEFT, RIGHT};
enum direction {LEFTWARDS, RIGHTWARDS};
enum machine_type {KH910, KH930, KH270};
enum carriage_type {KNIT, LACE, GARTER, KNIT270};
enum belt_phase_type {REGULAR, SHIFTED};

typedef struct {
    enum carriage_type type;
    int position;
    enum direction direction;
    int selected_needle;
} carriage_t;

typedef struct {
    enum machine_type type;
    enum side start_side;
    int sensor_radius;
    int num_needles;
    int num_solenoids;
    int belt_phase;

    carriage_t carriage;
    unsigned encoder_phase;
    int belt_phase_signal;
    int hall_left, hall_right;
    char needles[MAX_NEEDLES];
    uint16_t solenoid_states;
    uint16_t previous_solenoid_states;
    uint16_t armature_states;
    uint16_t previous_armature_states;
    int dirty;
} machine_t;

#endif
