#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <opus.h>
#include <string.h>

#include "main.h"

#define TICK_INTERVAL 15
#define TASK_STACK_SIZE 16384
#define ANSWER_BUFFER_SIZE 1024

// Decode: 2 ch, 16-bit, 16Khz, <= 120 ms = 1920 samples/ch
#define SAMPLE_RATE 16000
#define NUM_CHANNELS 2
#define OPUS_DECODE_BUFFER_SAMPLES_PER_CHANNEL (SAMPLE_RATE / 1000 * 120)
#define OPUS_DECODE_BUFFER_SIZE \
  OPUS_DECODE_BUFFER_SAMPLES_PER_CHANNEL *NUM_CHANNELS * sizeof(opus_int16)

static const char SDP_TYPE_ANSWER[] = "answer";

static PeerConnection *g_pc = NULL;
static SemaphoreHandle_t g_mutex = NULL;

opus_int16 *g_decode_buffer = NULL;
OpusDecoder *g_opus_decoder = NULL;

static StateCallback g_state_cb = NULL;
static SignalCallback g_signal_cb = NULL;

// Remote offer
static char *g_offer_buffer = NULL;
// Remote ICE candidate
static char *g_ice_candidate_buffer = NULL;
// Local description
static char *g_local_description = NULL;

static void on_ice_state_change(PeerConnectionState state, void *user_data) {
  ESP_LOGI(LOG_TAG, "Subscriber PeerConnectionState: %s",
           peer_connection_state_to_string(state));
  if (g_state_cb) {
    g_state_cb(state);
  }
}

static void on_ice_candidate(char *description, void *user_data) {
  // This method is confusing, it's called with the PC's local description
  // rather than with an ICE candidate.
  // Save the local description so we can use it to generate the answer.
  ESP_LOGD(LOG_TAG, "Subscriber on_ice_candidate: %s", description);
  g_local_description = strdup(description);
}

static void on_data_open(void *user_data) {
  ESP_LOGI(LOG_TAG, "Subscriber data channel opened");
}

static void on_data_message(char *data, size_t size, void *user_data,
                            uint16_t sid) {
  ESP_LOGI(LOG_TAG, "Subscriber data channel: %s", data);
}

static void init_audio_decoder() {
  int error = 0;
  g_opus_decoder = opus_decoder_create(SAMPLE_RATE, NUM_CHANNELS, &error);
  if (error != OPUS_OK) {
    panic("Failed to create Opus decoder");
  }

  g_decode_buffer = (opus_int16 *)malloc(OPUS_DECODE_BUFFER_SIZE);
  ESP_LOGI(LOG_TAG, "Initialized Opus decoder");
}

static void decode_audio(uint8_t *data, size_t size) {
  int samples = opus_decode(g_opus_decoder, data, size, g_decode_buffer,
                            OPUS_DECODE_BUFFER_SAMPLES_PER_CHANNEL, 0);
  if (samples <= 0) {
    ESP_LOGE(LOG_TAG, "Failed to decode audio");
    return;
  }

  lk_render_audio(g_decode_buffer, samples * NUM_CHANNELS * sizeof(opus_int16));
}

static PeerConnection *create_peer_connection() {
  PeerConfiguration pc_config = {
      .ice_servers = {},
      .audio_codec = CODEC_OPUS,
      .video_codec = CODEC_NONE,
      .datachannel = DATA_CHANNEL_STRING,
      .onaudiotrack = [](uint8_t *data, size_t size, void *userdata) -> void {
        decode_audio(data, size);
      },
      .onvideotrack = NULL,
      .on_request_keyframe = NULL,
      .user_data = NULL,
  };
  return peer_connection_create(&pc_config);
}

static void pc_task(void *user_data) {
  init_audio_decoder();
  g_mutex = xSemaphoreCreateMutex();
  g_pc = create_peer_connection();
  if (!g_pc) {
    panic("Failed to create peer connection");
  }

  peer_connection_oniceconnectionstatechange(g_pc, on_ice_state_change);
  peer_connection_onicecandidate(g_pc, on_ice_candidate);
  peer_connection_ondatachannel(g_pc, on_data_message, on_data_open, NULL);

  while (1) {
    if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
      peer_connection_loop(g_pc);
      xSemaphoreGive(g_mutex);
    }
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}

void lk_sub_create(StateCallback state_cb, SignalCallback signal_cb) {
  g_state_cb = state_cb;
  g_signal_cb = signal_cb;

  TaskHandle_t task_handle;
  BaseType_t ret = xTaskCreatePinnedToCore(
      pc_task, "lk_subscriber", TASK_STACK_SIZE, NULL, 5, &task_handle, 1);
  assert(ret == pdPASS);
  ESP_LOGI(LOG_TAG, "Created subscriber task handle %p", task_handle);
}

static char *create_answer() {
  // The offer will always have a data channel, and optionally an audio track.
  // The local desc will always have an audio track followed by a data
  // channel. Our job here is to shuffle the local desc to match the offer.
  // Note: we're not ensuring that a=mid values match the offer.
  bool include_audio = strstr(g_offer_buffer, "m=audio") != NULL;
  const char *local_data_m_section =
      strstr(g_local_description, "m=application");
  const char *local_audio_m_section = strstr(g_local_description, "m=audio");
  size_t session_len = local_audio_m_section - g_local_description;
  size_t audio_len = local_data_m_section - local_audio_m_section;
  size_t data_len = strlen(local_data_m_section);
  // First we'll copy the session-level attributes.
  char *answer = (char *)malloc(session_len + audio_len + data_len + 1);
  char *answer_ptr = answer;
  strncpy(answer_ptr, g_local_description, session_len);
  answer_ptr += session_len;
  // Now we'll copy the m= sections in the desired order.
  strncpy(answer_ptr, local_data_m_section, data_len);
  answer_ptr += data_len;
  if (include_audio) {
    strncpy(answer_ptr, local_audio_m_section, audio_len);
    answer_ptr += audio_len;
  }
  *answer_ptr = '\0';
  return answer;
}

static void process_signaling_values() {
  // If we don't have an offer buffer, nothing to do.
  if (!g_offer_buffer) {
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

  // Apply the offer and generate the answer.
  peer_connection_set_remote_description(g_pc, g_offer_buffer);
  char *answer = create_answer();
  g_signal_cb(SDP_TYPE_ANSWER, answer);

  // Clean up.
  free(g_offer_buffer);
  g_offer_buffer = NULL;
  free(answer);
}

void lk_sub_set_remote_description(char *offer) {
  if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
    g_offer_buffer = strdup(offer);
    process_signaling_values();
    xSemaphoreGive(g_mutex);
  }
}

void lk_sub_add_ice_candidate(char *description) {
  if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
    assert(g_ice_candidate_buffer == NULL);
    g_ice_candidate_buffer = strdup(description);
    process_signaling_values();
    xSemaphoreGive(g_mutex);
  }
}
