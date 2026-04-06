#include "capture.h"

#include "burst_buffer.h"
#include "protocol.h"
#include "radio.h"
#include "settings.h"
#include "transport.h"

#include <furi_hal.h>
#include <toolbox/level_duration.h>

struct FlipRSDRCapture {
    FlipRSDRTransport* transport;
    FlipRSDRSettings settings;
    FlipRSDRRadio radio;
    bool audio_speaker_owned;
    FuriMutex* mutex;
    FuriStreamBuffer* raw_stream;
    FuriThread* worker_thread;
    volatile bool callback_overrun;
    bool running;
    bool worker_should_exit;
    bool worker_finished;
    bool stop_requested;
    bool in_burst;
    uint32_t session_id;
    uint32_t next_burst_id;
    uint32_t session_start_tick;
    uint32_t last_event_tick;
    float last_rssi;
    bool overflow_seen;
    FlipRSDRBurstBuffer working;
    FlipRSDRBurstBuffer buffered;
    FlipRSDRBurstBuffer replay;
    uint32_t replay_next_index;
    uint32_t live_chunk[FLIPRSDR_PROTOCOL_CHUNK_TIMINGS];
    uint16_t live_chunk_count;
};

static void fliprsdr_capture_cleanup_worker(FlipRSDRCapture* capture) {
    if(capture->worker_finished && capture->worker_thread) {
        furi_thread_join(capture->worker_thread);
        furi_thread_free(capture->worker_thread);
        capture->worker_thread = NULL;
        if(capture->audio_speaker_owned) {
            fliprsdr_radio_set_audio_mirror(&capture->radio, false);
            furi_hal_speaker_release();
            capture->audio_speaker_owned = false;
        }
        fliprsdr_radio_close(&capture->radio);
    }

    if(!capture->running && capture->raw_stream) {
        furi_stream_buffer_free(capture->raw_stream);
        capture->raw_stream = NULL;
    }

    capture->worker_finished = false;
}

static bool fliprsdr_capture_stream_live_enabled(const FlipRSDRCapture* capture) {
    return (capture->settings.stream_mode == FlipRSDRStreamModeLive) ||
           (capture->settings.stream_mode == FlipRSDRStreamModeLiveBuffered);
}

static bool fliprsdr_capture_store_local_enabled(const FlipRSDRCapture* capture) {
    return (capture->settings.stream_mode == FlipRSDRStreamModeBuffered) ||
           (capture->settings.stream_mode == FlipRSDRStreamModeLiveBuffered);
}

static bool fliprsdr_capture_rssi_above_threshold(const FlipRSDRCapture* capture, float rssi) {
    if(!fliprsdr_settings_rssi_threshold_enabled(&capture->settings)) {
        return true;
    }

    return rssi >= (float)capture->settings.rssi_threshold_dbm;
}

static uint32_t fliprsdr_capture_frequency_hz(const FlipRSDRCapture* capture) {
    return fliprsdr_settings_frequency_hz(&capture->settings);
}

static void fliprsdr_capture_flush_live_chunk(FlipRSDRCapture* capture) {
    if(!fliprsdr_capture_stream_live_enabled(capture) || (capture->live_chunk_count == 0U)) {
        return;
    }

    fliprsdr_protocol_enqueue_timing_chunk(
        capture->transport,
        &capture->settings,
        capture->working.session_id,
        capture->working.burst_id,
        capture->live_chunk,
        capture->live_chunk_count);
    capture->live_chunk_count = 0U;
}

static void fliprsdr_capture_begin_burst(FlipRSDRCapture* capture, bool first_level) {
    const uint32_t timestamp_ms = furi_get_tick() - capture->session_start_tick;
    fliprsdr_burst_buffer_start(
        &capture->working,
        capture->session_id,
        capture->next_burst_id++,
        fliprsdr_capture_frequency_hz(capture),
        timestamp_ms,
        first_level);
    capture->in_burst = true;
    capture->live_chunk_count = 0U;

    if(fliprsdr_capture_stream_live_enabled(capture)) {
        fliprsdr_protocol_enqueue_burst_start(
            capture->transport, &capture->working, &capture->settings);
    }
}

