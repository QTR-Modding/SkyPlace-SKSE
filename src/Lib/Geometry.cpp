#include "Geometry.h"

#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")

UINT GetBufferLength(REX::W32::ID3D11Buffer* reBuffer) {
    auto buffer = reinterpret_cast<ID3D11Buffer*>(reBuffer);
    D3D11_BUFFER_DESC bufferDesc = {};
    buffer->GetDesc(&bufferDesc);
    return bufferDesc.ByteWidth;
}

RE::BSVisit::BSVisitControl TraverseScenegrap(RE::NiAVObject* a_object, std::function<RE::BSVisit::BSVisitControl(RE::NiAVObject*)> a_func) {
    auto result = RE::BSVisit::BSVisitControl::kContinue;

    if (!a_object) {
        return result;
    }

    if (a_object) {
        a_func(a_object);
    }

    auto node = a_object->AsNode();
    if (node) {
        for (auto& child : node->GetChildren()) {
            result = TraverseScenegrap(child.get(), a_func);
            if (result == RE::BSVisit::BSVisitControl::kStop) {
                break;
            }
        }
    }

    return result;
}

void EachGeometry(RE::NiAVObject* obj, std::function<void(RE::NiAVObject*)> callback) {
    TraverseScenegrap(obj, [callback](RE::NiAVObject* a_obj) -> RE::BSVisit::BSVisitControl {
        callback(a_obj);
        return RE::BSVisit::BSVisitControl::kContinue;
    });
}

void Geometry::FetchVertexes(RE::BSGeometry* o3d, RE::BSGraphics::TriShape* triShape) {
    if (const uint8_t* vertexData = triShape->rawVertexData) {
        if (!triShape->vertexBuffer || !o3d) {
            return;
        }
        uint32_t stride = triShape->vertexDesc.GetSize();
        auto numPoints = GetBufferLength(triShape->vertexBuffer);
        auto numPositions = numPoints / stride;
        positions.reserve(positions.size() + numPositions);
        for (auto i = 0; i < numPoints; i += stride) {
            const uint8_t* currentVertex = vertexData + i;

            const float* position = reinterpret_cast<const float*>(currentVertex + triShape->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION));

            if (!position) {
            } else {
                auto pos = o3d->world * RE::NiPoint3{position[0], position[1], position[2]};
                positions.push_back(pos);
            }
        }
    }
}
void Geometry::FetchBoneVertexes(RE::BSGeometry* o3d, RE::NiPointer<RE::NiSkinPartition>& partitions) {
    if (!partitions || !o3d) {
        return;
    }

    for (int p = 0; p < partitions->numPartitions; p++) {
        auto& partition = partitions->partitions[p];

        auto triShape = partition.buffData;
        if (!triShape || !triShape->rawVertexData || !triShape->vertexBuffer) {
            continue;
        }

        uint32_t stride = partition.vertexDesc.GetSize();
        uint32_t posOffset = partition.vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);

        for (uint16_t v = 0; v < partition.vertices; v++) {
            uint16_t mappedIndex = partition.vertexMap ? partition.vertexMap[v] : v;
            const uint8_t* vertexBase = triShape->rawVertexData + mappedIndex * stride;
            const float* position = reinterpret_cast<const float*>(vertexBase + posOffset);

            auto worldPos = o3d->world * RE::NiPoint3{position[0], position[1], position[2]};

            positions.push_back(worldPos);
        }
    }
}

void Geometry::FetchIndexes(RE::BSGraphics::TriShape* triShape) {
    auto numIndexes = GetBufferLength(triShape->indexBuffer) / sizeof(uint16_t);

    auto offset = indexes.size();
    indexes.reserve(indexes.size() + numIndexes);

    for (auto i = 0; i < numIndexes; i++) {
        const uint16_t* currentIndex = triShape->rawIndexData + i;
        indexes.push_back(currentIndex[0] + offset);
    }
}

RE::NiPoint3 Geometry::Rotate(const RE::NiPoint3& A, const RE::NiPoint3& angles) {
    RE::NiMatrix3 R;
    R.SetEulerAnglesXYZ(angles);
    return R * A;
}

Geometry::~Geometry() {}

Geometry::Geometry(RE::NiAVObject* obj) {
    EachGeometry(obj, [this](RE::NiAVObject* o3d) -> void {
        RE::BSGeometry* geometry = o3d->AsGeometry();
        if (!geometry) {
            return;
        }

        RE::BSGeometry::GEOMETRY_RUNTIME_DATA& model =
            geometry->GetGeometryRuntimeData();
        const std::size_t initialPositionCount = positions.size();

        if (model.rendererData) {
            FetchVertexes(geometry, model.rendererData);
        }

        if (positions.size() == initialPositionCount &&
            model.skinInstance &&
            model.skinInstance->skinPartition) {
            FetchBoneVertexes(geometry, model.skinInstance->skinPartition);
        }
    });
}

bool Geometry::Empty() const {
    return positions.empty();
}


std::pair<RE::NiPoint3, RE::NiPoint3> Geometry::GetWorldBoundingBox() const {
    if (positions.empty()) {
        return {};
    }

    RE::NiPoint3 min = positions.front();
    RE::NiPoint3 max = positions.front();

    for (const RE::NiPoint3& position : positions) {
        min.x = std::min(min.x, position.x);
        min.y = std::min(min.y, position.y);
        min.z = std::min(min.z, position.z);
        max.x = std::max(max.x, position.x);
        max.y = std::max(max.y, position.y);
        max.z = std::max(max.z, position.z);
    }

    return {min, max};
}
