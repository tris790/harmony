#ifndef HARMONY_PROTOCOL_H
#define HARMONY_PROTOCOL_H

#include "../memory_arena.h"
#include <string.h>
#include <stdbool.h>
#include <unistd.h> // for usleep

// Max UDP payload size (safe MTU - header)
// MTU 1500 - IP(20) - UDP(8) = 1472. Let's stay safe with 1400.
#define MAX_PACKET_PAYLOAD 1400

// Packet Types
typedef enum PacketType {
    PACKET_TYPE_VIDEO = 0,
    PACKET_TYPE_METADATA = 1,
    PACKET_TYPE_KEEPALIVE = 2,
    PACKET_TYPE_PUNCH = 3,      // UDP hole punch packet
    PACKET_TYPE_AUDIO = 4       // Opus-encoded audio
} PacketType;

typedef struct PacketHeader {
    uint32_t frame_id;      // Unique ID for the logical unit (monotonic)
    uint16_t chunk_id;      // 0 to total_chunks-1
    uint16_t total_chunks;
    uint32_t payload_size;  // Size of data in this chunk
    uint8_t  packet_type;   // PacketType
    uint8_t  padding[3];    // Alignment
} PacketHeader;

typedef struct StreamMetadata {
    char os_name[32];
    char de_name[32];
    uint32_t screen_width;
    uint32_t screen_height;
    uint32_t fps;
    char format_name[16]; // e.g. "BGRx"
    char color_space[16]; // e.g. "sRGB"
} StreamMetadata;

// --- Packetizer (Sender) ---

typedef struct Packetizer {
    uint32_t frame_id_counter;
} Packetizer;

// Callback function type for sending packets
typedef void (*SendPacketCallback)(void *user_data, void *packet_data, size_t packet_size);

static void Protocol_SendData(Packetizer *pz, uint8_t type, void *data, size_t size, SendPacketCallback send_fn, void *user_data) {
    pz->frame_id_counter++;
    uint32_t current_frame_id = pz->frame_id_counter;

    uint16_t total_chunks = (uint16_t)((size + MAX_PACKET_PAYLOAD - 1) / MAX_PACKET_PAYLOAD);
    uint8_t *data_bytes = (uint8_t *)data;
    size_t bytes_remaining = size;
    size_t offset = 0;

    for (uint16_t i = 0; i < total_chunks; ++i) {
        size_t chunk_size = (bytes_remaining > MAX_PACKET_PAYLOAD) ? MAX_PACKET_PAYLOAD : bytes_remaining;

        // Construct Packet
        uint8_t buffer[sizeof(PacketHeader) + MAX_PACKET_PAYLOAD];
        PacketHeader *header = (PacketHeader *)buffer;
        
        header->frame_id = current_frame_id;
        header->chunk_id = i;
        header->total_chunks = total_chunks;
        header->payload_size = (uint32_t)chunk_size;
        header->packet_type = type;
        memset(header->padding, 0, 3);

        memcpy(buffer + sizeof(PacketHeader), data_bytes + offset, chunk_size);
        
        // Send
        send_fn(user_data, buffer, sizeof(PacketHeader) + chunk_size);

        offset += chunk_size;
        bytes_remaining -= chunk_size;

        // PACING: Sleep every few packets to prevent flooding UDP buffer
        // 192KB frame = ~137 packets. 
        // Sending all back-to-back causes drops.
        if (i > 0 && i % 10 == 0) {
            usleep(200); // 0.2ms pause every 10 packets
        }
    }
}

static void Protocol_SendFrame(Packetizer *pz, void *frame_data, size_t frame_size, SendPacketCallback send_fn, void *user_data) {
    Protocol_SendData(pz, PACKET_TYPE_VIDEO, frame_data, frame_size, send_fn, user_data);
}

static void Protocol_SendMetadata(Packetizer *pz, StreamMetadata *meta, SendPacketCallback send_fn, void *user_data) {
    Protocol_SendData(pz, PACKET_TYPE_METADATA, meta, sizeof(StreamMetadata), send_fn, user_data);
}

static void Protocol_SendAudio(Packetizer *pz, void *audio_data, size_t audio_size, SendPacketCallback send_fn, void *user_data) {
    Protocol_SendData(pz, PACKET_TYPE_AUDIO, audio_data, audio_size, send_fn, user_data);
}

