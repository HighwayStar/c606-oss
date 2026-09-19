#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Rider profile derived values, calories and training zones.
 *
 * The profile (weight, height, birth year, sex, max HR, LTHR, FTP) lives in
 * the config. Everything here is computed from it:
 *   - age, BMI, basal metabolic rate (Mifflin-St Jeor), estimated max HR
 *     (220 - age) and lactate threshold HR (89 % of max HR) when the user
 *     has not entered them,
 *   - calories burnt during the ride, integrated once a second while the
 *     ride runs (health_tick()) from the best available source, the same
 *     way the vendor firmware does it (see docs/HARDWARE.md, "Calories"):
 *       power meter   kJ / 0.99 (about 24 % metabolic efficiency),
 *       heart rate    Keytel et al. 2005 (HR, weight, age, sex),
 *       speed only    MET table for cycling by speed x weight,
 *   - heart rate zones 1..5 as a percentage of max HR (Garmin's 50/60/70/
 *     80/90 %) or of LTHR (68/85/90/95/100 %), power zones 1..7 as a
 *     percentage of FTP (Coggan), and the time spent in each zone. */

#define HR_ZONES  5          /* zone 0 = below zone 1 */
#define PWR_ZONES 7

typedef enum { HRZ_PCT_MAX = 0, HRZ_PCT_LTHR = 1 } hr_zone_mode_t;

void health_init(void);
void health_reset(void);              /* at ride start */
/* Once a second while the ride is running (not paused): integrates the
 * calories and the time in zones. */
void health_tick(void);

/* profile */
int   health_age(void);               /* years, 0 when the birth year or the clock is unknown */
int   health_max_hr(void);            /* bpm: configured, else 220 - age (or 180 without an age) */
int   health_lthr(void);              /* bpm: configured, else 89 % of max HR */
bool  health_max_hr_auto(void);
bool  health_lthr_auto(void);
float health_bmi(void);               /* 0 when weight or height is missing */
const char *health_bmi_class(float bmi);
int   health_bmr_kcal(void);          /* kcal/day, 0 when the profile is incomplete */

/* calories */
float health_kcal(void);
float health_kcal_per_h(void);        /* over the session time */
const char *health_kcal_source(void); /* "power" / "HR" / "speed" / "--" */

/* zones: bpm / W at the low edge of zone z (1..N); zone_of() returns 0..N */
int      health_hr_zone_low(int z);
int      health_hr_zone(int bpm);
int      health_pwr_zone_low(int z);
int      health_pwr_zone(int watts);
bool     health_pwr_zones_available(void);   /* FTP is set */
uint32_t health_hr_zone_ms(int z);           /* time spent in zone z (0..HR_ZONES) this ride */
uint32_t health_pwr_zone_ms(int z);
int      health_hr_zone_current(void);       /* zone of the live HR, -1 without one */
int      health_pwr_zone_current(void);
uint32_t health_zone_since_ms(void);         /* time in the current HR zone */
