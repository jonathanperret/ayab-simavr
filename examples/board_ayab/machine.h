#ifndef __AYAB_MACHINE__
#define __AYAB_MACHINE__

#define HALL_VALUE_NORTH 700
#define HALL_VALUE_IDLE  400
#define HALL_VALUE_SOUTH 100

#define BELT_PHASE_ADVANCE 8

#define MARGIN_NEEDLES 28

enum side {LEFT, RIGHT};
enum machine_type {KH910, KH930, KH270};
enum carriage_type {KNIT, LACE, GARTER, KNIT270};
enum belt_phase_type {REGULAR, SHIFTED};

typedef struct {
    enum carriage_type type;
    int position;
} carriage_t;

typedef struct {
    enum machine_type type;
    enum side start_side;
    int num_needles;
    int num_solenoids;
    carriage_t carriage;
    int belt_phase;
    int hall_left, hall_right;
    uint16_t solenoid_states;
    uint16_t armature_states;
    uint16_t previous_armature_states;
} machine_t;

#endif
