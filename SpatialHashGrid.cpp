#include "SpatialHashGrid.h"
#include <algorithm>

const std::vector<entt::entity> SpatialHashGrid::empty_;

uint32_t SpatialHashGrid::GetOrCreateZoneId(const std::string& location_name) {
    if (location_name.empty()) {
        return 0; // 유효하지 않은 구역
    }
    auto it = name_to_zone_.find(location_name);
    if (it != name_to_zone_.end()) {
        return it->second;
    }
    uint32_t zone_id = next_zone_id_++;
    name_to_zone_[location_name] = zone_id;
    return zone_id;
}

void SpatialHashGrid::Insert(entt::entity e, uint32_t zone_id) {
    if (zone_id == 0) return;
    auto& list = zone_entities_[zone_id];
    if (std::find(list.begin(), list.end(), e) == list.end()) {
        list.push_back(e);
    }
}

void SpatialHashGrid::Move(entt::entity e, uint32_t old_zone, uint32_t new_zone) {
    if (old_zone == new_zone) return;
    Remove(e, old_zone);
    Insert(e, new_zone);
}

void SpatialHashGrid::Remove(entt::entity e, uint32_t zone_id) {
    if (zone_id == 0) return;
    auto it = zone_entities_.find(zone_id);
    if (it != zone_entities_.end()) {
        auto& list = it->second;
        list.erase(std::remove(list.begin(), list.end(), e), list.end());
    }
}

const std::vector<entt::entity>& SpatialHashGrid::GetEntitiesInZone(uint32_t zone_id) const {
    if (zone_id == 0) return empty_;
    auto it = zone_entities_.find(zone_id);
    if (it != zone_entities_.end()) {
        return it->second;
    }
    return empty_;
}
