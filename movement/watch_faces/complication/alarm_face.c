/*
 * MIT License
 *
 * Copyright (c) 2022 Andreas Nebinger
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
 */

#include <stdlib.h>
#include <string.h>

#include "alarm_face.h"
#include "watch.h"
#include "watch_utility.h"
#include "watch_private_display.h"

typedef enum {
    alarm_setting_idx_day,
    alarm_setting_idx_hour,
    alarm_setting_idx_minute,
    alarm_setting_idx_pitch,
    alarm_setting_idx_beeps
} alarm_setting_idx_t;

static const char _dow_strings[ALARM_DAY_STATES][2] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU", "AL", "MF", "WN"};
static const char _beeps_strings[ALARM_MAX_BEEP_ROUNDS][1] = {"o", "1", "2", "3", "4", "5", "6", "7", "8", "9", "L"};
static const uint8_t _blink_idx[ALARM_SETTING_STATES] = {0, 4, 6, 2, 3};
static const uint8_t _blink_idx2[ALARM_SETTING_STATES] = {1, 5, 7, 2, 3};
static const BuzzerNote _buzzer_notes[3] = {BUZZER_NOTE_B6, BUZZER_NOTE_C8, BUZZER_NOTE_A8};

static int8_t _wait_ticks;

static uint8_t _get_weekday_idx(watch_date_time date_time) {
    date_time.unit.year += 20;
    if (date_time.unit.month <= 2) {
        date_time.unit.month += 12;
        date_time.unit.year--;
    }
    return (date_time.unit.day + 13 * (date_time.unit.month + 1) / 5 + date_time.unit.year + date_time.unit.year / 4 + 525 - 2) % 7;
}

static void _alarm_set_signal(alarm_state_t *state) {
    if (state->alarm.enabled)
        watch_set_indicator(WATCH_INDICATOR_SIGNAL);
    else
        watch_clear_indicator(WATCH_INDICATOR_SIGNAL);
}

static void _alarm_face_draw(movement_settings_t *settings, alarm_state_t *state, uint8_t subsecond) {
    char buf[12];

    //handle am/pm for hour display
    uint8_t h = state->alarm.hour;
    if (!settings->bit.clock_mode_24h) {
        if (h >= 12) {
            watch_set_indicator(WATCH_INDICATOR_PM);
            h %= 12;
        } else {
            watch_clear_indicator(WATCH_INDICATOR_PM);
        }
        if (h == 0) h = 12;
    } else {
        watch_set_indicator(WATCH_INDICATOR_24H);
    }
    sprintf(buf, "%c%c%c%c%2d%02d  ",
        _dow_strings[state->alarm.day][0], _dow_strings[state->alarm.day][1],
        33, // 3 horizontal lines
        _beeps_strings[state->alarm.beeps][0],
        h,
        state->alarm.minute);
    // blink items if in settings mode
    if (state->is_setting && subsecond % 2 && !state->alarm_quick_ticks) {
        buf[_blink_idx[state->setting_state]] = buf[_blink_idx2[state->setting_state]] = ' ';
    }
    watch_display_string(buf, 0);
    // set alarm indicator
    _alarm_set_signal(state);
}

static void _alarm_initiate_setting(movement_settings_t *settings, alarm_state_t *state, uint8_t subsecond) {
    state->is_setting = true;
    state->setting_state = 0;
    movement_request_tick_frequency(4);
    _alarm_face_draw(settings, state, subsecond);
}

static void _alarm_resume_setting(movement_settings_t *settings, alarm_state_t *state, uint8_t subsecond) {
    state->is_setting = false;
    movement_request_tick_frequency(1);
    _alarm_face_draw(settings, state, subsecond);
}

static void _alarm_update_alarm_enabled(movement_settings_t *settings, alarm_state_t *state) {
    // save indication for active alarms to movement settings
    bool active_alarms = false;
    watch_date_time now = watch_rtc_get_date_time();
    uint8_t weekday_idx = _get_weekday_idx(now);
    uint16_t now_minutes_of_day = now.unit.hour * 60 + now.unit.minute;
    uint16_t alarm_minutes_of_day = state->alarm.hour * 60 + state->alarm.minute;
    if (state->alarm.enabled) {
        // figure out if alarm is to go off in the next 24 h
        if (state->alarm.day == ALARM_DAY_EACH_DAY) {
            active_alarms = true;
        } else {
            // no more shortcuts: check days and times for all possible cases...
            if ((state->alarm.day == weekday_idx && alarm_minutes_of_day >= now_minutes_of_day)
                || ((weekday_idx + 1) % 7 == state->alarm.day && alarm_minutes_of_day <= now_minutes_of_day) 
                || (state->alarm.day == ALARM_DAY_WORKDAY && (weekday_idx < 4
                    || (weekday_idx == 4 && alarm_minutes_of_day >= now_minutes_of_day)
                    || (weekday_idx == 6 && alarm_minutes_of_day <= now_minutes_of_day)))
                || (state->alarm.day == ALARM_DAY_WEEKEND && (weekday_idx == 5
                    || (weekday_idx == 6 && alarm_minutes_of_day >= now_minutes_of_day)
                    || (weekday_idx == 4 && alarm_minutes_of_day <= now_minutes_of_day)))) {
                active_alarms = true;
            }
        }
    }
    settings->bit.alarm_enabled = active_alarms;
}

