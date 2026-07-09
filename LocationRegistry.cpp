#include "LocationRegistry.h"
#include <iostream>

uint32_t LocationRegistry::GetOrCreateZoneId(const std::string& name) {
    auto it = name_to_zone_.find(name);
    if (it != name_to_zone_.end()) {
        return it->second;
    }
    uint32_t zone_id = next_zone_id_++;
    name_to_zone_[name] = zone_id;
    return zone_id;
}

void LocationRegistry::RegisterLocation(const std::string& name, float x, float z,
                                        LocationType type, uint32_t region_id,
                                        uint32_t territory_id) {
    location_centers_[name] = {x, z, type, region_id, territory_id};
    // 거점 등록 시 Zone ID도 미리 매핑 생성
    GetOrCreateZoneId(name);
}

void LocationRegistry::UpdateEntityPosition(entt::entity e, float x, float z, entt::registry& reg) {
    // 1. 셀 키 갱신
    uint64_t new_cell = GetCellKey(x, z);
    bool cell_changed = true;
    
    auto it = entity_to_cell_.find(e);
    if (it != entity_to_cell_.end()) {
        if (it->second == new_cell) {
            cell_changed = false;
        } else {
            // 이전 셀에서 엔티티 제거
            auto& old_vec = cell_entities_[it->second];
            old_vec.erase(std::remove(old_vec.begin(), old_vec.end(), e), old_vec.end());
        }
    }

    if (cell_changed) {
        entity_to_cell_[e] = new_cell;
        cell_entities_[new_cell].push_back(e);
    }

    // 2. 가장 가까운 거점 파악 및 LocationComp 갱신
    if (reg.all_of<LocationComp>(e)) {
        auto& loc = reg.get<LocationComp>(e);
        
        float min_dist = 999999.0f;
        std::string closest_loc = "Wilderness";

        for (const auto& [loc_name, coords] : location_centers_) {
            float dx = coords.x - x;
            float dz = coords.z - z;
            float dist = std::sqrt(dx * dx + dz * dz);
            if (dist < min_dist) {
                min_dist = dist;
                closest_loc = loc_name;
            }
        }

        std::string resolved_loc = (min_dist <= LOCATION_RADIUS) ? closest_loc : "Wilderness";
        uint32_t resolved_zone = GetOrCreateZoneId(resolved_loc);
        
        if (loc.location_name != resolved_loc || loc.zone_id != resolved_zone) {
            std::cout << "🔀 [구역 동적 진입] 엔티티 " << (int)e 
                      << " 이동: [" << loc.location_name << "] ➔ [" << resolved_loc << "]" << std::endl;
            loc.location_name = resolved_loc;
            loc.zone_id = resolved_zone;

            // 새 구역의 정적 메타데이터 갱신 (type, region_id, territory_id)
            auto meta_it = location_centers_.find(resolved_loc);
            if (meta_it != location_centers_.end()) {
                loc.type = meta_it->second.type;
                loc.region_id = meta_it->second.region_id;
                loc.territory_id = meta_it->second.territory_id;
            } else {
                // Wilderness 또는 미등록 구역: 기본값으로 초기화
                loc.type = (resolved_loc == "Wilderness") ? LocationType::Wilderness : LocationType::Unspecified;
                loc.region_id = 0;
                loc.territory_id = 0;
            }
        }
        
        // x, z도 일괄 동기화
        loc.x = x;
        loc.z = z;
    }
}

void LocationRegistry::RemoveEntity(entt::entity e) {
    auto it = entity_to_cell_.find(e);
    if (it != entity_to_cell_.end()) {
        auto& old_vec = cell_entities_[it->second];
        old_vec.erase(std::remove(old_vec.begin(), old_vec.end(), e), old_vec.end());
        entity_to_cell_.erase(it);
    }
}

std::vector<entt::entity> LocationRegistry::GetNearbyEntities(float x, float z, float radius, const entt::registry& reg) const {
    std::vector<entt::entity> result;
    int cx = static_cast<int>(std::floor(x / CELL_SIZE));
    int cz = static_cast<int>(std::floor(z / CELL_SIZE));

    // 3x3 주변 셀 탐색
    for (int i = -1; i <= 1; ++i) {
        for (int j = -1; j <= 1; ++j) {
            uint64_t cell_key = (static_cast<uint64_t>(static_cast<uint32_t>(cx + i)) << 32) | static_cast<uint32_t>(cz + j);
            auto it = cell_entities_.find(cell_key);
            if (it != cell_entities_.end()) {
                for (entt::entity e : it->second) {
                    if (!reg.valid(e) || !reg.all_of<LocationComp>(e)) continue;
                    
                    const auto& loc = reg.get<LocationComp>(e);
                    float dx = loc.x - x;
                    float dz = loc.z - z;
                    float dist_sq = dx * dx + dz * dz;
                    
                    if (dist_sq <= radius * radius) {
                        result.push_back(e);
                    }
                }
            }
        }
    }
    return result;
}

