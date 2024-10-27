
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <opus.h>
#include <string.h>

#include "main.h"

#define TICK_INTERVAL 1
#define TASK_STACK_SIZE 32768

// Encode: 2 ch, 16-bit, 16Khz, 20 ms = 320 samples/ch
#define SAMPLE_RATE 16000
#define NUM_CHANNELS 2
#define OPUS_ENCODE_BUFFER_SAMPLES_PER_CHANNEL (SAMPLE_RATE / 1000 * 20)
#define OPUS_ENCODE_BUFFER_SIZE \
  OPUS_ENCODE_BUFFER_SAMPLES_PER_CHANNEL *NUM_CHANNELS * sizeof(opus_int16)
#define OPUS_OUT_BUFFER_SIZE 4000
#define OPUS_ENCODER_BITRATE 20000
#define OPUS_ENCODER_COMPLEXITY 0

static const char SDP_TYPE_OFFER[] = "offer";

// Publisher setup proceeds as follows:
// 1. Send AddTrackRequest
// 2. Receive response and create local offer
// 3. Send local offer
// 4. Receive remote answerr

static PeerConnection *g_pc = NULL;
static SemaphoreHandle_t g_mutex = NULL;

static OpusEncoder *g_opus_encoder = NULL;
static uint8_t *g_encode_buffer = NULL;

static StateCallback g_state_cb = NULL;
static SignalCallback g_signal_cb = NULL;

static char *g_local_description = NULL;
static char *g_answer_buffer = NULL;
static char *g_ice_candidate_buffer = NULL;

static void on_ice_state_change(PeerConnectionState state, void *user_data) {
  ESP_LOGI(LOG_TAG, "Publisher PeerConnectionState: %s",
           peer_connection_state_to_string(state));
  if (g_state_cb) {
    g_state_cb(state);
  }
}

static void on_ice_candidate(char *description, void *user_data) {
  ESP_LOGD(LOG_TAG, "Publisher on_ice_candidate: %s", description);
  g_local_description = strdup(description);
  if (g_signal_cb) {
    g_signal_cb(SDP_TYPE_OFFER, g_local_description);
  }
}

static void init_audio_encoder() {
  int error;
  g_opus_encoder = opus_encoder_create(SAMPLE_RATE, NUM_CHANNELS,
                                       OPUS_APPLICATION_VOIP, &error);
  if (error != OPUS_OK) {
    panic("Failed to create Opus encoder");
  }

  opus_encoder_ctl(g_opus_encoder, OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE));
  opus_encoder_ctl(g_opus_encoder,
                   OPUS_SET_COMPLEXITY(OPUS_ENCODER_COMPLEXITY));
  opus_encoder_ctl(g_opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  // OPUS_BANDWIDTH_MEDIUMBAND or NARROWBAND if we need more speed
  opus_encoder_ctl(g_opus_encoder,
                   OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND));
  opus_encoder_ctl(g_opus_encoder, OPUS_SET_FORCE_CHANNELS(1));
  g_encode_buffer = (uint8_t *)malloc(OPUS_OUT_BUFFER_SIZE);
  ESP_LOGI(LOG_TAG, "Initialized Opus encoder");
}

static void encode_audio(const int16_t *data, size_t in_bytes) {
  assert(in_bytes == OPUS_ENCODE_BUFFER_SIZE);
  size_t out_bytes =
      opus_encode(g_opus_encoder, data, OPUS_ENCODE_BUFFER_SAMPLES_PER_CHANNEL,
                  g_encode_buffer, OPUS_OUT_BUFFER_SIZE);
  peer_connection_send_audio(g_pc, g_encode_buffer, out_bytes);
}

static PeerConnection *create_peer_connection() {
  PeerConfiguration pc_config = {
      .ice_servers = {},
      .audio_codec = CODEC_OPUS,
      .video_codec = CODEC_NONE,
      .datachannel = DATA_CHANNEL_NONE,
      .onaudiotrack = NULL,
      .onvideotrack = NULL,
      .on_request_keyframe = NULL,
      .user_data = NULL,
  };
  return peer_connection_create(&pc_config);
}

static void pc_task(void *user_data) {
  init_audio_encoder();
  g_mutex = xSemaphoreCreateMutex();
  g_pc = create_peer_connection();
  if (!g_pc) {
    panic("Failed to create peer connection");
  }

  peer_connection_oniceconnectionstatechange(g_pc, on_ice_state_change);
  peer_connection_onicecandidate(g_pc, on_ice_candidate);

  uint32_t start_time = esp_timer_get_time();
  int frames = 0;
  while (1) {
    PeerConnectionState state = peer_connection_get_state(g_pc);
    const int16_t *data = NULL;
    size_t bytes = 0;
    if (state == PEER_CONNECTION_COMPLETED) {
      data = lk_capture_audio(&bytes);
      if (data) {
        frames++;
        uint32_t current_time = esp_timer_get_time();
        uint32_t elapsed_time = current_time - start_time;
        if (elapsed_time > 1000000) {
          if (frames < 50) {
            ESP_LOGI(LOG_TAG, "Audio encode is too slow, fps: %d", frames);
          }
          frames = 0;
          start_time = current_time;
        }
      }
    }
    if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
      if (data) {
        encode_audio(data, bytes);
      }
      peer_connection_loop(g_pc);
      xSemaphoreGive(g_mutex);
    }
    int delay_ms = (state == PEER_CONNECTION_COMPLETED) ? 1 : 10;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }
}
void lk_pub_create(StateCallback state_cb, SignalCallback signal_cb) {
  g_state_cb = state_cb;
  g_signal_cb = signal_cb;

  TaskHandle_t task_handle;
  StackType_t *task_stack = (StackType_t *)malloc(TASK_STACK_SIZE);
  StaticTask_t *task_control_block =
      (StaticTask_t *)malloc(sizeof(StaticTask_t));
  task_handle =
      xTaskCreateStaticPinnedToCore(pc_task, "lk_publisher", TASK_STACK_SIZE,
                                    NULL, 7, task_stack, task_control_block, 0);
  assert(task_handle != NULL);
  ESP_LOGI(LOG_TAG, "Created publisher task handle %p", task_handle);
}

static void process_signaling_values() {
  // If we don't have an offer buffer, nothing to do.
  if (!g_answer_buffer) {
    return;
  }

  auto state = peer_connection_get_state(g_pc);
  if (state != PEER_CONNECTION_COMPLETED) {
    // If we're not connected yet, we need an ICE candidate too.
    if (!g_ice_candidate_buffer) {
      return;
    }

    // Apply the ICE candidate and dispose of it.
    peer_connection_add_ice_candidate(g_pc, g_ice_candidate_buffer);
    free(g_ice_candidate_buffer);
    g_ice_candidate_buffer = NULL;
  }

  // Apply the offer and dispose of it.
  peer_connection_set_remote_description(g_pc, g_answer_buffer);
  free(g_answer_buffer);
  g_answer_buffer = NULL;
}

void lk_pub_reoffer() {
  if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
    peer_connection_create_offer(g_pc);
    xSemaphoreGive(g_mutex);
  }
}

void lk_pub_set_remote_description(char *sdp) {
  if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
    g_answer_buffer = strdup(sdp);
    process_signaling_values();
    xSemaphoreGive(g_mutex);
  }
}

void lk_pub_add_ice_candidate(char *description) {
  if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
    assert(g_ice_candidate_buffer == NULL);
    g_ice_candidate_buffer = strdup(description);
    process_signaling_values();
    xSemaphoreGive(g_mutex);
  }
}