static void fliprsdr_capture_finish_burst(FlipRSDRCapture* capture, bool truncated) {
    if(!capture->in_burst) return;

    capture->working.truncated |= truncated;
    fliprsdr_capture_flush_live_chunk(capture);
    capture->last_rssi = fliprsdr_radio_get_rssi(&capture->radio);
    fliprsdr_burst_buffer_complete(&capture->working, capture->last_rssi);

    if(fliprsdr_capture_stream_live_enabled(capture)) {
        fliprsdr_protocol_enqueue_burst_end(
            capture->transport, &capture->working, &capture->settings);
    }

    if(fliprsdr_capture_store_local_enabled(capture)) {
        fliprsdr_burst_buffer_copy(&capture->buffered, &capture->working);
        if(capture->settings.auto_send_after_burst) {
            fliprsdr_protocol_send_buffered_capture(
                capture->transport, &capture->buffered, &capture->settings);
        }
        capture->worker_should_exit = true;
    }

    capture->in_burst = false;
}

static void fliprsdr_capture_process_reset_marker(FlipRSDRCapture* capture) {
    capture->overflow_seen = true;
    if(capture->in_burst) {
        capture->working.overflow = true;
        capture->working.truncated = true;
        fliprsdr_capture_finish_burst(capture, true);
    }
}

static void fliprsdr_capture_process_timing(
    FlipRSDRCapture* capture,
    bool level,
    uint32_t duration_us) {
    capture->last_event_tick = furi_get_tick();
    capture->last_rssi = fliprsdr_radio_get_rssi(&capture->radio);

    if(!capture->in_burst) {
        if(!fliprsdr_capture_rssi_above_threshold(capture, capture->last_rssi)) {
            return;
        }
        fliprsdr_capture_begin_burst(capture, level);
    }

    fliprsdr_burst_buffer_append(
        &capture->working,
        duration_us,
        fliprsdr_capture_store_local_enabled(capture),
        capture->settings.max_pulse_count);

    if(fliprsdr_capture_stream_live_enabled(capture)) {
        if(capture->live_chunk_count < FLIPRSDR_PROTOCOL_CHUNK_TIMINGS) {
            capture->live_chunk[capture->live_chunk_count++] = duration_us;
        }
        if(capture->live_chunk_count >= FLIPRSDR_PROTOCOL_CHUNK_TIMINGS) {
            fliprsdr_capture_flush_live_chunk(capture);
        }
    }

    if((!level) && (duration_us >= ((uint32_t)capture->settings.gap_threshold_ms * 1000UL))) {
        fliprsdr_capture_finish_burst(capture, false);
    }
}

static void fliprsdr_capture_rx_callback(bool level, uint32_t duration, void* context) {
    FlipRSDRCapture* capture = context;

    LevelDuration level_duration = level_duration_make(level, duration);
    if(capture->callback_overrun) {
        capture->callback_overrun = false;
        level_duration = level_duration_reset();
    }

    if(furi_stream_buffer_send(capture->raw_stream, &level_duration, sizeof(level_duration), 0) !=
       sizeof(level_duration)) {
        capture->callback_overrun = true;
    }
}

static LevelDuration fliprsdr_capture_replay_callback(void* context) {
    FlipRSDRCapture* capture = context;
    furi_assert(capture);

    if(capture->replay_next_index == 0U && capture->replay.valid && !capture->replay.first_level) {
        capture->replay_next_index = 1U;
    }

    if(!capture->replay.valid || capture->replay_next_index >= capture->replay.stored_count) {
        return level_duration_reset();
    }

    const uint32_t timing_index = capture->replay_next_index++;
    const bool level = capture->replay.first_level ? ((timing_index % 2U) == 0U) :
                                                   ((timing_index % 2U) != 0U);
    return level_duration_make(level, capture->replay.timings[timing_index]);
}

static int32_t fliprsdr_capture_worker(void* context) {
    FlipRSDRCapture* capture = context;
    LevelDuration level_duration;

    while(!capture->worker_should_exit) {
        const size_t received = furi_stream_buffer_receive(
            capture->raw_stream, &level_duration, sizeof(level_duration), 10);

        if(received == sizeof(level_duration)) {
            furi_check(furi_mutex_acquire(capture->mutex, FuriWaitForever) == FuriStatusOk);
            if(level_duration_is_reset(level_duration)) {
                fliprsdr_capture_process_reset_marker(capture);
                furi_check(furi_mutex_release(capture->mutex) == FuriStatusOk);
                continue;
            }

            fliprsdr_capture_process_timing(
                capture,
                level_duration_get_level(level_duration),
                level_duration_get_duration(level_duration));
            furi_check(furi_mutex_release(capture->mutex) == FuriStatusOk);
        }

        if(capture->in_burst) {
            furi_check(furi_mutex_acquire(capture->mutex, FuriWaitForever) == FuriStatusOk);
            const uint32_t elapsed_ms = furi_get_tick() - capture->last_event_tick;
            if(elapsed_ms >= capture->settings.capture_timeout_ms) {
                fliprsdr_capture_finish_burst(capture, false);
            }
            furi_check(furi_mutex_release(capture->mutex) == FuriStatusOk);
        }
    }

    if(capture->stop_requested && capture->in_burst) {
        furi_check(furi_mutex_acquire(capture->mutex, FuriWaitForever) == FuriStatusOk);
        capture->working.truncated = true;
        fliprsdr_capture_finish_burst(capture, true);
        furi_check(furi_mutex_release(capture->mutex) == FuriStatusOk);
    }

    fliprsdr_radio_stop_async_rx(&capture->radio);
    furi_hal_power_suppress_charge_exit();

    capture->running = false;
    capture->worker_finished = true;
    return 0;
}

