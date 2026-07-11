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

class GridMap {
public:
    static constexpr int WIDTH = 2000;
    static constexpr int HEIGHT = 2000;

    GridMap();
    void LoadMap(const std::vector<MundusVivens::LocationData>& locations);
    bool IsWalkable(int x, int z) const;
    
    // 거점 이름으로 좌표 조회
    bool GetLocationCoords(const std::string& loc_name, float& out_x, float& out_z) const;

    // A* 길찾기 알고리즘 (float 입력을 받아 타일 단위로 연산 후 Vector2 경로 리턴)
    std::vector<GridVector2> FindPath(float start_x, float start_z, float end_x, float end_z) const;

    // 두 지점 사이에 장애물이 있는지 확인 (시야(LOS) 판정용)
    bool IsPathBlocked(float x1, float z1, float x2, float z2) const;

private:
    std::vector<bool> grid_; // 실제 격자맵
    std::unordered_map<std::string, GridVector2> location_coords_;
};