static void _alarm_play_short_beep(uint8_t pitch_idx) {
    // play a short double beep
    watch_buzzer_play_note(_buzzer_notes[pitch_idx], 50);
    watch_buzzer_play_note(BUZZER_NOTE_REST, 50);
    watch_buzzer_play_note(_buzzer_notes[pitch_idx], 70);
}

static void _alarm_indicate_beep(alarm_state_t *state) {
    // play an example for the current beep setting
    if (state->alarm.beeps == 0) {
        // short double beep
        _alarm_play_short_beep(state->alarm.pitch);
    } else {
        // regular alarm beep
        movement_play_alarm_beeps(1, _buzzer_notes[state->alarm.pitch]);
    }
}

static void _abort_quick_ticks(alarm_state_t *state) {
    // abort counting quick ticks
    if (state->alarm_quick_ticks) {
        state->alarm_quick_ticks = false;
        movement_request_tick_frequency(4);
    }
}

void alarm_face_setup(movement_settings_t *settings, uint8_t watch_face_index, void **context_ptr) {
    (void) settings;
    (void) watch_face_index;

    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(alarm_state_t));
        alarm_state_t *state = (alarm_state_t *)*context_ptr;
        memset(*context_ptr, 0, sizeof(alarm_state_t));
        // initialize the default alarm values
        state->alarm.day = ALARM_DAY_EACH_DAY;
        state->alarm.beeps = 5;
        state->alarm.pitch = 1;
        state->alarm_handled_minute = -1;
        _wait_ticks = -1;
    }
}

void alarm_face_activate(movement_settings_t *settings, void *context) {
    (void) settings;
    (void) context;
    watch_set_colon();
}

void alarm_face_resign(movement_settings_t *settings, void *context) {
    alarm_state_t *state = (alarm_state_t *)context;
    state->is_setting = false;
    _alarm_update_alarm_enabled(settings, state);
    watch_set_led_off();
    state->alarm_quick_ticks = false;
    _wait_ticks = -1;
    movement_request_tick_frequency(1);
}

bool alarm_face_wants_background_task(movement_settings_t *settings, void *context) {
    (void) settings;
    alarm_state_t *state = (alarm_state_t *)context;
    watch_date_time now = watch_rtc_get_date_time();
    // just a failsafe: never fire more than one alarm within a minute
    if (state->alarm_handled_minute == now.unit.minute) return false;
    state->alarm_handled_minute = now.unit.minute;
    // check the rest
    if (state->alarm.enabled) {
        if (state->alarm.minute == now.unit.minute) {
            if (state->alarm.hour == now.unit.hour) {
                if (state->alarm.day == ALARM_DAY_EACH_DAY) return true;
                uint8_t weekday_idx = _get_weekday_idx(now);
                if (state->alarm.day == weekday_idx) return true;
                if (state->alarm.day == ALARM_DAY_WORKDAY && weekday_idx < 5) return true;
                if (state->alarm.day == ALARM_DAY_WEEKEND && weekday_idx >= 5) return true;
            }
        }
    }
    state->alarm_handled_minute = -1;
    // update the movement's alarm indicator five times an hour
    if (now.unit.minute % 12 == 0) _alarm_update_alarm_enabled(settings, state);
    return false;
}