FlipRSDRCapture* fliprsdr_capture_alloc(FlipRSDRTransport* transport) {
    FlipRSDRCapture* capture = malloc(sizeof(FlipRSDRCapture));
    capture->transport = transport;
    fliprsdr_settings_load_defaults(&capture->settings);
    fliprsdr_radio_init(&capture->radio);
    capture->audio_speaker_owned = false;
    capture->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    capture->raw_stream = NULL;
    capture->worker_thread = NULL;
    capture->callback_overrun = false;
    capture->running = false;
    capture->worker_should_exit = false;
    capture->worker_finished = false;
    capture->stop_requested = false;
    capture->in_burst = false;
    capture->session_id = 0U;
    capture->next_burst_id = 1U;
    capture->session_start_tick = 0U;
    capture->last_event_tick = 0U;
    capture->last_rssi = 0.0f;
    capture->overflow_seen = false;
    fliprsdr_burst_buffer_reset(&capture->working);
    fliprsdr_burst_buffer_reset(&capture->buffered);
    fliprsdr_burst_buffer_reset(&capture->replay);
    capture->replay_next_index = 0U;
    capture->live_chunk_count = 0U;
    return capture;
}

void fliprsdr_capture_free(FlipRSDRCapture* capture) {
    furi_assert(capture);
    fliprsdr_capture_stop(capture);
    fliprsdr_capture_cleanup_worker(capture);
    furi_mutex_free(capture->mutex);
    free(capture);
}

void fliprsdr_capture_apply_settings(FlipRSDRCapture* capture, const FlipRSDRSettings* settings) {
    furi_assert(capture);
    furi_assert(settings);
    furi_check(furi_mutex_acquire(capture->mutex, FuriWaitForever) == FuriStatusOk);
    capture->settings = *settings;
    fliprsdr_settings_validate(&capture->settings);
    furi_check(furi_mutex_release(capture->mutex) == FuriStatusOk);
}

bool fliprsdr_capture_start(FlipRSDRCapture* capture) {
    furi_assert(capture);
    fliprsdr_capture_cleanup_worker(capture);
    if(capture->running) return true;

    if(!fliprsdr_radio_open(&capture->radio, &capture->settings)) {
        return false;
    }

    uint32_t frequency = fliprsdr_capture_frequency_hz(capture);
    if(!fliprsdr_radio_is_frequency_valid(&capture->radio, frequency)) {
        fliprsdr_radio_close(&capture->radio);
        return false;
    }

    capture->session_id++;
    capture->next_burst_id = 1U;
    capture->session_start_tick = furi_get_tick();
    capture->last_rssi = 0.0f;
    capture->overflow_seen = false;
    capture->callback_overrun = false;
    capture->worker_should_exit = false;
    capture->worker_finished = false;
    capture->stop_requested = false;
    capture->in_burst = false;
    fliprsdr_burst_buffer_reset(&capture->working);
    capture->live_chunk_count = 0U;

    capture->raw_stream = furi_stream_buffer_alloc(
        sizeof(LevelDuration) * FLIPRSDR_CAPTURE_STREAM_DEPTH, sizeof(LevelDuration));
    capture->worker_thread =
        furi_thread_alloc_ex("FlipRSDRCap", 4096, fliprsdr_capture_worker, capture);
    if(!capture->raw_stream || !capture->worker_thread) {
        if(capture->worker_thread) {
            furi_thread_free(capture->worker_thread);
            capture->worker_thread = NULL;
        }
        if(capture->raw_stream) {
            furi_stream_buffer_free(capture->raw_stream);
            capture->raw_stream = NULL;
        }
        fliprsdr_radio_close(&capture->radio);
        return false;
    }

    fliprsdr_radio_reset(&capture->radio);
    fliprsdr_radio_load_preset(&capture->radio, FuriHalSubGhzPresetOok270Async);
    frequency = fliprsdr_radio_set_frequency(&capture->radio, frequency);
    capture->audio_speaker_owned = false;
    if(capture->settings.preview_audio) {
        capture->audio_speaker_owned = furi_hal_speaker_acquire(100);
        if(capture->audio_speaker_owned) {
            fliprsdr_radio_set_audio_mirror(&capture->radio, true);
        }
    }
    furi_hal_power_suppress_charge_enter();

    capture->running = true;
    furi_thread_start(capture->worker_thread);
    fliprsdr_radio_start_async_rx(&capture->radio, fliprsdr_capture_rx_callback, capture);
    capture->last_event_tick = furi_get_tick();
    return true;
}

