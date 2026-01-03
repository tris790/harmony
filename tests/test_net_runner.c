#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "../src/memory_arena.h"
#include "../src/net/protocol.h"

// Mock Sender
typedef struct MockNetwork {
    Reassembler *receiver;
    int packets_sent;
    int packets_dropped; // Simulating loss
} MockNetwork;

void MockSendCallback(void *user_data, void *packet_data, size_t packet_size) {
    MockNetwork *net = (MockNetwork *)user_data;
    net->packets_sent++;

    // Feed directly to receiver
    void *frame_out = NULL;
    size_t size_out = 0;
    ReassemblyResult res = Protocol_HandlePacket(net->receiver, packet_data, packet_size, &frame_out, &size_out);
    
    if (res == RESULT_COMPLETE) {
        printf("MockNet: Frame Reassembled! Size: %zu\n", size_out);
        // Verify content
        uint8_t *data = (uint8_t *)frame_out;
        for (size_t i = 0; i < size_out; ++i) {
            if (data[i] != (uint8_t)(i % 255)) {
                printf("MockNet: DATA MISMATCH at index %zu! Expected %d, got %d\n", i, (uint8_t)(i % 255), data[i]);
                exit(1);
            }
        }
        printf("MockNet: DATA VERIFIED successfully.\n");
    }
}

int main() {
    printf("Starting Network Protocol Test...\n");

    MemoryArena arena;
    ArenaInit(&arena, 4 * 1024 * 1024);

    // Setup
    Packetizer pz = {0};
    Reassembler r = {0};
    Reassembler_Init(&r, &arena);

    MockNetwork mock_net = { .receiver = &r };

    // Create a Dummy Frame (larger than MTU 1400)
    size_t frame_size = 5000;
    uint8_t *frame_data = ArenaPush(&arena, frame_size);
    for (size_t i = 0; i < frame_size; ++i) {
        frame_data[i] = (uint8_t)(i % 255);
    }

    printf("Generated Frame of size %zu. Sending...\n", frame_size);

    // Send
    Protocol_SendFrame(&pz, frame_data, frame_size, MockSendCallback, &mock_net);

    // Verify
    printf("Sent %d packets.\n", mock_net.packets_sent);
    
    // Check if reassembler got it
    // In the mock callback we printed success. 
    // We can verify content if we kept the pointer.
    
    // Test Complete
    return 0;
}
