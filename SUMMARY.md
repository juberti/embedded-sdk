# Summary

An overview of the code in this project:

- ultravox.cpp: does a REST request to create a call, and then a websocket request to join the call, returning a URL and token that can be used by LiveKit.
- livekit.cpp: contains the basic protocol logic for managing a LiveKit websocket connection. Uses the info from ultravox, and then starts the individual subscriber and publisher tasks.
- lk_sub.cpp: contains the logic for managing the inbound connection from LiveKit. Receives an incoming offer, generates an outgoing answer, and then decodes the audio and sends it to the playout device.
- lk_pub.cpp: contains the logic for managing the outbound connection to LiveKit. Handles an outgoing offer and an incomnig answer, and then encodes audio from the playout device, and sends it to LiveKit.
- media.cpp: ES8311-based audio capture and playout, manages an inbound and outbound 16KHz stereo audio stream. (This is essentially a port of the aiphone echo sample code.)
- wifi.cpp: ESP-IDF wifi handling, connects to the access point indicated by the WIFI_SSID and WIFI_PASSWORD environment variables.
- main.cpp: entry point for the program, initializes the wifi and audio handling, and then kicks off the ultravox logic.

# Running the code

- Set the WIFI_SSID and WIFI_PASSWORD environment variables.
- Set the UVAPI_API_KEY environment variable with your Ultravox API key (there's a specific one for this project, starting with j37R).
- Build with idf.py build, flash with idf.py flash, and monitor with idf.py monitor.

# Known issues

- Electrical noise on the ESP32-S3 microphone input. This seems to be related to the wifi module, I didn't test super closely but I couldn't hear this noise in the aiphone echo sample.
- Occasional failures to connect to the ultravox servers. This seems to be related to the audio module, almost like when the audio pipeline is running, the network module doesn't get enough processing time.
  When the audio module is not inited (via lk_audio_init), I don't see these errors (typically issues with the REST request failing).
- Occasional watchdog timeouts, especially on the publisher task. Some more debugging is needed here, it seems like something is blocking in the task loop but I can't pin down what.
- It takes a while to set up a call (5-10 seconds from when wifi is connected to when the call is set up). Some of this may be related to the audio/network issue mentioned above, as
  things progress much faster when the audio module is not inited, so it may be possible to bring this down substantially.
  There are also some othe potential optimizations, e.g. starting the publisher protocol (add_track) immediately on websocket open rather than waiting for the subscriber to connect.
  Pre-loading the Ultravox REST request could also help. I didn't get a chance to really dig into this as I was hampered by the overall network slowness issue.
  Ultimately the right thing to do is probably to play some ringback audio from a local file while the call is setting up

# TODOs

- Add support for the hook switch. This can be used as an indicator to start warming up the call (and perhaps only actually taking the final action, doing add_track, when the dial button is pressed).
- Add support for the dial button.
- Verify that every HTTP operation has some sort of retry logic (maybe less necessary if the network issue is fixed).
- Tune out the gain and volume settings.
- ICE restart handling for network blips. Right now we reset the device if any connection drops, but some of these are likely recoverable.
- The libpeer SCTP module doesn't output binary payloads at the moment. They would be helpful for debugging, especially the ASR transcripts, so this would be good to add.

# Changes vs the baseline code:

- Most of the code has been rewritten vs the embedded-SDK head to try to reduce the number of moving parts, and also support use of ES8311. It would be nice to rework the audio module to allow ES8311 to be optional, i.e., when using a Sonatino board.
- For libpeer, 2 specific changes have been made:
  - #define KEEPALIVE_CONNCHECK 0 disables the expectation that the remote peer will send continuous ICE consent checks, which are not required. The right behavior here would be to reset the timeout on any inbound packet, not just a connectivity check, but this was much easier.
  - #define AGENT_POLL_TIMEOUT 0 avoids any sleeping when doing the pc event loop, which caused the audio encode pipeline to fall behind.

# Other notes:

- The error message "ERROR ./deps/libpeer/src/dtls_srtp.c 331 failed! mbedtls_ssl_handshake returned -0x7280" simply means that packets were received prior to the DTLS handshake completing. It's unclear exactly how though.
- To minimize latency, the device is set to send audio every 20 ms. We could do 60 or 120 ms, as Opus supports those, if we needed more wiggle room on performance. Opus encode only takes about 5ms for a 20ms packet though, so I think we're probably OK here.
- The choice of stack size for the various tasks is incredibly important. I spent a lot of time debugging various malfunctions only to find they resolved when I increased the stack size.
- The threshold for when to use offboard RAM is also very important. If set too high, the internal RAM (120KB or so???) will be exhausted quickly and things will crash. 4K seemed to work OK as a threshold.
- Reviewing call histories from the admin server is useful for verifying the captured audio quality.
