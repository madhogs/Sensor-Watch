/*
 * MIT License
 *
 * Copyright (c) 2022 Andreas Nebinger, building on Wesley Ellis’ countdown_face.c
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
#include "timer_face.h"
#include "watch.h"
#include "watch_utility.h"

static const uint32_t _default_timer_value = 0x000100; // default timer: 1 min

// sound sequence for a single beeping sequence
static const int8_t _sound_seq_beep[] = {BUZZER_NOTE_C8, 3, BUZZER_NOTE_REST, 3, -2, 2, BUZZER_NOTE_C8, 5, BUZZER_NOTE_REST, 25, 0};
static const int8_t _sound_seq_start[] = {BUZZER_NOTE_C8, 2, 0};

static uint8_t _beeps_to_play;    // temporary counter for ring signals playing

static inline int32_t _get_tz_offset(movement_settings_t *settings) {
    return movement_timezone_offsets[settings->bit.time_zone] * 60;
}

static void _signal_callback() {
    if (_beeps_to_play) {
        _beeps_to_play--;
        watch_buzzer_play_sequence((int8_t *)_sound_seq_beep, _signal_callback);
    }
}

static void _start(timer_state_t *state, movement_settings_t *settings, bool with_beep) {
    if (state->timer.value == 0) return;
    watch_date_time now = watch_rtc_get_date_time();
    state->now_ts = watch_utility_date_time_to_unix_time(now, _get_tz_offset(settings));
    if (state->mode == pausing)
        state->target_ts = state->now_ts + state->paused_left;
    else
        state->target_ts = watch_utility_offset_timestamp(state->now_ts, 
                                                          state->timer.unit.hours, 
                                                          state->timer.unit.minutes, 
                                                          state->timer.unit.seconds);
    watch_date_time target_dt = watch_utility_date_time_from_unix_time(state->target_ts, _get_tz_offset(settings));
    state->mode = running;
    movement_schedule_background_task_for_face(state->watch_face_index, target_dt);
    watch_set_indicator(WATCH_INDICATOR_BELL);
    if (with_beep) watch_buzzer_play_sequence((int8_t *)_sound_seq_start, NULL);
}

static void _draw(timer_state_t *state, uint8_t subsecond) {
    char buf[14];
    uint32_t delta;
    div_t result;
    uint8_t h, min, sec;

    switch (state->mode) {
        case pausing:
            if (state->pausing_seconds % 2)
                watch_clear_indicator(WATCH_INDICATOR_BELL);
            else
                watch_set_indicator(WATCH_INDICATOR_BELL);
            if (state->pausing_seconds != 1)
                // not 1st iteration (or 256th): do not write anything
                return;
            // fall through
        case running:
            delta = state->target_ts - state->now_ts;
            result = div(delta, 60);
            sec = result.rem;
            result = div(result.quot, 60);
            min = result.rem;
            h = result.quot;
            sprintf(buf, "%d%2u%02u%02u", state->loop_count, h, min, sec);
            watch_set_colon();
            break;
        case setting:
            if (state->settings_state == 0) {
                // ask it the current timer shall be erased
                sprintf(buf, " CLEAR%c", state->erase_timer_flag ? 'y' : 'n');
                watch_clear_colon();
            } else if (state->settings_state == 4) {
                sprintf(buf, "  LOOP%c", state->timer.unit.repeat ? 'y' : 'n');
                watch_clear_colon();
            } else {
                sprintf(buf, " %2u%02u%02u", state->timer.unit.hours,
                        state->timer.unit.minutes,
                        state->timer.unit.seconds);
                watch_set_colon();
            }
            break;
        case waiting:
            sprintf(buf, " %2u%02u%02u", state->timer.unit.hours,
                    state->timer.unit.minutes,
                    state->timer.unit.seconds);
            watch_set_colon();
            break;
    }
    if (state->loop_count == 0) buf[0] = ' ';
    if (state->mode == setting && subsecond % 2) {
        // blink the current settings value
        if (state->settings_state == 0 || state->settings_state == 4) buf[6] = ' ';
        else buf[state->settings_state * 2 - 1] = buf[state->settings_state * 2] = ' ';
    }
    watch_display_string(buf, 3);
    // set lap indicator when we have a looping timer
    if (state->timer.unit.repeat) watch_set_indicator(WATCH_INDICATOR_LAP);
    else watch_clear_indicator(WATCH_INDICATOR_LAP);
}

static void _reset(timer_state_t *state) {
    state->mode = waiting;
    movement_cancel_background_task_for_face(state->watch_face_index);
    watch_clear_indicator(WATCH_INDICATOR_BELL);
}

static void _resume_setting(timer_state_t *state) {
    state->settings_state = 0;
    state->mode = waiting;
    movement_request_tick_frequency(1);
}

static void _settings_increment(timer_state_t *state) {
    switch(state->settings_state) {
        case 0:
            state->erase_timer_flag ^= 1;
            break;
        case 1:
            state->timer.unit.hours = (state->timer.unit.hours + 1) % 24;
            break;
        case 2:
            state->timer.unit.minutes = (state->timer.unit.minutes + 1) % 60;
            break;
        case 3:
            state->timer.unit.seconds = (state->timer.unit.seconds + 1) % 60;
            break;
        case 4:
            state->timer.unit.repeat ^= 1;
            break;
        default:
            // should never happen
            break;
    }
    return;
}

static void _abort_quick_cycle(timer_state_t *state) {
    if (state->quick_cycle) {
        state->quick_cycle = false;
        movement_request_tick_frequency(4);
    }
}

static inline bool _check_for_signal() {
    if (_beeps_to_play) {
        _beeps_to_play = 0;
        return true;
    }
    return false;
}

void timer_face_setup(movement_settings_t *settings, uint8_t watch_face_index, void ** context_ptr) {
    (void) settings;

    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(timer_state_t));
        timer_state_t *state = (timer_state_t *)*context_ptr;
        memset(*context_ptr, 0, sizeof(timer_state_t));
        state->watch_face_index = watch_face_index;
        state->timer.value = _default_timer_value;
        state->loop_count = 0;
    }
}

void timer_face_activate(movement_settings_t *settings, void *context) {
    (void) settings;
    timer_state_t *state = (timer_state_t *)context;
    watch_display_string("TR", 0);
    watch_set_colon();
    if(state->mode == running) {
        watch_date_time now = watch_rtc_get_date_time();
        state->now_ts = watch_utility_date_time_to_unix_time(now, _get_tz_offset(settings));
        watch_set_indicator(WATCH_INDICATOR_BELL);
    } else {
        state->pausing_seconds = 1;
        _beeps_to_play = 0;
    }
}

bool timer_face_loop(movement_event_t event, movement_settings_t *settings, void *context) {
    (void) settings;
    timer_state_t *state = (timer_state_t *)context;
    uint8_t subsecond = event.subsecond;

    switch (event.event_type) {
        case EVENT_ACTIVATE:
            _draw(state, event.subsecond);
            break;
        case EVENT_TICK:
            if (state->mode == running) state->now_ts++;
            else if (state->mode == pausing) state->pausing_seconds++;
            else if (state->quick_cycle) {
                if (watch_get_pin_level(BTN_ALARM)) {
                    _settings_increment(state);
                    subsecond = 0;
                } else _abort_quick_cycle(state);
            }
            _draw(state, subsecond);
            break;
        case EVENT_LIGHT_BUTTON_UP:
            switch (state->mode) {
                case pausing:
                case waiting:
                case running:
                    movement_illuminate_led();
                    break;
                case setting:
                    if (state->erase_timer_flag) {
                        state->timer.value = 0;
                        state->erase_timer_flag = false;
                    }
                    state->settings_state = (state->settings_state + 1) % 5;
                    if (state->settings_state == 0) _resume_setting(state);
                    else if (state->settings_state == 4 && (state->timer.value & 0xFFFFFF) == 0) state->settings_state = 1;
                    break;
                default:
                    break;
            }
            _draw(state, event.subsecond);
            break;
        case EVENT_ALARM_BUTTON_UP:
            _abort_quick_cycle(state);
            if (_check_for_signal()) break;;
            switch (state->mode) {
                case running:
                    state->mode = pausing;
                    state->pausing_seconds = 0;
                    state->paused_left = state->target_ts - state->now_ts;
                    movement_cancel_background_task();
                    break;
                case pausing:
                    _start(state, settings, false);
                    break;
                case waiting: {
                    break;
                }
                case setting:
                    _settings_increment(state);
                    subsecond = 0;
                    break;
            }
            _draw(state, subsecond);
            break;
        case EVENT_LIGHT_LONG_PRESS:
            if (state->mode == waiting) {
                // initiate settings
                state->mode = setting;
                state->settings_state = 0;
                state->erase_timer_flag = false;
                movement_request_tick_frequency(4);
            } else if (state->mode == setting) {
                _resume_setting(state);
            }
            _draw(state, event.subsecond);
            break;
        case EVENT_BACKGROUND_TASK:
            // play the alarm
            _beeps_to_play = 4;
            watch_buzzer_play_sequence((int8_t *)_sound_seq_beep, _signal_callback);
            _reset(state);
            if (state->timer.unit.repeat) {
                state->loop_count = (state->loop_count + 1) % 10;
                _start(state, settings, false);
            }
            else state->loop_count = 0;
            break;
        case EVENT_ALARM_LONG_PRESS:
            switch(state->mode) {
                case setting:
                    switch (state->settings_state) {
                        case 1:
                        case 2:
                        case 3:
                            state->quick_cycle = true;
                            movement_request_tick_frequency(8);
                            break;
                        default:
                            break;
                    }
                    break;
                case waiting:
                    _start(state, settings, true);
                    break;
                case pausing:
                case running:
                    _reset(state);
                    state->loop_count = 0;
                    if (settings->bit.button_should_sound) watch_buzzer_play_note(BUZZER_NOTE_C7, 50);
                    break;
                default:
                    break;
            }
            _draw(state, event.subsecond);
            break;
        case EVENT_ALARM_LONG_UP:
            _abort_quick_cycle(state);
            break;
        case EVENT_MODE_LONG_PRESS:
        case EVENT_TIMEOUT:
            _abort_quick_cycle(state);
            movement_move_to_face(0);
            break;
        // don't light up every time light is hit
        case EVENT_LIGHT_BUTTON_DOWN:
            break;
        default:
            movement_default_loop_handler(event, settings);
            break;
    }

    return true;
}

void timer_face_resign(movement_settings_t *settings, void *context) {
    (void) settings;
    timer_state_t *state = (timer_state_t *)context;
    if (state->mode == setting) {
        state->settings_state = 0;
        state->mode = waiting;
    }
}
