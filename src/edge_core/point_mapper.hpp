#pragma once
#include "point_registry.hpp"
#include "unified_message_v2.hpp"
namespace mqmgateway::edge {
class PointMapper {
public:
    static bool apply(UnifiedMessageV2& message, const PointDefinition& point);
};
}