bool alarm_face_loop(movement_event_t event, movement_settings_t *settings, void *context) {
    (void) settings;
    alarm_state_t *state = (alarm_state_t *)context;

    switch (event.event_type) {
    case EVENT_TICK:
        if (state->alarm_quick_ticks) {
            // we are in fast cycling mode
            if (state->setting_state == alarm_setting_idx_hour) {
                        state->alarm.hour = (state->alarm.hour + 1) % 24;
            } else if (state->setting_state == alarm_setting_idx_minute) {
                        state->alarm.minute = (state->alarm.minute + 1) % 60;
            } else _abort_quick_ticks(state);
        } else if (!state->is_setting) {
            if (_wait_ticks >= 0) _wait_ticks++;
            if (_wait_ticks == 2) {
                // extra long press of alarm button
                _wait_ticks = -1;
                // revert change of enabled flag and show it briefly
                state->alarm.enabled ^= 1;
                _alarm_set_signal(state);
                delay_ms(275);
            } else break; // no need to do anything when we are not in settings mode and no quick ticks are running
        }
        // fall through
    case EVENT_ACTIVATE:
        _alarm_face_draw(settings, state, event.subsecond);
        break;
    case EVENT_LIGHT_BUTTON_UP:
        if (!state->is_setting) {
            movement_illuminate_led();
            break;
        }
        state->setting_state += 1;
        if (state->setting_state >= ALARM_SETTING_STATES) {
            // we have done a full settings cycle, so resume to normal
            _alarm_resume_setting(settings, state, event.subsecond);
        }
        break;
    case EVENT_LIGHT_LONG_PRESS:
        if (state->is_setting) {
            _alarm_resume_setting(settings, state, event.subsecond);
        } else {
            _alarm_initiate_setting(settings, state, event.subsecond);
        }
        break;
    case EVENT_ALARM_BUTTON_UP:
        if (!state->is_setting) {
            // stop wait ticks counter
            _wait_ticks = -1;
        } else {
            // handle the settings behaviour
            switch (state->setting_state) {
            case alarm_setting_idx_day:
                // day selection
                state->alarm.day = (state->alarm.day + 1) % (ALARM_DAY_STATES);
                break;
            case alarm_setting_idx_hour:
                // hour selection
                _abort_quick_ticks(state);
                state->alarm.hour = (state->alarm.hour + 1) % 24;
                break;
            case alarm_setting_idx_minute:
                // minute selection
                _abort_quick_ticks(state);
                state->alarm.minute = (state->alarm.minute + 1) % 60;
                break;
            case alarm_setting_idx_pitch:
                // pitch level
                state->alarm.pitch = (state->alarm.pitch + 1) % 3;
                // play sound to show user what this is for
                _alarm_indicate_beep(state);
                break;
            case alarm_setting_idx_beeps:
                // number of beeping rounds selection
                state->alarm.beeps = (state->alarm.beeps + 1) % ALARM_MAX_BEEP_ROUNDS;
                // play sounds when user reaches 'short' length and also one time on regular beep length
                if (state->alarm.beeps <= 1) _alarm_indicate_beep(state);
                break;
            default:
                break;
            }
        }
        _alarm_face_draw(settings, state, event.subsecond);
        break;
    case EVENT_ALARM_LONG_PRESS:
        if (!state->is_setting) {
            // toggle the enabled flag for current alarm
            state->alarm.enabled ^= 1;
            // start wait ticks counter
            _wait_ticks = 0;
        } else {
            // handle the long press settings behaviour
            switch (state->setting_state) {
            case alarm_setting_idx_minute:
            case alarm_setting_idx_hour:
                // initiate fast cycling for hour or minute settings
                movement_request_tick_frequency(8);
                state->alarm_quick_ticks = true;
                break;
            default:
                break;
            }
        }
        _alarm_face_draw(settings, state, event.subsecond);
        break;
    case EVENT_ALARM_LONG_UP:
        if (state->is_setting) {
            if (state->setting_state == alarm_setting_idx_hour || state->setting_state == alarm_setting_idx_minute)
                _abort_quick_ticks(state);
        } else _wait_ticks = -1;
        break;
    case EVENT_BACKGROUND_TASK:
        // play alarm
        if (state->alarm.beeps == 0) {
            // short beep
            if (watch_is_buzzer_or_led_enabled()) {
                _alarm_play_short_beep(state->alarm.pitch);
            } else {
                // enable, play beep and disable buzzer again
                watch_enable_buzzer();
                _alarm_play_short_beep(state->alarm.pitch);
                watch_disable_buzzer();
            }
        } else {
            // regular alarm beeps
            movement_play_alarm_beeps((state->alarm.beeps == (ALARM_MAX_BEEP_ROUNDS - 1) ? 20 : state->alarm.beeps), 
                                  _buzzer_notes[state->alarm.pitch]);
        }
        break;
    case EVENT_TIMEOUT:
        movement_move_to_face(0);
        break;
    case EVENT_LIGHT_BUTTON_DOWN:
        // don't light up every time light is hit
        break;
    default:
        movement_default_loop_handler(event, settings);
        break;
    }

    return true;
}