void fliprsdr_capture_stop(FlipRSDRCapture* capture) {
    furi_assert(capture);
    fliprsdr_capture_cleanup_worker(capture);
    if(!capture->running || !capture->worker_thread) return;

    capture->stop_requested = true;
    capture->worker_should_exit = true;
    fliprsdr_capture_cleanup_worker(capture);
}

void fliprsdr_capture_clear_buffered(FlipRSDRCapture* capture) {
    furi_assert(capture);
    furi_check(furi_mutex_acquire(capture->mutex, FuriWaitForever) == FuriStatusOk);
    fliprsdr_burst_buffer_reset(&capture->buffered);
    furi_check(furi_mutex_release(capture->mutex) == FuriStatusOk);
}

bool fliprsdr_capture_send_buffered(FlipRSDRCapture* capture) {
    furi_assert(capture);
    fliprsdr_capture_cleanup_worker(capture);

    FlipRSDRBurstBuffer* buffered = malloc(sizeof(FlipRSDRBurstBuffer));
    if(!buffered) return false;

    furi_check(furi_mutex_acquire(capture->mutex, FuriWaitForever) == FuriStatusOk);
    *buffered = capture->buffered;
    furi_check(furi_mutex_release(capture->mutex) == FuriStatusOk);

    if(!buffered->valid) {
        free(buffered);
        return false;
    }

    const bool ok =
        fliprsdr_protocol_send_buffered_capture(capture->transport, buffered, &capture->settings);
    free(buffered);
    return ok;
}

bool fliprsdr_capture_send_debug_burst(FlipRSDRCapture* capture) {
    furi_assert(capture);
    return fliprsdr_protocol_send_debug_capture(
        capture->transport,
        &capture->settings,
        capture->session_id + 1U,
        1U,
        fliprsdr_capture_frequency_hz(capture));
}

bool fliprsdr_capture_prepare_replay(
    FlipRSDRCapture* capture,
    uint32_t frequency_hz,
    bool first_level,
    uint32_t total_count) {
    furi_assert(capture);

    if(total_count == 0U || total_count > FLIPRSDR_BURST_TIMINGS_CAPACITY) {
        return false;
    }

    furi_check(furi_mutex_acquire(capture->mutex, FuriWaitForever) == FuriStatusOk);
    fliprsdr_burst_buffer_start(&capture->replay, 0U, 0U, frequency_hz, 0U, first_level);
    capture->replay.total_count = total_count;
    capture->replay.stored_count = 0U;
    capture->replay.complete = false;
    capture->replay_next_index = 0U;
    furi_check(furi_mutex_release(capture->mutex) == FuriStatusOk);
    return true;
}

bool fliprsdr_capture_append_replay_timings(
    FlipRSDRCapture* capture,
    uint32_t offset,
    const uint32_t* timings,
    uint16_t timing_count) {
    furi_assert(capture);
    furi_assert(timings || (timing_count == 0U));

    bool ok = false;
    furi_check(furi_mutex_acquire(capture->mutex, FuriWaitForever) == FuriStatusOk);

    if(capture->replay.valid && offset == capture->replay.stored_count &&
       (capture->replay.stored_count + timing_count) <= capture->replay.total_count &&
       capture->replay.total_count <= FLIPRSDR_BURST_TIMINGS_CAPACITY) {
        ok = true;
        for(uint16_t i = 0; i < timing_count; i++) {
            const uint32_t duration = timings[i];
            if(duration == 0U) {
                ok = false;
                break;
            }
            capture->replay.timings[capture->replay.stored_count++] = duration;
        }
    }

    if(!ok) {
        fliprsdr_burst_buffer_reset(&capture->replay);
        capture->replay_next_index = 0U;
    }

    furi_check(furi_mutex_release(capture->mutex) == FuriStatusOk);
    return ok;
}

