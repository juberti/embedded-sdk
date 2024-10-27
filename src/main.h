#include <peer.h>

#include <string>

#define LOG_TAG "embedded-sdk"

struct CallRequest {
  std::string system_prompt;
  std::string voice;
};

typedef void (*StateCallback)(PeerConnectionState state);
typedef void (*SignalCallback)(const char *type, const char *message);

void uv_run(const CallRequest &request, const char *api_key);

void lk_wifi(void);
void lk_websocket(const char *url, const char *token);

void lk_pub_create(StateCallback state_cb, SignalCallback signal_cb);
void lk_pub_reoffer();
void lk_pub_set_remote_description(char *sdp);
void lk_pub_add_ice_candidate(char *description);

void lk_sub_create(StateCallback state_cb, SignalCallback signal_cb);
void lk_sub_set_remote_description(char *sdp);
void lk_sub_add_ice_candidate(char *description);

void lk_init_audio(void);
const int16_t *lk_capture_audio(size_t *bytes);
void lk_render_audio(const int16_t *data, size_t bytes);

void panic(const char *msg);
