#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include "MundusVivensClient.h"

// Available locations and NPC lists
const std::vector<std::string> Locations = { "성당 (Church)", "술집 (Tavern)", "광장 (Square)" };
const std::vector<std::string> NpcIds = { "npc_kyle", "npc_eva", "npc_bart" };

std::map<std::string, std::string> NpcNames = {
    { "npc_kyle", "카일" },
    { "npc_eva", "에바" },
    { "npc_bart", "바르트" }
};

std::map<std::string, std::string> CurrentLocations = {
    { "npc_kyle", "성당 (Church)" },
    { "npc_eva", "술집 (Tavern)" },
    { "npc_bart", "술집 (Tavern)" }
};

std::map<std::string, std::string> CurrentActivities = {
    { "npc_kyle", "기도 중" },
    { "npc_eva", "맥주 컵 청소" },
    { "npc_bart", "술 마시기" }
};

// Cooldown dictionary for dialogues (Key: sorted NPC pair joined by ":", Value: tick number when they can talk again)
std::map<std::string, int> DialogueCooldowns;

// Helper to generate sorted cooldown key
std::string GetCooldownKey(const std::string& a, const std::string& b) {
    std::string first = a;
    std::string second = b;
    if (first > second) {
        std::swap(first, second);
    }
    return first + ":" + second;
}

int main() {
#ifdef _WIN32
    // Windows 콘솔 한글 깨짐 방지 (UTF-8 설정)
    system("chcp 65001 > nul");
#endif

    std::cout << "=======================================================" << std::endl;
    std::cout << "🎮 Mundus Vivens — C++ Game Server Simulator Console" << std::endl;
    std::cout << "=======================================================\n" << std::endl;

    // Connect to C# AI gRPC server
    const std::string server_address = "localhost:5001";
    std::cout << "[C++ Server] Connecting to C# AI gRPC server at " << server_address << "..." << std::endl;
    
    MundusVivens::MundusVivensClient client(server_address);
    std::cout << "[C++ Server] Channel initialized successfully." << std::endl;
    std::cout << "[C++ Server] Starting simulation loop (5s interval). Press Ctrl+C to stop.\n" << std::endl;

    // Setup random generator
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0.0, 1.0);
    std::uniform_int_distribution<> loc_dis(0, Locations.size() - 1);

    int tick = 0;

    while (true) {
        tick++;
        std::cout << "\n================== [ C++ TICK " << tick << " ] ==================" << std::endl;

        // 1. World Tick Notification
        std::string out_msg;
        bool success = client.ProcessWorldTick(tick, out_msg);
        if (success) {
            std::cout << "⏱️ [Tick Sync] Tick " << tick << " successfully sent to C# server. Msg: " << out_msg << std::endl;
        } else {
            std::cerr << "❌ [Tick Sync Error] Failed to connect or sync tick with C# server: " << out_msg << std::endl;
            std::cout << "[C++ Server] Re-trying in 5 seconds..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        // 2. Simulating movement for each NPC
        for (const auto& npc_id : NpcIds) {
            // 30% probability of moving
            if (dis(gen) < 0.3) {
                std::string old_loc = CurrentLocations[npc_id];
                std::string new_loc = Locations[loc_dis(gen)];
                CurrentLocations[npc_id] = new_loc;

                // Update activity based on location
                if (new_loc.find("Church") != std::string::npos) {
                    CurrentActivities[npc_id] = "예배 참여 및 침묵 명상";
                } else if (new_loc.find("Tavern") != std::string::npos) {
                    CurrentActivities[npc_id] = "술집 테이블 정리 및 잡담";
                } else {
                    CurrentActivities[npc_id] = "광장 게시판 구경";
                }

                std::cout << "🏃 [Movement] " << NpcNames[npc_id] << " moved: [" << old_loc << "] ➔ [" << new_loc << "] (Activity: " << CurrentActivities[npc_id] << ")" << std::endl;
            } else {
                std::cout << "🧍 [Status] " << NpcNames[npc_id] << " remains at [" << CurrentLocations[npc_id] << "] (Activity: " << CurrentActivities[npc_id] << ")" << std::endl;
            }

            // Sync updated status to C# AI server
            std::string status_msg;
            client.UpdateAgentStatus(npc_id, CurrentLocations[npc_id], "평온함", CurrentActivities[npc_id], status_msg);
        }

        // 3. Check for overlaps and trigger dialogues
        for (size_t i = 0; i < NpcIds.size(); ++i) {
            for (size_t j = i + 1; j < NpcIds.size(); ++j) {
                std::string npcA = NpcIds[i];
                std::string npcB = NpcIds[j];

                // Check if they are in the same location
                if (CurrentLocations[npcA] == CurrentLocations[npcB]) {
                    std::string cooldown_key = GetCooldownKey(npcA, npcB);

                    // Check cooldown
                    if (DialogueCooldowns.find(cooldown_key) != DialogueCooldowns.end() && tick < DialogueCooldowns[cooldown_key]) {
                        continue; // Still on cooldown
                    }

                    // 50% chance to trigger dialogue
                    if (dis(gen) < 0.5) {
                        std::cout << "\n💬 [C++ Overlap Alert] " << NpcNames[npcA] << " and " << NpcNames[npcB] << " met at [" << CurrentLocations[npcA] << "]!" << std::endl;
                        std::cout << "💬 Triggering dialogue via gRPC call to C# AI server..." << std::endl;

                        auto result = client.TriggerDialogue(npcA, npcB, true);
                        if (result.dialogue_lines.empty()) {
                            std::cerr << "❌ [Dialogue Error] Dialogue trigger returned no lines." << std::endl;
                        } else {
                            std::cout << "\n================== [ C++ DIALOGUE SUMMARY ] ==================" << std::endl;
                            std::cout << result.dialogue_summary << std::endl;
                            std::cout << "==============================================================" << std::endl;
                            
                            std::cout << "\n[Dialogue Detail Logs]" << std::endl;
                            for (const auto& line : result.dialogue_lines) {
                                std::cout << line << std::endl;
                            }
                            std::cout << "==============================================================\n" << std::endl;

                            // Reset activities
                            CurrentActivities[npcA] = "대화 마침";
                            CurrentActivities[npcB] = "대화 마침";

                            // Set dialogue cooldown (5 ticks)
                            DialogueCooldowns[cooldown_key] = tick + 5;
                        }
                    }
                }
            }
        }

        // Wait for 5 seconds
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    return 0;
}