// Send a minimal keepalive packet (header only, no payload)
static void Protocol_SendKeepalive(Packetizer *pz, SendPacketCallback send_fn, void *user_data) {
    pz->frame_id_counter++;
    
    uint8_t buffer[sizeof(PacketHeader)];
    PacketHeader *header = (PacketHeader *)buffer;
    
    header->frame_id = pz->frame_id_counter;
    header->chunk_id = 0;
    header->total_chunks = 1;
    header->payload_size = 0;  // No payload
    header->packet_type = PACKET_TYPE_KEEPALIVE;
    memset(header->padding, 0, 3);
    
    send_fn(user_data, buffer, sizeof(PacketHeader));
}

// Send a UDP hole punch packet (opens firewall for return traffic)
static void Protocol_SendPunch(Packetizer *pz, SendPacketCallback send_fn, void *user_data) {
    pz->frame_id_counter++;
    
    uint8_t buffer[sizeof(PacketHeader)];
    PacketHeader *header = (PacketHeader *)buffer;
    
    header->frame_id = pz->frame_id_counter;
    header->chunk_id = 0;
    header->total_chunks = 1;
    header->payload_size = 0;  // No payload
    header->packet_type = PACKET_TYPE_PUNCH;
    memset(header->padding, 0, 3);
    
    send_fn(user_data, buffer, sizeof(PacketHeader));
}

// --- Reassembler (Receiver) ---

typedef struct ReassemblyBuffer {
    uint32_t frame_id;
    uint8_t *data;
    size_t total_size;
    size_t received_bytes;
    uint8_t packet_type; 
} ReassemblyBuffer;

typedef struct Reassembler {
    ReassemblyBuffer active_buffer; // Supports 1 active frame reassembly
    MemoryArena *arena; // To allocate the large frame buffer
} Reassembler;

// Result of processing a packet
typedef enum ReassemblyResult {
    RESULT_PARTIAL,
    RESULT_COMPLETE,
    RESULT_IGNORED
} ReassemblyResult;

static void Reassembler_Init(Reassembler *r, MemoryArena *arena) {
    r->arena = arena;
    r->active_buffer.frame_id = 0;
    r->active_buffer.data = NULL;
}

static ReassemblyResult Protocol_HandlePacket(Reassembler *r, void *packet_data, size_t packet_size, void **out_data, size_t *out_size, uint8_t *out_type) {
    if (packet_size < sizeof(PacketHeader)) return RESULT_IGNORED;

    PacketHeader *header = (PacketHeader *)packet_data;
    uint8_t *payload = (uint8_t *)packet_data + sizeof(PacketHeader);

    // Check if this is a new frame (or metadata unit)
    if (header->frame_id > r->active_buffer.frame_id) {
        // New logical unit started
        r->active_buffer.frame_id = header->frame_id;
        r->active_buffer.received_bytes = 0;
        r->active_buffer.packet_type = header->packet_type;
        
        // Ensure buffer exists.
        // We always use a 2MB buffer for simplicity, which covers both Video and Metadata.
        if (!r->active_buffer.data) {
            r->active_buffer.data = ArenaPush(r->arena, 2 * 1024 * 1024);
        }
        
        r->active_buffer.total_size = 0; 
    }

    if (header->frame_id == r->active_buffer.frame_id) {
        size_t offset = header->chunk_id * MAX_PACKET_PAYLOAD;
        if (offset + header->payload_size > 2 * 1024 * 1024) return RESULT_IGNORED;

        memcpy(r->active_buffer.data + offset, payload, header->payload_size);
        r->active_buffer.received_bytes += header->payload_size;
        
        if (header->chunk_id == header->total_chunks - 1) {
             size_t expected_size = (header->total_chunks - 1) * MAX_PACKET_PAYLOAD + header->payload_size;
             r->active_buffer.total_size = expected_size;
        }

        if (r->active_buffer.total_size > 0 && r->active_buffer.received_bytes >= r->active_buffer.total_size) {
            *out_data = r->active_buffer.data;
            *out_size = r->active_buffer.total_size;
            if (out_type) *out_type = r->active_buffer.packet_type;
            return RESULT_COMPLETE;
        }
        
        return RESULT_PARTIAL;
    }

    return RESULT_IGNORED;
}

#endif // HARMONY_PROTOCOL_H
