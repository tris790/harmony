#include "../config_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

// Config file location: ~/.config/harmony/config.txt
static char g_config_path[512] = {0};

static const char* GetConfigPath(void) {
    if (g_config_path[0] != '\0') {
        return g_config_path;
    }
    
    const char *home = getenv("HOME");
    if (!home) {
        home = "/tmp";
    }
    
    snprintf(g_config_path, sizeof(g_config_path), "%s/.config/harmony/config.txt", home);
    return g_config_path;
}

static bool EnsureConfigDir(void) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    
    char dir_path[512];
    
    // Create ~/.config if needed
    snprintf(dir_path, sizeof(dir_path), "%s/.config", home);
    mkdir(dir_path, 0755); // Ignore error if exists
    
    // Create ~/.config/harmony if needed
    snprintf(dir_path, sizeof(dir_path), "%s/.config/harmony", home);
    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Config: Failed to create directory %s\n", dir_path);
        return false;
    }
    
    return true;
}

const char* Config_GetPath(void) {
    return GetConfigPath();
}

bool Config_Load(PersistentConfig *config) {
    // Set defaults first
    config->is_host = true;
    config->verbose = false;
    config->use_portal_audio = false;
    strcpy(config->target_ip, "127.0.0.1");
    strcpy(config->stream_password, "");
    strcpy(config->encoder_preset, "faster"); // Default to 'faster' for good quality/speed balance
    
    const char *path = GetConfigPath();
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("Config: No config file at %s (using defaults)\n", path);
        return false;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
            continue;
        }
        
        // Remove trailing newline
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        
        // Parse key=value
        char *eq = strchr(line, '=');
        if (!eq) continue;
        
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        
        if (strcmp(key, "is_host") == 0) {
            config->is_host = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "verbose") == 0) {
            config->verbose = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "target_ip") == 0) {
            strncpy(config->target_ip, value, sizeof(config->target_ip) - 1);
            config->target_ip[sizeof(config->target_ip) - 1] = '\0';
        } else if (strcmp(key, "use_portal_audio") == 0) {
            config->use_portal_audio = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "stream_password") == 0) {
            strncpy(config->stream_password, value, sizeof(config->stream_password) - 1);
            config->stream_password[sizeof(config->stream_password) - 1] = '\0';
        } else if (strcmp(key, "encoder_preset") == 0) {
            strncpy(config->encoder_preset, value, sizeof(config->encoder_preset) - 1);
            config->encoder_preset[sizeof(config->encoder_preset) - 1] = '\0';
        }
    }
    
    fclose(f);
    printf("Config: Loaded from %s (is_host=%s, verbose=%s, target_ip=%s)\n", 
           path, config->is_host ? "true" : "false", config->verbose ? "true" : "false", config->target_ip);
    return true;
}

bool Config_Save(const PersistentConfig *config) {
    if (!EnsureConfigDir()) {
        return false;
    }
    
    const char *path = GetConfigPath();
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Config: Failed to write %s\n", path);
        return false;
    }
    
    fprintf(f, "# Harmony Config\n");
    fprintf(f, "is_host=%s\n", config->is_host ? "true" : "false");
    fprintf(f, "verbose=%s\n", config->verbose ? "true" : "false");
    fprintf(f, "target_ip=%s\n", config->target_ip);
    fprintf(f, "stream_password=%s\n", config->stream_password);
    fprintf(f, "use_portal_audio=%s\n", config->use_portal_audio ? "true" : "false");
    fprintf(f, "# encoder_preset: ultrafast, superfast, veryfast, faster, fast, medium (slower = better quality)\n");
    fprintf(f, "encoder_preset=%s\n", config->encoder_preset);
    
    fclose(f);
    printf("Config: Saved to %s\n", path);
    return true;
}
