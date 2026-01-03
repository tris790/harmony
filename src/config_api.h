#ifndef HARMONY_CONFIG_API_H
#define HARMONY_CONFIG_API_H

#include <stdbool.h>
#include <stdint.h>

// Persistent configuration - saved between app sessions
typedef struct PersistentConfig {
    bool is_host;
    bool verbose;
    char target_ip[64];
    char stream_password[64];
    bool use_portal_audio;
    char encoder_preset[32]; // x264 preset: ultrafast, superfast, veryfast, faster, fast, medium
    uint32_t fps;
} PersistentConfig;

// Load config from OS-specific location. Returns false if file doesn't exist.
// Config will be filled with defaults if load fails.
bool Config_Load(PersistentConfig *config);

// Save config to OS-specific location. Creates directories if needed.
bool Config_Save(const PersistentConfig *config);

// Get the config file path (for debugging). Returns static buffer.
const char* Config_GetPath(void);

#endif // HARMONY_CONFIG_API_H
