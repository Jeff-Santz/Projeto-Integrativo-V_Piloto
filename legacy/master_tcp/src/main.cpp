#include <iostream>
#include <thread>
#include "network/NodeDispatcher.hpp"
#include "core/IntersectionManager.hpp"

int main() {
    NodeDispatcher dispatcher(5000); // Master listens on port 5000
    IntersectionManager manager(&dispatcher);

    std::cout << "[SYSTEM] Waiting for ESP32 connections...\n";
    
    // Blocks here until all required nodes are connected
    // The ESP sends a string like: "REGISTER:A1:B2:C3:D4:E5:F6:S1"
    while (!dispatcher.allNodesRegistered()) {
        dispatcher.listenForRegistrations();
    }

    std::cout << "[SYSTEM] Hardware mapped. Initializing Security Matrix.\n";
    
    // Configures the matrix. Ex: S1 (Bit 0) cannot turn green with S2 (Bit 1), but can with S3 (Bit 2). The numbers in here represents the position of the bits in the mask, not the node IDs.
    // Mask to turn S1 green: Monitor only S2. Therefore, mask = 0000 0010 (0x02).
    manager.setupConflictRules(0, 0x02); // Rule for Phase 1
    
    // Starts the asynchronous network thread
    std::thread netThread(&NodeDispatcher::runAsyncLoop, &dispatcher);
    
    // Starts the traffic loop on the main thread
    manager.startTrafficLoop();

    netThread.join();
    return 0;
}