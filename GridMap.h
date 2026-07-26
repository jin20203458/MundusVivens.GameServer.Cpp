#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include "MundusVivensClient.h"

struct GridVector2 {
    float x = 0.0f;
    float z = 0.0f;

    bool operator==(const GridVector2& other) const {
        return x == other.x && z == other.z;
    }
};

struct PathResult {
    std::vector<GridVector2> waypoints;
    bool is_partial = false;
    bool is_failed = false;
};

// Phase 3: Cluster Cache 엔트리
struct PathCacheEntry {
    std::vector<GridVector2> waypoints;
    int tick_computed = 0;
};

class GridMap {
public:
    static constexpr int WIDTH = 2000;
    static constexpr int HEIGHT = 2000;

    GridMap();
    void LoadMap(const std::vector<MundusVivens::LocationData>& locations);
    bool IsWalkable(int x, int z) const;
    
    // 거점 이름으로 좌표 조회
    bool GetLocationCoords(const std::string& loc_name, float& out_x, float& out_z) const;

    // A* 길찾기 알고리즘 (PathResult 반환, max_iterations 지정 가능)
    PathResult FindPath(float start_x, float start_z, float end_x, float end_z, int max_iterations = 10000) const;

    // 두 지점 사이에 장애물이 있는지 확인 (시야(LOS) 판정용)
    bool IsPathBlocked(float x1, float z1, float x2, float z2) const;

    // 캐시 관리
    void ClearCache() const { path_cache_.clear(); }

private:
    std::vector<bool> grid_; // 실제 격자맵
    std::unordered_map<std::string, GridVector2> location_coords_;

    // Phase 3: Destination Cluster Cache (8x8 타일 버킷 양자화)
    struct PathCacheKey {
        int sx_bucket, sz_bucket, ex_bucket, ez_bucket;
        bool operator==(const PathCacheKey& other) const {
            return sx_bucket == other.sx_bucket && sz_bucket == other.sz_bucket &&
                   ex_bucket == other.ex_bucket && ez_bucket == other.ez_bucket;
        }
    };
    struct PathCacheKeyHash {
        std::size_t operator()(const PathCacheKey& k) const {
            std::size_t h1 = std::hash<int>()(k.sx_bucket);
            std::size_t h2 = std::hash<int>()(k.sz_bucket);
            std::size_t h3 = std::hash<int>()(k.ex_bucket);
            std::size_t h4 = std::hash<int>()(k.ez_bucket);
            return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
        }
    };
    mutable std::unordered_map<PathCacheKey, PathCacheEntry, PathCacheKeyHash> path_cache_;
};
