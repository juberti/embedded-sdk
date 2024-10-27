#include <cJSON.h>
#include <esp_log.h>
#include <esp_websocket_client.h>
#include <livekit_rtc.pb-c.h>
#include <pthread.h>

#include "main.h"

#define WEBSOCKET_URI_SIZE 1024
#define WEBSOCKET_BUFFER_SIZE 2048
#define LIVEKIT_PROTOCOL_VERSION 3

const char TRACK_NAME[] = "microphone";

static esp_websocket_client_handle_t g_client;

static const char *request_message_to_string(
    Livekit__SignalRequest__MessageCase message_case) {
  switch (message_case) {
    case LIVEKIT__SIGNAL_REQUEST__MESSAGE_OFFER:
      return "OFFER";
    case LIVEKIT__SIGNAL_REQUEST__MESSAGE_ANSWER:
      return "ANSWER";
    case LIVEKIT__SIGNAL_REQUEST__MESSAGE_TRICKLE:
      return "TRICKLE";
    case LIVEKIT__SIGNAL_REQUEST__MESSAGE_ADD_TRACK:
      return "ADD_TRACK";
    case LIVEKIT__SIGNAL_REQUEST__MESSAGE_MUTE:
      return "MUTE";
    case LIVEKIT__SIGNAL_REQUEST__MESSAGE_SUBSCRIPTION:
      return "SUBSCRIPTION";
    case LIVEKIT__SIGNAL_REQUEST__MESSAGE_TRACK_SETTING:
      return "TRACK_SETTING";
    case LIVEKIT__SIGNAL_REQUEST__MESSAGE_LEAVE:
      return "LEAVE";
    default:
      ESP_LOGI(LOG_TAG, "Unknown request message type %d", message_case);
      return "UNKNOWN";
  }
}

static const char *response_message_to_string(
    Livekit__SignalResponse__MessageCase message_case) {
  switch (message_case) {
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE__NOT_SET:
      return "NOT_SET (Ping/Pong)";
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_JOIN:
      return "JOIN";
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_ANSWER:
      return "ANSWER";
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_OFFER:
      return "OFFER";
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_TRICKLE:
      return "TRICKLE";
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_UPDATE:
      return "UPDATE";
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_TRACK_PUBLISHED:
      return "TRACK_PUBLISHED";
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_LEAVE:
      return "LEAVE";
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_MUTE:
      return "MUTE";
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_SPEAKERS_CHANGED:
      return "SPEAKERS_CHANGED";
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_ROOM_UPDATE:
      return "ROOM_UPDATE";
    default:
      ESP_LOGI(LOG_TAG, "Unknown response message type %d", message_case);
      return "UNKNOWN";
  }
}

static void lk_pack_and_send_signal_request(const Livekit__SignalRequest *r) {
  ESP_LOGI(LOG_TAG, "Send %s", request_message_to_string(r->message_case));
  auto size = livekit__signal_request__get_packed_size(r);
  auto *buffer = (uint8_t *)malloc(size);
  livekit__signal_request__pack(r, buffer);
  auto len = esp_websocket_client_send_bin(g_client, (char *)buffer, size,
                                           portMAX_DELAY);
  if (len == -1) {
    panic("Failed to send message.");
  }
  free(buffer);
}

static void add_track(const char *cid, const char *name,
                      Livekit__TrackSource source) {
  Livekit__SignalRequest r = LIVEKIT__SIGNAL_REQUEST__INIT;
  Livekit__AddTrackRequest a = LIVEKIT__ADD_TRACK_REQUEST__INIT;
  a.cid = (char *)cid;
  a.name = (char *)name;
  a.source = source;
  r.add_track = &a;
  r.message_case = LIVEKIT__SIGNAL_REQUEST__MESSAGE_ADD_TRACK;
  lk_pack_and_send_signal_request(&r);
}

static void on_sub_state(PeerConnectionState state) {
  if (state == PEER_CONNECTION_COMPLETED) {
    add_track(TRACK_NAME, TRACK_NAME, LIVEKIT__TRACK_SOURCE__MICROPHONE);
  } else if (state == PEER_CONNECTION_DISCONNECTED) {
    panic("Subscriber peer connection disconnected");
  }
}

static void on_pub_state(PeerConnectionState state) {
  if (state == PEER_CONNECTION_DISCONNECTED) {
    panic("Publisher peer connection disconnected");
  }
}

static void on_sub_signal(const char *type, const char *sdp) {
  assert(strcmp(type, "answer") == 0);
  Livekit__SignalRequest r = LIVEKIT__SIGNAL_REQUEST__INIT;
  Livekit__SessionDescription s = LIVEKIT__SESSION_DESCRIPTION__INIT;
  s.sdp = (char *)sdp;
  s.type = (char *)type;
  r.answer = &s;
  r.message_case = LIVEKIT__SIGNAL_REQUEST__MESSAGE_ANSWER;
  lk_pack_and_send_signal_request(&r);
}

