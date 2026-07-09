#pragma once
#include <unordered_map>
#include <vector>
#include <string>
#include <entt/entt.hpp>
#include <cmath>
#include <algorithm>
#include "Components.h"

struct Vector2f {
    float x = 0.0f;
    float z = 0.0f;
};

class LocationRegistry {
public:
    static constexpr float CELL_SIZE = 8.0f;
    static constexpr float LOCATION_RADIUS = 8.0f;

    uint32_t GetOrCreateZoneId(const std::string& name);

    void RegisterLocation(const std::string& name, float x, float z);

    void UpdateEntityPosition(entt::entity e, float x, float z, entt::registry& reg);
    
    void RemoveEntity(entt::entity e);

    std::vector<entt::entity> GetNearbyEntities(float x, float z, float radius, const entt::registry& reg) const;


    static uint64_t GetCellKey(float x, float z) {
        int cx = static_cast<int>(std::floor(x / CELL_SIZE));
        int cz = static_cast<int>(std::floor(z / CELL_SIZE));
        return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cz);
    }

private:
    std::unordered_map<std::string, Vector2f> location_centers_;
    std::unordered_map<uint64_t, std::vector<entt::entity>> cell_entities_;
    std::unordered_map<entt::entity, uint64_t> entity_to_cell_;
    
    // Zone ID mapping
    std::unordered_map<std::string, uint32_t> name_to_zone_;
    uint32_t next_zone_id_ = 1;
};
