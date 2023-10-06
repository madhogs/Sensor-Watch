/*
 * MIT License
 *
 * Copyright (c) 2022 Joey Castillo
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Sunrise/sunset calculations are public domain code by Paul Schlyter, December 1992
 *
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sunrise_sunset_face.h"
#include "watch.h"
#include "watch_utility.h"
#include "sunriset.h"

#if __EMSCRIPTEN__
#include <emscripten.h>
#include <time.h>
#endif

static void _sunrise_sunset_set_expiration(sunrise_sunset_state_t *state, watch_date_time next_rise_set) {
    uint32_t timestamp = watch_utility_date_time_to_unix_time(next_rise_set, 0);
    state->rise_set_expires = watch_utility_date_time_from_unix_time(timestamp + 60, 0);
}

static void _sunrise_sunset_face_update(movement_settings_t *settings, sunrise_sunset_state_t *state) {
    char buf[14];
    double rise, set, minutes, seconds;
    bool show_next_match = false;
    movement_location_t movement_location = (movement_location_t) watch_get_backup_data(1);

    if (movement_location.reg == 0) {
        watch_display_string("RI  no Loc", 0);
        return;
    }

    watch_date_time date_time = watch_rtc_get_date_time(); // the current local date / time
    watch_date_time utc_now = watch_utility_date_time_convert_zone(date_time, movement_timezone_offsets[settings->bit.time_zone] * 60, 0); // the current date / time in UTC
    watch_date_time scratch_time; // scratchpad, contains different values at different times
    scratch_time.reg = utc_now.reg;

    // Weird quirky unsigned things were happening when I tried to cast these directly to doubles below.
    // it looks redundant, but extracting them to local int16's seemed to fix it.
    int16_t lat_centi = (int16_t)movement_location.bit.latitude;
    int16_t lon_centi = (int16_t)movement_location.bit.longitude;

    double lat = (double)lat_centi / 100.0;
    double lon = (double)lon_centi / 100.0;

    // sunriset returns the rise/set times as signed decimal hours in UTC.
    // this can mean hours below 0 or above 31, which won't fit into a watch_date_time struct.
    // to deal with this, we set aside the offset in hours, and add it back before converting it to a watch_date_time.
    double hours_from_utc = ((double)movement_timezone_offsets[settings->bit.time_zone]) / 60.0;

    // we loop twice because if it's after sunset today, we need to recalculate to display values for tomorrow.
    for(int i = 0; i < 2; i++) {
        uint8_t result = sun_rise_set(scratch_time.unit.year + WATCH_RTC_REFERENCE_YEAR, scratch_time.unit.month, scratch_time.unit.day, lon, lat, &rise, &set);

        if (result != 0) {
            watch_clear_colon();
            watch_clear_indicator(WATCH_INDICATOR_PM);
            watch_clear_indicator(WATCH_INDICATOR_24H);
            sprintf(buf, "%s%2d none ", (result == 1) ? "SE" : "rI", scratch_time.unit.day);
            watch_display_string(buf, 0);
            return;
        }

        watch_set_colon();
        if (settings->bit.clock_mode_24h) watch_set_indicator(WATCH_INDICATOR_24H);

        rise += hours_from_utc;
        set += hours_from_utc;

        minutes = 60.0 * fmod(rise, 1);
        seconds = 60.0 * fmod(minutes, 1);
        scratch_time.unit.hour = floor(rise);
        if (seconds < 30) scratch_time.unit.minute = floor(minutes);
        else scratch_time.unit.minute = ceil(minutes);

        if (scratch_time.unit.minute == 60) {
            scratch_time.unit.minute = 0;
            scratch_time.unit.hour = (scratch_time.unit.hour + 1) % 24;
        }

        if (date_time.reg < scratch_time.reg) _sunrise_sunset_set_expiration(state, scratch_time);

        if (date_time.reg < scratch_time.reg || show_next_match) {
            if (state->rise_index == 0 || show_next_match) {
                if (!settings->bit.clock_mode_24h) {
                    if (watch_utility_convert_to_12_hour(&scratch_time)) watch_set_indicator(WATCH_INDICATOR_PM);
                    else watch_clear_indicator(WATCH_INDICATOR_PM);
                }
                sprintf(buf, "rI%2d%2d%02d %c", scratch_time.unit.day, scratch_time.unit.hour, scratch_time.unit.minute, 47);
                watch_display_string(buf, 0);
                return;
            } else {
                show_next_match = true;
            }
        }

        minutes = 60.0 * fmod(set, 1);
        seconds = 60.0 * fmod(minutes, 1);
        scratch_time.unit.hour = floor(set);
        if (seconds < 30) scratch_time.unit.minute = floor(minutes);
        else scratch_time.unit.minute = ceil(minutes);

        if (scratch_time.unit.minute == 60) {
            scratch_time.unit.minute = 0;
            scratch_time.unit.hour = (scratch_time.unit.hour + 1) % 24;
        }

        if (date_time.reg < scratch_time.reg) _sunrise_sunset_set_expiration(state, scratch_time);

        if (date_time.reg < scratch_time.reg || show_next_match) {
            if (state->rise_index == 0 || show_next_match) {
                if (!settings->bit.clock_mode_24h) {
                    if (watch_utility_convert_to_12_hour(&scratch_time)) watch_set_indicator(WATCH_INDICATOR_PM);
                    else watch_clear_indicator(WATCH_INDICATOR_PM);
                }
                sprintf(buf, "SE%2d%2d%02d %c", scratch_time.unit.day, scratch_time.unit.hour, scratch_time.unit.minute, 47);
                watch_display_string(buf, 0);
                return;
            } else {
                show_next_match = true;
            }
        }

        // it's after sunset. we need to display sunrise/sunset for tomorrow.
        uint32_t timestamp = watch_utility_date_time_to_unix_time(utc_now, 0);
        timestamp += 86400;
        scratch_time = watch_utility_date_time_from_unix_time(timestamp, 0);
    }
}

static int16_t _sunrise_sunset_face_latlon_from_struct(sunrise_sunset_lat_lon_settings_t val) {
    int16_t retval = (val.sign ? -1 : 1) *
                        (
                            val.hundreds * 10000 +
                            val.tens * 1000 +
                            val.ones * 100 +
                            val.tenths * 10 +
                            val.hundredths
                        );
    return retval;
}

static sunrise_sunset_lat_lon_settings_t _sunrise_sunset_face_struct_from_latlon(int16_t val) {
    sunrise_sunset_lat_lon_settings_t retval;

    retval.sign = val < 0;
    val = abs(val);
    retval.hundredths = val % 10;
    val /= 10;
    retval.tenths = val % 10;
    val /= 10;
    retval.ones = val % 10;
    val /= 10;
    retval.tens = val % 10;
    val /= 10;
    retval.hundreds = val % 10;

    return retval;
}

static void _sunrise_sunset_face_update_location_register(sunrise_sunset_state_t *state) {
    if (state->location_changed) {
        movement_location_t movement_location;
        int16_t lat = _sunrise_sunset_face_latlon_from_struct(state->working_latitude);
        int16_t lon = _sunrise_sunset_face_latlon_from_struct(state->working_longitude);
        movement_location.bit.latitude = lat;
        movement_location.bit.longitude = lon;
        watch_store_backup_data(movement_location.reg, 1);
        state->location_changed = false;
    }
}

static void _sunrise_sunset_face_update_settings_display(movement_event_t event, sunrise_sunset_state_t *state) {
    char buf[12];

    switch (state->page) {
        case 0:
            return;
        case 1:
            sprintf(buf, "LA  %c %04d", state->working_latitude.sign ? '-' : '+', abs(_sunrise_sunset_face_latlon_from_struct(state->working_latitude)));
            break;
        case 2:
            sprintf(buf, "LO  %c%05d", state->working_longitude.sign ? '-' : '+', abs(_sunrise_sunset_face_latlon_from_struct(state->working_longitude)));
            break;
    }
    if (event.subsecond % 2) {
        buf[state->active_digit + 4] = ' ';
    }
    watch_display_string(buf, 0);
}

static void _sunrise_sunset_face_advance_digit(sunrise_sunset_state_t *state) {
    state->location_changed = true;
    switch (state->page) {
        case 1: // latitude
            switch (state->active_digit) {
                case 0:
                    state->working_latitude.sign++;
                    break;
                case 1:
                    // we skip this digit
                    break;
                case 2:
                    state->working_latitude.tens = (state->working_latitude.tens + 1) % 10;
                    if (abs(_sunrise_sunset_face_latlon_from_struct(state->working_latitude)) > 9000) {
                        // prevent latitude from going over ±90.
                        // TODO: perform these checks when advancing the digit?
                        state->working_latitude.ones = 0;
                        state->working_latitude.tenths = 0;
                        state->working_latitude.hundredths = 0;
                    }
                    break;
                case 3:
                    state->working_latitude.ones = (state->working_latitude.ones + 1) % 10;
                    if (abs(_sunrise_sunset_face_latlon_from_struct(state->working_latitude)) > 9000) state->working_latitude.ones = 0;
                    break;
                case 4:
                    state->working_latitude.tenths = (state->working_latitude.tenths + 1) % 10;
                    if (abs(_sunrise_sunset_face_latlon_from_struct(state->working_latitude)) > 9000) state->working_latitude.tenths = 0;
                    break;
                case 5:
                    state->working_latitude.hundredths = (state->working_latitude.hundredths + 1) % 10;
                    if (abs(_sunrise_sunset_face_latlon_from_struct(state->working_latitude)) > 9000) state->working_latitude.hundredths = 0;
                    break;
            }
            break;
        case 2: // longitude
            switch (state->active_digit) {
                case 0:
                    state->working_longitude.sign++;
                    break;
                case 1:
                    state->working_longitude.hundreds = (state->working_longitude.hundreds + 1) % 10;
                    if (abs(_sunrise_sunset_face_latlon_from_struct(state->working_longitude)) > 18000) {
                        // prevent longitude from going over ±180
                        state->working_longitude.tens = 8;
                        state->working_longitude.ones = 0;
                        state->working_longitude.tenths = 0;
                        state->working_longitude.hundredths = 0;
                    }
                    break;
                case 2:
                    state->working_longitude.tens = (state->working_longitude.tens + 1) % 10;
                    if (abs(_sunrise_sunset_face_latlon_from_struct(state->working_longitude)) > 18000) state->working_longitude.tens = 0;
                    break;
                case 3:
                    state->working_longitude.ones = (state->working_longitude.ones + 1) % 10;
                    if (abs(_sunrise_sunset_face_latlon_from_struct(state->working_longitude)) > 18000) state->working_longitude.ones = 0;
                    break;
                case 4:
                    state->working_longitude.tenths = (state->working_longitude.tenths + 1) % 10;
                    if (abs(_sunrise_sunset_face_latlon_from_struct(state->working_longitude)) > 18000) state->working_longitude.tenths = 0;
                    break;
                case 5:
                    state->working_longitude.hundredths = (state->working_longitude.hundredths + 1) % 10;
                    if (abs(_sunrise_sunset_face_latlon_from_struct(state->working_longitude)) > 18000) state->working_longitude.hundredths = 0;
                    break;
            }
            break;
    }
}

void sunrise_sunset_face_setup(movement_settings_t *settings, uint8_t watch_face_index, void ** context_ptr) {
    (void) settings;
    (void) watch_face_index;
    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(sunrise_sunset_state_t));
        memset(*context_ptr, 0, sizeof(sunrise_sunset_state_t));
    }

    // Emulator only: Seed random number generator
    #if __EMSCRIPTEN__
    srand(time(NULL));
    #endif
}

void sunrise_sunset_face_activate(movement_settings_t *settings, void *context) {
    (void) settings;
    if (watch_tick_animation_is_running()) watch_stop_tick_animation();

#if __EMSCRIPTEN__
    int16_t browser_lat = EM_ASM_INT({
        return lat;
    });
    int16_t browser_lon = EM_ASM_INT({
        return lon;
    });
    if ((watch_get_backup_data(1) == 0) && (browser_lat || browser_lon)) {
        movement_location_t browser_loc;
        browser_loc.bit.latitude = browser_lat;
        browser_loc.bit.longitude = browser_lon;
        watch_store_backup_data(browser_loc.reg, 1);
    }
#endif


    sunrise_sunset_state_t *state = (sunrise_sunset_state_t *)context;
    movement_location_t movement_location = (movement_location_t) watch_get_backup_data(1);
    state->working_latitude = _sunrise_sunset_face_struct_from_latlon(movement_location.bit.latitude);
    state->working_longitude = _sunrise_sunset_face_struct_from_latlon(movement_location.bit.longitude);
    state->rolled_value = 0;
}

static void _update_moon_phase(movement_settings_t *settings) {
    float lunar_days = 29.53058770576;
    double lunar_seconds = lunar_days * 24 * 60 * 60;
    uint32_t first_moon = 947182440; // Saturday, 6 January 2000 18:14:00 in unix epoch time
    uint8_t num_phases = 8;
    float phase_changes[] = {0, 1, 6.38264692644, 8.38264692644, 13.76529385288, 15.76529385288, 21.14794077932, 23.14794077932, 28.53058770576, 29.53058770576};

    char buf[11];
    watch_date_time date_time = watch_rtc_get_date_time();
    uint32_t now = watch_utility_date_time_to_unix_time(date_time, movement_timezone_offsets[settings->bit.time_zone] * 60);
    date_time = watch_utility_date_time_from_unix_time(now, movement_timezone_offsets[settings->bit.time_zone] * 60);
    double currentfrac = fmod(now - first_moon, lunar_seconds) / lunar_seconds;
    double currentday = currentfrac * lunar_days;
    uint8_t phase_index = 0;

    for(phase_index = 0; phase_index <= num_phases; phase_index++) {
        if (currentday > phase_changes[phase_index] && currentday <= phase_changes[phase_index + 1]) break;
    }
    
    watch_clear_display();
    watch_display_string(" ", 0);
    switch (phase_index) {
        case 0:
        case 8:
            sprintf(buf, "%2d Neu  ", date_time.unit.day);
            break;
        case 1:
            sprintf(buf, "%2dCresnt", date_time.unit.day);
            watch_set_pixel(2, 13);
            watch_set_pixel(2, 15);
            if (currentfrac > 0.125) watch_set_pixel(1, 13);
            break;
        case 2:
            sprintf(buf, "%2d 1st q", date_time.unit.day);
            watch_set_pixel(2, 13);
            watch_set_pixel(2, 15);
            watch_set_pixel(1, 13);
            watch_set_pixel(1, 14);
            break;
        case 3:
            sprintf(buf, "%2d Gibb ", date_time.unit.day);
            watch_set_pixel(2, 13);
            watch_set_pixel(2, 15);
            watch_set_pixel(1, 14);
            watch_set_pixel(1, 13);
            watch_set_pixel(1, 15);
            break;
        case 4:
            sprintf(buf, "%2d FULL ", date_time.unit.day);
            watch_set_pixel(2, 13);
            watch_set_pixel(2, 15);
            watch_set_pixel(1, 14);
            watch_set_pixel(2, 14);
            watch_set_pixel(1, 15);
            watch_set_pixel(0, 14);
            watch_set_pixel(0, 13);
            watch_set_pixel(1, 13);
            break;
        case 5:
            sprintf(buf, "%2d Gibb ", date_time.unit.day);
            watch_set_pixel(1, 14);
            watch_set_pixel(2, 14);
            watch_set_pixel(1, 15);
            watch_set_pixel(0, 14);
            watch_set_pixel(0, 13);
            break;
        case 6:
            sprintf(buf, "%2d 3rd q", date_time.unit.day);
            watch_set_pixel(1, 14);
            watch_set_pixel(2, 14);
            watch_set_pixel(0, 14);
            watch_set_pixel(0, 13);
            break;
        case 7:
            sprintf(buf, "%2dCresnt", date_time.unit.day);
            watch_set_pixel(0, 14);
            watch_set_pixel(0, 13);
            if (currentfrac < 0.875) watch_set_pixel(2, 14);
            break;
    }
    watch_display_string(buf, 2);
}

static void generate_random_number(sunrise_sunset_state_t *state) {
    uint8_t dice_sides = 100;
    // Emulator: use rand. Hardware: use arc4random.
    #if __EMSCRIPTEN__
    state->rolled_value = rand() % dice_sides + 1;
    #else
    state->rolled_value = arc4random_uniform(dice_sides) + 1;
    #endif

    state->is_rolling = true;
    movement_request_tick_frequency(8);
}

static void display_dice_roll(sunrise_sunset_state_t *state) {
    char buf[8];
    sprintf(buf, "%2d", state->rolled_value);
    watch_display_string(buf, 8);
    movement_request_tick_frequency(1);
}

static void display_dice_roll_animation(sunrise_sunset_state_t *state) {
    if (state->is_rolling) {
        if (state->animation_frame == 0) {
            watch_display_string("  ", 8);
            watch_set_pixel(1, 4);
            watch_set_pixel(1, 6);
            state->animation_frame = 1;
        } else if (state->animation_frame == 1) {
            watch_clear_pixel(1, 4);
            watch_clear_pixel(1, 6);
            watch_set_pixel(2, 4);
            watch_set_pixel(0, 6);
            state->animation_frame = 2;
        } else if (state->animation_frame == 2) {
            watch_clear_pixel(2, 4);
            watch_clear_pixel(0, 6);
            watch_set_pixel(2, 5);
            watch_set_pixel(0, 5);
            state->animation_frame = 3;
        } else if (state->animation_frame == 3) {
            state->animation_frame = 0;
            state->is_rolling = false;
            display_dice_roll(state);
        }
    }
}

bool sunrise_sunset_face_loop(movement_event_t event, movement_settings_t *settings, void *context) {
    sunrise_sunset_state_t *state = (sunrise_sunset_state_t *)context;

    switch (event.event_type) {
        case EVENT_ACTIVATE:
            _sunrise_sunset_face_update(settings, state);
            break;
        case EVENT_LOW_ENERGY_UPDATE:
        case EVENT_TICK:
            if (state->page == 0) {
                // if entering low energy mode, start tick animation
                if (event.event_type == EVENT_LOW_ENERGY_UPDATE && !watch_tick_animation_is_running()) watch_start_tick_animation(1000);
                // check if we need to update the display
                watch_date_time date_time = watch_rtc_get_date_time();
                if (date_time.reg >= state->rise_set_expires.reg) {
                    // and on the off chance that this happened before EVENT_TIMEOUT snapped us back to rise/set 0, go back now
                    state->rise_index = 0;
                    _sunrise_sunset_face_update(settings, state);
                }
            } else {
                _sunrise_sunset_face_update_settings_display(event, state);
            }
            display_dice_roll_animation(state);
            break;
        case EVENT_LIGHT_BUTTON_DOWN:
            if (state->page) {
                state->active_digit++;
                if (state->page == 1 && state->active_digit == 1) state->active_digit++; // max latitude is +- 90, no hundreds place
                if (state->active_digit > 5) {
                    state->active_digit = 0;
                    state->page = (state->page + 1) % 3;
                    _sunrise_sunset_face_update_location_register(state);
                }
                _sunrise_sunset_face_update_settings_display(event, context);
                if (state->page == 0) {
                    movement_request_tick_frequency(1);
                    state->rise_index = 0;
                    _sunrise_sunset_face_update(settings, state);
                }
            } else {
                movement_illuminate_led();
            }
            break;
        case EVENT_ALARM_BUTTON_UP:
            if (state->page) {
                _sunrise_sunset_face_advance_digit(state);
                _sunrise_sunset_face_update_settings_display(event, context);
            } else {
                state->rise_index = (state->rise_index + 1) % 3;
                if (state->rise_index == 2) {
                    _update_moon_phase(settings);
                } else {
                    _sunrise_sunset_face_update(settings, state);
                }
            }
            break;
        case EVENT_LIGHT_LONG_PRESS:
            if (state->page == 0) {
                state->page++;
                state->active_digit = 0;
                watch_clear_display();
                movement_request_tick_frequency(4);
                _sunrise_sunset_face_update_settings_display(event, context);
            }
            break;
        case EVENT_ALARM_LONG_PRESS:
            if (state->page == 0 && (state->rise_index == 0 || state->rise_index == 1)) {
                // Roll the die
                generate_random_number(state);
            }
            break;
        case EVENT_TIMEOUT:
            state->page = 0;
            state->rise_index = 0;
            movement_request_tick_frequency(1);
            movement_move_to_face(0);
            break;
        default:
            return movement_default_loop_handler(event, settings);
    }

    return true;
}

void sunrise_sunset_face_resign(movement_settings_t *settings, void *context) {
    (void) settings;
    sunrise_sunset_state_t *state = (sunrise_sunset_state_t *)context;
    state->page = 0;
    state->active_digit = 0;
    state->rise_index = 0;
    _sunrise_sunset_face_update_location_register(state);
}
