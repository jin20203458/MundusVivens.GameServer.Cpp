#pragma once
#include <unordered_map>
#include <vector>
#include <string>
#include <entt/entt.hpp>

class SpatialHashGrid {
public:
    uint32_t GetOrCreateZoneId(const std::string& location_name);
    void Insert(entt::entity e, uint32_t zone_id);
    void Move(entt::entity e, uint32_t old_zone, uint32_t new_zone);
    void Remove(entt::entity e, uint32_t zone_id);
    const std::vector<entt::entity>& GetEntitiesInZone(uint32_t zone_id) const;
    const std::unordered_map<uint32_t, std::vector<entt::entity>>& AllZones() const { return zone_entities_; }

private:
    std::unordered_map<std::string, uint32_t> name_to_zone_;
    std::unordered_map<uint32_t, std::vector<entt::entity>> zone_entities_;
    uint32_t next_zone_id_ = 1; // 0은 유효하지 않거나 기본값으로 사용 가능
    static const std::vector<entt::entity> empty_;
};