void fliprsdr_capture_cancel_replay(FlipRSDRCapture* capture) {
    furi_assert(capture);
    furi_check(furi_mutex_acquire(capture->mutex, FuriWaitForever) == FuriStatusOk);
    fliprsdr_burst_buffer_reset(&capture->replay);
    capture->replay_next_index = 0U;
    furi_check(furi_mutex_release(capture->mutex) == FuriStatusOk);
}

bool fliprsdr_capture_replay(FlipRSDRCapture* capture) {
    furi_assert(capture);
    fliprsdr_capture_cleanup_worker(capture);

    FlipRSDRBurstBuffer replay = {0};
    furi_check(furi_mutex_acquire(capture->mutex, FuriWaitForever) == FuriStatusOk);
    replay = capture->replay;
    capture->replay_next_index = 0U;
    furi_check(furi_mutex_release(capture->mutex) == FuriStatusOk);

    if(!replay.valid || replay.stored_count == 0U || replay.stored_count != replay.total_count) {
        return false;
    }

    if(capture->running) {
        fliprsdr_capture_stop(capture);
        fliprsdr_capture_cleanup_worker(capture);
    }

    bool opened_here = false;
    if(!fliprsdr_radio_device(&capture->radio)) {
        if(!fliprsdr_radio_open(&capture->radio, &capture->settings)) {
            return false;
        }
        opened_here = true;
    }

    if(!fliprsdr_radio_is_frequency_valid(&capture->radio, replay.frequency_hz)) {
        if(opened_here) {
            fliprsdr_radio_close(&capture->radio);
        }
        return false;
    }

    fliprsdr_radio_reset(&capture->radio);
    fliprsdr_radio_idle(&capture->radio);
    fliprsdr_radio_load_preset(&capture->radio, FuriHalSubGhzPresetOok270Async);
    fliprsdr_radio_set_frequency(&capture->radio, replay.frequency_hz);

    furi_check(furi_mutex_acquire(capture->mutex, FuriWaitForever) == FuriStatusOk);
    capture->replay = replay;
    capture->replay_next_index = 0U;
    furi_check(furi_mutex_release(capture->mutex) == FuriStatusOk);

    bool ok = false;
    if(fliprsdr_radio_set_tx(&capture->radio)) {
        ok = fliprsdr_radio_start_async_tx(
            &capture->radio, fliprsdr_capture_replay_callback, capture);
    }

    if(ok) {
        while(!fliprsdr_radio_is_async_tx_complete(&capture->radio)) {
            furi_delay_ms(5);
        }
        fliprsdr_radio_stop_async_tx(&capture->radio);
    }

    fliprsdr_radio_idle(&capture->radio);
    if(opened_here) {
        fliprsdr_radio_close(&capture->radio);
    }

    return ok;
}

void fliprsdr_capture_copy_snapshot(
    FlipRSDRCapture* capture,
    FlipRSDRCaptureSnapshot* snapshot) {
    furi_assert(capture);
    furi_assert(snapshot);
    fliprsdr_capture_cleanup_worker(capture);

    furi_check(furi_mutex_acquire(capture->mutex, FuriWaitForever) == FuriStatusOk);
    snapshot->running = capture->running;
    snapshot->in_burst = capture->in_burst;
    snapshot->buffered_valid = capture->buffered.valid;
    snapshot->buffered_truncated = capture->buffered.truncated;
    snapshot->overflow = capture->overflow_seen;
    snapshot->first_level =
        capture->in_burst ? capture->working.first_level : capture->buffered.first_level;
    snapshot->session_id =
        capture->in_burst ? capture->working.session_id : capture->buffered.session_id;
    snapshot->burst_id = capture->in_burst ? capture->working.burst_id : capture->buffered.burst_id;
    snapshot->current_total_count = capture->working.total_count;
    snapshot->current_stored_count = capture->working.stored_count;
    snapshot->buffered_total_count = capture->buffered.total_count;
    snapshot->buffered_stored_count = capture->buffered.stored_count;
    snapshot->frequency_hz = fliprsdr_capture_frequency_hz(capture);
    snapshot->last_rssi = capture->last_rssi;

    if(capture->running) {
        snapshot->state = capture->in_burst ?
                              (fliprsdr_capture_stream_live_enabled(capture) ?
                                   FlipRSDRCaptureStateStreaming :
                                   FlipRSDRCaptureStateReceiving) :
                              FlipRSDRCaptureStateListening;
    } else if(capture->buffered.valid) {
        snapshot->state = FlipRSDRCaptureStateComplete;
    } else {
        snapshot->state = FlipRSDRCaptureStateIdle;
    }
    furi_check(furi_mutex_release(capture->mutex) == FuriStatusOk);
}
