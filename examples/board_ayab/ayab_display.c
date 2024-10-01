#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <termios.h>
#include "ayab_display.h"
#include "queue.h"

pthread_t avr_thread;
int avr_thread_running;

event_queue_t event_queue = {.index_read = 0, .index_write = 0};

shield_t *_shield;
machine_t *_machine;

struct termios orig_termios;

// Function to restore the original terminal settings
void term_reset()
{
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

void term_init()
{
    struct termios raw;

    // Get current terminal attributes
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(term_reset); // Ensure original settings are restored when the program exits

    // Modify the terminal attributes for raw mode
    raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO); // Disable canonical mode and echoing

    // Set the terminal to raw mode
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void term_loop()
{
    char ch;

    while (read(STDIN_FILENO, &ch, 1) == 1 && ch != 'q')
    {
        switch (ch)
        {
        case 'h':
            queue_push(&event_queue, CARRIAGE_LEFT, 0);
            break;
        case 'l':
            queue_push(&event_queue, CARRIAGE_RIGHT, 0);
            break;
        default:
            printf("Pressed: %d\n", ch);
            break;
        }
    }
}

void ayab_display(int argc, char *argv[], void *(*avr_run_thread)(void *), machine_t *machine, shield_t *shield)
{

    _shield = shield;
    _machine = machine;

    term_init();

    // Run avr thread
    avr_thread_running = 1;
    pthread_create(&avr_thread, NULL, avr_run_thread, &avr_thread_running);

    term_loop();

    // Terminate the AVR thread ...
    avr_thread_running = 0;
    pthread_join(avr_thread, NULL);
}
