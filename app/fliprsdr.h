#pragma once

#include <furi.h>
#include <stdbool.h>
#include <stdint.h>
#include <storage/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLIPRSDR_SETTINGS_PATH                  APP_DATA_PATH("fliprsdr.settings")
#define FLIPRSDR_SETTINGS_MAGIC                 0x52U
#define FLIPRSDR_SETTINGS_VERSION               3U
#define FLIPRSDR_BURST_TIMINGS_CAPACITY         8192U
#define FLIPRSDR_CAPTURE_STREAM_DEPTH           2048U
#define FLIPRSDR_PROTOCOL_LINE_MAX              256U
#define FLIPRSDR_PROTOCOL_CHUNK_TIMINGS         16U
#define FLIPRSDR_TRANSPORT_QUEUE_DEPTH          16U
#define FLIPRSDR_PREVIEW_GRAPH_POINTS           48U
#define FLIPRSDR_USB_VCP_CHANNEL                1U
#define FLIPRSDR_USB_SEND_TIMEOUT_MS            200U
#define FLIPRSDR_BLE_SEND_TIMEOUT_MS            750U
#define FLIPRSDR_FREQUENCY_OFFSET_MIN_KHZ       (-250)
#define FLIPRSDR_FREQUENCY_OFFSET_MAX_KHZ       250
#define FLIPRSDR_FREQUENCY_OFFSET_STEP_KHZ      5

typedef enum {
    FlipRSDRFrequencyPreset300,
    FlipRSDRFrequencyPreset315,
    FlipRSDRFrequencyPreset390,
    FlipRSDRFrequencyPreset43392,
    FlipRSDRFrequencyPreset86835,
    FlipRSDRFrequencyPreset915,
    FlipRSDRFrequencyPresetCount,
} FlipRSDRFrequencyPreset;

typedef enum {
    FlipRSDRTransportKindUsb,
    FlipRSDRTransportKindBle,
    FlipRSDRTransportKindCount,
} FlipRSDRTransportKind;

typedef enum {
    FlipRSDRStreamModeLive,
    FlipRSDRStreamModeBuffered,
    FlipRSDRStreamModeLiveBuffered,
    FlipRSDRStreamModeCount,
} FlipRSDRStreamMode;

typedef enum {
    FlipRSDRCaptureStateIdle,
    FlipRSDRCaptureStateListening,
    FlipRSDRCaptureStateReceiving,
    FlipRSDRCaptureStateStreaming,
    FlipRSDRCaptureStateComplete,
} FlipRSDRCaptureState;

typedef enum {
    FlipRSDRTransportStateDisconnected,
    FlipRSDRTransportStateWaiting,
    FlipRSDRTransportStateConnected,
    FlipRSDRTransportStateError,
} FlipRSDRTransportState;

typedef struct {
    uint8_t frequency_preset;
    uint8_t transport_kind;
    uint8_t stream_mode;
    int16_t frequency_offset_khz;
    bool auto_send_after_burst;
    bool include_rssi;
    bool include_timestamp;
    uint16_t max_pulse_count;
    uint16_t capture_timeout_ms;
    uint16_t gap_threshold_ms;
    uint16_t preview_bandwidth_khz;
} FlipRSDRSettings;

typedef struct {
    bool valid;
    bool complete;
    bool truncated;
    bool overflow;
    bool first_level;
    uint32_t session_id;
    uint32_t burst_id;
    uint32_t frequency_hz;
    uint32_t timestamp_ms;
    uint32_t total_count;
    uint32_t stored_count;
    float rssi;
    uint32_t timings[FLIPRSDR_BURST_TIMINGS_CAPACITY];
} FlipRSDRBurstBuffer;

typedef struct {
    FlipRSDRCaptureState state;
    bool running;
    bool in_burst;
    bool buffered_valid;
    bool buffered_truncated;
    bool overflow;
    bool first_level;
    uint32_t session_id;
    uint32_t burst_id;
    uint32_t current_total_count;
    uint32_t current_stored_count;
    uint32_t buffered_total_count;
    uint32_t buffered_stored_count;
    uint32_t frequency_hz;
    float last_rssi;
} FlipRSDRCaptureSnapshot;

typedef struct {
    FlipRSDRTransportKind kind;
    FlipRSDRTransportState state;
    bool configured;
    bool connected;
    bool advertising;
    bool last_send_ok;
    uint32_t usb_baud_rate;
} FlipRSDRTransportSnapshot;

#ifdef __cplusplus
}
#endif