static void on_pub_signal(const char *type, const char *sdp) {
  assert(strcmp(type, "offer") == 0);
  Livekit__SignalRequest r = LIVEKIT__SIGNAL_REQUEST__INIT;
  Livekit__SessionDescription s = LIVEKIT__SESSION_DESCRIPTION__INIT;
  s.sdp = (char *)sdp;
  s.type = (char *)type;
  r.offer = &s;
  r.message_case = LIVEKIT__SIGNAL_REQUEST__MESSAGE_OFFER;
  lk_pack_and_send_signal_request(&r);
}

static void handle_livekit_response(Livekit__SignalResponse *packet) {
  ESP_LOGI(LOG_TAG, "Recv %s",
           response_message_to_string(packet->message_case));
  switch (packet->message_case) {
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_JOIN:
      ESP_LOGI(LOG_TAG, "Join complete, room sid: %s", packet->join->room->sid);
      break;
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_TRICKLE: {
      // Skip TCP ICE Candidates
      if (strstr(packet->trickle->candidateinit, "tcp") != NULL) {
        ESP_LOGD(LOG_TAG, "skipping tcp ice candidate");
        return;
      }
      // Skip non-IPv4 candidates
      if (!strchr(packet->trickle->candidateinit, '.')) {
        ESP_LOGI(LOG_TAG, "skipping non-IPv4 candidate");
        return;
      }
      auto parsed = cJSON_Parse(packet->trickle->candidateinit);
      if (!parsed) {
        ESP_LOGW(LOG_TAG, "failed to parse ice_candidate_init");
        return;
      }

      auto candidate_obj = cJSON_GetObjectItem(parsed, "candidate");
      if (candidate_obj && cJSON_IsString(candidate_obj)) {
        ESP_LOGI(LOG_TAG, "Candidate: %d / %s", packet->trickle->target,
                 candidate_obj->valuestring);
        if (packet->trickle->target == LIVEKIT__SIGNAL_TARGET__SUBSCRIBER) {
          lk_sub_add_ice_candidate(candidate_obj->valuestring);
        } else {
          lk_pub_add_ice_candidate(candidate_obj->valuestring);
        }
      } else {
        ESP_LOGW(LOG_TAG, "ice_candidate_init has no candidate");
        return;
      }
      cJSON_Delete(parsed);
      break;
    }
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_OFFER:
      ESP_LOGD(LOG_TAG, "SDP:\n%s", packet->offer->sdp);
      lk_sub_set_remote_description(packet->offer->sdp);
      break;
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_ANSWER:
      ESP_LOGD(LOG_TAG, "SDP:\n%s", packet->answer->sdp);
      lk_pub_set_remote_description(packet->answer->sdp);
      break;
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_TRACK_PUBLISHED:
      lk_pub_reoffer();
      break;
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_LEAVE:
      panic("Unexpected LEAVE message");
      break;
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_MUTE:
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_SPEAKERS_CHANGED:
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_ROOM_UPDATE:
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE__NOT_SET:
    case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_UPDATE:
      break;
    default:
      ESP_LOGI(LOG_TAG, "Unknown message type received.");
  }
}

static void event_handler(void *handler_args, esp_event_base_t base,
                          int32_t event_id, void *event_data) {
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(LOG_TAG, "WEBSOCKET_EVENT_CONNECTED");
      // add_track(TRACK_NAME, TRACK_NAME, LIVEKIT__TRACK_SOURCE__MICROPHONE);
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGI(LOG_TAG, "WEBSOCKET_EVENT_DISCONNECTED");
      panic("LiveKit websocket disconnected");
      break;
    case WEBSOCKET_EVENT_DATA: {
      if (data->op_code != 0x2) {
        ESP_LOGD(LOG_TAG, "Message, opcode=%d, len=%d", data->op_code,
                 data->data_len);
        return;
      }

      auto new_response = livekit__signal_response__unpack(
          NULL, data->data_len, (uint8_t *)data->data_ptr);
      if (new_response == NULL) {
        panic("Failed to decode SignalResponse message.");
      }

      handle_livekit_response(new_response);
      livekit__signal_response__free_unpacked(new_response, NULL);
      break;
    }
    case WEBSOCKET_EVENT_ERROR:
      panic("LiveKit websocket error");
  }
}

void lk_websocket(const char *room_url, const char *token) {
  char *ws_uri = (char *)malloc(WEBSOCKET_URI_SIZE);
  snprintf(ws_uri, WEBSOCKET_URI_SIZE,
           "%s/rtc?protocol=%d&access_token=%s&auto_subscribe=true", room_url,
           LIVEKIT_PROTOCOL_VERSION, token);

  lk_sub_create(on_sub_state, on_sub_signal);
  lk_pub_create(on_pub_state, on_pub_signal);

  esp_websocket_client_config_t ws_cfg;
  memset(&ws_cfg, 0, sizeof(ws_cfg));
  ws_cfg.uri = ws_uri;
  ws_cfg.buffer_size = WEBSOCKET_BUFFER_SIZE;
  ws_cfg.disable_pingpong_discon = true;
  ws_cfg.reconnect_timeout_ms = 5000;
  ws_cfg.network_timeout_ms = 5000;

  g_client = esp_websocket_client_init(&ws_cfg);
  esp_websocket_register_events(g_client, WEBSOCKET_EVENT_ANY, event_handler,
                                NULL);
  esp_websocket_client_start(g_client);
  free(ws_uri);

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
