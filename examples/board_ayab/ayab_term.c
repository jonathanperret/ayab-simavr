#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <termios.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>

#include "ayab_term.h"
#include "queue.h"

enum
{
    KEY_LEFT,
    KEY_RIGHT
};

struct timespec term_lastkey;
int key_down = 0;
#define KEYUP_TIMEOUT 0 // 1e8

struct termios oldt;

pthread_t avr_thread;
int avr_thread_running;

event_queue_t event_queue = {.index_read = 0, .index_write = 0};

shield_t *_shield;
machine_t *_machine;

int carriageSpeed = 0;

const int ACCELERATION = 1;

const int MINSPEED = 4;
const int MAXSPEED = 64;

void term_teardown()
{
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
}

void term_setup()
{
    tcgetattr(STDIN_FILENO, &oldt);
    struct termios newt = oldt;
    // newt.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
    //                 | INLCR | IGNCR | ICRNL | IXON);
    // newt.c_oflag &= ~OPOST;
    // newt.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    newt.c_lflag &= ~ICANON;
    newt.c_lflag &= ~ECHO;
    // newt.c_cflag &= ~(CSIZE | PARENB);
    // newt.c_cflag |= CS8;

    // MIN=0, TIME>0: read() will timeout if no bytes received
    newt.c_cc[VMIN] = 0;
    newt.c_cc[VTIME] = 1;

    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    atexit(term_teardown);

    // fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
}

void move_carriage()
{
    if (carriageSpeed == 0)
        return;
    // fprintf(stderr, "speed=%d\n", carriageSpeed);
    for (int i = 0; i < abs(carriageSpeed) && !is_queue_full(&event_queue); i++)
        queue_push(&event_queue, carriageSpeed > 0 ? CARRIAGE_RIGHT : CARRIAGE_LEFT, 0);
}

void keyCB(unsigned char key)
{
    switch (key)
    {
    case 0x1b:
    case 'q':
        // Terminate the AVR thread ...
        avr_thread_running = 0;
        pthread_join(avr_thread, NULL);
        // ... and exit
        exit(0);
        break;

    case 'r':
        queue_push(&event_queue, RESET_ARDUINO, 0);
        break;

    case 'v':
        queue_push(&event_queue, VCD_DUMP, 0);
        break;
    default:
        break;
    }
}

void term_read()
{
    char buf[10];
    int nread = read(STDIN_FILENO, buf, 10);
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    if (nread > 0)
    {
        key_down = 1;
        // for(int i=0;i <nread; i++)
        //     fprintf(stderr, "%02x ", buf[i]);
        // fprintf(stderr, "\n");

        if (memcmp(buf, "\x1b\x5b\x43", 3) == 0)
        {
            carriageSpeed += ACCELERATION;
            if (carriageSpeed < MINSPEED)
                carriageSpeed = MINSPEED;
            if (carriageSpeed > MAXSPEED)
                carriageSpeed = MAXSPEED;
        }
        else if (memcmp(buf, "\x1b\x5b\x44", 3) == 0)
        {
            carriageSpeed -= ACCELERATION;
            if (carriageSpeed > -MINSPEED)
                carriageSpeed = -MINSPEED;
            if (carriageSpeed < -MAXSPEED)
                carriageSpeed = -MAXSPEED;
        } else
        {
            keyCB(buf[0]);
        }
        term_lastkey = now;
    }
    else
    {
        double dt = (now.tv_sec * 1e9 + now.tv_nsec - (term_lastkey.tv_sec * 1e9 + term_lastkey.tv_nsec));
        if (key_down &&
            dt > KEYUP_TIMEOUT)
        {
            key_down = 0;
            // fprintf(stderr, "dt=%g -> keyup\n", dt);
            carriageSpeed = 0;
        }
    }
}

void ayab_display(int argc, char *argv[], void *(*avr_run_thread)(void *), machine_t *machine, shield_t *shield)
{
    _shield = shield;
    _machine = machine;

    // Run avr thread
    avr_thread_running = 1;
    pthread_create(&avr_thread, NULL, avr_run_thread, &avr_thread_running);

    term_setup();
    while (1)
    {
        term_read();
        move_carriage();
        // while(!is_queue_empty(&event_queue))
        //     usleep(1000);
    }
}
