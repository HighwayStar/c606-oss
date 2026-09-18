#pragma once
#include <stdbool.h>

/* Ride state machine.
 *   IDLE    idle screen; nothing is recorded, statistics are frozen
 *   RIDING  data pages, track recording, statistics running
 *   PAUSED  data pages, recording and statistics paused
 * Key 2: idle -> start; riding <-> paused; long press (riding/paused) ->
 * "End ride?" dialog -> idle, track file closed. */

typedef enum { RIDE_IDLE, RIDE_RIDING, RIDE_PAUSED } ride_mode_t;
typedef void (*ride_mode_cb_t)(ride_mode_t mode);

void ride_init(ride_mode_cb_t cb);
ride_mode_t ride_mode(void);
void ride_start(void);      /* from IDLE: reset statistics, open the track file */
void ride_pause(void);
void ride_resume(void);
void ride_toggle(void);     /* start / pause / resume, as key 2 does */
void ride_end(void);        /* close the track file, back to IDLE */
bool ride_recording(void);  /* RIDING: track points are written */

/* Auto pause: feed the current speed about once a second (valid = a wheel
 * sensor or a GPS fix is delivering). Standing still for a few seconds
 * pauses the ride, moving again resumes it; a manual pause (key 2) is never
 * auto-resumed. */
void ride_speed(float kmh, bool valid);
bool ride_auto_paused(void);
