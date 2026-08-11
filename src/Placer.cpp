#include "Placer.h"
#include "Shader.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

#include <glm/ext.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/norm.hpp>
#include <glm/gtx/quaternion.hpp>

#include "DrawDebug.h"
#include "Geometry.h"
#include "HUD.h"
#include "Menu.h"
#include "ObjectGroup.h"
#include "PlacementItems.h"
#include "Picker.h"
#include "Raycast.h"
#include "Transform.h"
#include "Translations.h"
#include "ScreenLog.h"

namespace {
    constexpr float defaultRaycastDistance = 500.0f;
    constexpr float minimumRaycastDistance = 10.0f;
    constexpr float maximumRaycastDistance = 5000.0f;

    bool IsFixableMotionType(RE::hkpMotion::MotionType motionType) {
        return motionType == RE::hkpMotion::MotionType::kDynamic ||
            motionType == RE::hkpMotion::MotionType::kSphereInertia ||
            motionType == RE::hkpMotion::MotionType::kBoxInertia;
    }

    bool HasMovablePhysics(RE::hkpMotion::MotionType motionType) {
        return motionType == RE::hkpMotion::MotionType::kDynamic ||
            motionType == RE::hkpMotion::MotionType::kSphereInertia ||
            motionType == RE::hkpMotion::MotionType::kBoxInertia ||
            motionType == RE::hkpMotion::MotionType::kKeyframed ||
            motionType == RE::hkpMotion::MotionType::kThinBoxInertia;
    }

    void FixDynamicBodiesInPlace(RE::TESObjectREFR* ref) {
        RE::NiAVObject* root = ref ? ref->Get3D() : nullptr;
        if (!root) {
            return;
        }

        std::vector<RE::NiAVObject*> objectsToFix;
        RE::BSVisit::TraverseScenegraphCollision(
            root,
            [&](RE::bhkNiCollisionObject* collisionObject) {
                RE::bhkRigidBody* rigidBody =
                    collisionObject && collisionObject->body ?
                        collisionObject->body->AsBhkRigidBody() : nullptr;
                RE::hkpRigidBody* havokBody =
                    rigidBody ? rigidBody->GetRigidBody() : nullptr;
                if (havokBody &&
                    IsFixableMotionType(havokBody->motion.type.get()) &&
                    collisionObject->sceneObject)
                {
                    objectsToFix.push_back(collisionObject->sceneObject);
                }
                return RE::BSVisit::BSVisitControl::kContinue;
            });

        bool changed = false;
        for (RE::NiAVObject* object : objectsToFix) {
            changed |= object->SetMotionType(
                RE::hkpMotion::MotionType::kKeyframed,
                false,
                false,
                true);
        }
        if (changed) {
            ref->AddChange(RE::TESObjectREFR::ChangeFlags::kHavokMoved);
        }
    }

    void DrawWorldBoundingBox(
        const std::pair<RE::NiPoint3, RE::NiPoint3>& bounds) {
        const RE::NiPoint3& minimum = bounds.first;
        const RE::NiPoint3& maximum = bounds.second;

        const RE::NiPoint3 bottomBackLeft{minimum.x, minimum.y, minimum.z};
        const RE::NiPoint3 bottomBackRight{maximum.x, minimum.y, minimum.z};
        const RE::NiPoint3 bottomFrontRight{maximum.x, maximum.y, minimum.z};
        const RE::NiPoint3 bottomFrontLeft{minimum.x, maximum.y, minimum.z};
        const RE::NiPoint3 topBackLeft{minimum.x, minimum.y, maximum.z};
        const RE::NiPoint3 topBackRight{maximum.x, minimum.y, maximum.z};
        const RE::NiPoint3 topFrontRight{maximum.x, maximum.y, maximum.z};
        const RE::NiPoint3 topFrontLeft{minimum.x, maximum.y, maximum.z};

        DrawDebug::DrawLine(bottomBackLeft, bottomBackRight);
        DrawDebug::DrawLine(bottomBackRight, bottomFrontRight);
        DrawDebug::DrawLine(bottomFrontRight, bottomFrontLeft);
        DrawDebug::DrawLine(bottomFrontLeft, bottomBackLeft);
        DrawDebug::DrawLine(topBackLeft, topBackRight);
        DrawDebug::DrawLine(topBackRight, topFrontRight);
        DrawDebug::DrawLine(topFrontRight, topFrontLeft);
        DrawDebug::DrawLine(topFrontLeft, topBackLeft);
        DrawDebug::DrawLine(bottomBackLeft, topBackLeft);
        DrawDebug::DrawLine(bottomBackRight, topBackRight);
        DrawDebug::DrawLine(bottomFrontRight, topFrontRight);
        DrawDebug::DrawLine(bottomFrontLeft, topFrontLeft);
    }

    void WrapDown(const RE::ObjectRefHandle& handle) {
        const RE::NiPointer<RE::TESObjectREFR> ref = handle.get();
        if (!ref || ref->GetBaseObject()->IsInventoryObject()) {
            return;
        }
        auto pos = ref->GetPosition();
        pos.z -= 200;
        Transform::Wrap(handle, pos, ref->GetAngle());

        const RE::NiPointer<RE::TESObjectREFR> wrappedRef = handle.get();
        if (!wrappedRef) {
            return;
        }

        // MoveTo can replace the 3D after the room change. Refresh now when the
        // replacement is already available, then refresh it again next frame.
        Shader::RefreshReferenceHighlight(handle);
        Shader::QueueReferenceHighlight(handle, wrappedRef->Get3D());
    }

    RE::NiPoint3 MatrixToEulerXYZ(const RE::NiMatrix3& rotation) {
        RE::NiPoint3 result;
        const float sinY = std::clamp(-rotation.entry[0][2], -1.0f, 1.0f);
        result.y = std::asin(sinY);

        if (std::abs(std::cos(result.y)) > 0.00001f) {
            result.x = std::atan2(rotation.entry[1][2], rotation.entry[2][2]);
            result.z = std::atan2(rotation.entry[0][1], rotation.entry[0][0]);
        } else if (sinY > 0.0f) {
            result.x = std::atan2(rotation.entry[1][0], rotation.entry[1][1]);
            result.z = 0.0f;
        } else {
            result.x = std::atan2(-rotation.entry[1][0], rotation.entry[1][1]);
            result.z = 0.0f;
        }

        return result;
    }

    void MoveCollisionBodies(
        RE::NiAVObject* root,
        const RE::NiTransform& oldTransform,
        const RE::NiTransform& newTransform) {
        if (!root) {
            return;
        }

        const RE::NiMatrix3 rotationDelta =
            newTransform.rotate * oldTransform.rotate.Transpose();
        const float scaleDelta = oldTransform.scale != 0.0f ?
            newTransform.scale / oldTransform.scale : 1.0f;
        const float worldScale = RE::bhkWorld::GetWorldScale();
        const float worldScaleInverse = RE::bhkWorld::GetWorldScaleInverse();

        RE::BSVisit::TraverseScenegraphCollision(
            root,
            [&](RE::bhkNiCollisionObject* collisionObject) {
                RE::bhkRigidBody* rigidBody =
                    collisionObject && collisionObject->body ?
                        collisionObject->body->AsBhkRigidBody() : nullptr;
                if (!rigidBody) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }

                RE::hkpRigidBody* havokBody = rigidBody->GetRigidBody();
                const float bodyScaleDelta =
                    havokBody &&
                        HasMovablePhysics(havokBody->motion.type.get()) ?
                    scaleDelta : 1.0f;

                RE::hkVector4 bodyPosition;
                RE::hkQuaternion bodyRotation;
                rigidBody->GetPosition(bodyPosition);
                rigidBody->GetRotation(bodyRotation);

                alignas(16) float position[4];
                alignas(16) float rotation[4];
                _mm_store_ps(position, bodyPosition.quad);
                _mm_store_ps(rotation, bodyRotation.vec.quad);

                const RE::NiPoint3 oldBodyPosition{
                    position[0] * worldScaleInverse,
                    position[1] * worldScaleInverse,
                    position[2] * worldScaleInverse
                };
                const RE::NiPoint3 newBodyPosition =
                    newTransform.translate +
                    rotationDelta *
                        ((oldBodyPosition - oldTransform.translate) *
                         bodyScaleDelta);

                const RE::NiQuaternion oldBodyRotation{
                    rotation[3], rotation[0], rotation[1], rotation[2]};
                const RE::NiMatrix3 newBodyRotation =
                    rotationDelta * oldBodyRotation.ToRotation();
                const RE::NiQuaternion newBodyQuaternion{newBodyRotation};

                RE::hkVector4 havokPosition{
                    newBodyPosition.x * worldScale,
                    newBodyPosition.y * worldScale,
                    newBodyPosition.z * worldScale,
                    0.0f
                };
                RE::hkQuaternion havokRotation;
                havokRotation.vec = RE::hkVector4{
                    newBodyQuaternion.x,
                    newBodyQuaternion.y,
                    newBodyQuaternion.z,
                    newBodyQuaternion.w
                };
                rigidBody->SetPositionAndRotation(havokPosition, havokRotation);

                return RE::BSVisit::BSVisitControl::kContinue;
            });
    }

    bool ScaleHavokShape(
        const RE::hkpShape* shape,
        float scaleRatio,
        std::unordered_set<const RE::hkpShape*>& scaledShapes) {
        if (!shape) {
            return false;
        }
        if (scaledShapes.contains(shape)) {
            return true;
        }

        const RE::hkVector4 scaleVector(scaleRatio);
        switch (shape->type) {
        case RE::hkpShapeType::kBox:
            {
                RE::hkpBoxShape* boxShape =
                    const_cast<RE::hkpBoxShape*>(
                        static_cast<const RE::hkpBoxShape*>(shape));
                boxShape->radius *= scaleRatio;
                boxShape->halfExtents = boxShape->halfExtents * scaleVector;
            }
            break;
        case RE::hkpShapeType::kSphere:
            {
                RE::hkpSphereShape* sphereShape =
                    const_cast<RE::hkpSphereShape*>(
                        static_cast<const RE::hkpSphereShape*>(shape));
                sphereShape->radius *= scaleRatio;
            }
            break;
        case RE::hkpShapeType::kCapsule:
            {
                RE::hkpCapsuleShape* capsuleShape =
                    const_cast<RE::hkpCapsuleShape*>(
                        static_cast<const RE::hkpCapsuleShape*>(shape));
                capsuleShape->radius *= scaleRatio;
                capsuleShape->vertexA = capsuleShape->vertexA * scaleVector;
                capsuleShape->vertexB = capsuleShape->vertexB * scaleVector;
            }
            break;
        case RE::hkpShapeType::kConvexVertices:
            {
                RE::hkpConvexVerticesShape* verticesShape =
                    const_cast<RE::hkpConvexVerticesShape*>(
                        static_cast<const RE::hkpConvexVerticesShape*>(shape));
                verticesShape->radius *= scaleRatio;
                verticesShape->aabbHalfExtents =
                    verticesShape->aabbHalfExtents * scaleVector;
                verticesShape->aabbCenter =
                    verticesShape->aabbCenter * scaleVector;

                for (RE::hkFourTransposedPoints& vertices :
                     verticesShape->rotatedVertices) {
                    vertices.x = vertices.x * scaleVector;
                    vertices.y = vertices.y * scaleVector;
                    vertices.z = vertices.z * scaleVector;
                }

                const __m128 planeScale =
                    _mm_setr_ps(1.0f, 1.0f, 1.0f, scaleRatio);
                for (RE::hkVector4& plane : verticesShape->planeEquations) {
                    plane.quad = _mm_mul_ps(plane.quad, planeScale);
                }
            }
            break;
        default:
            {
                const RE::hkpShapeContainer* container = shape->GetContainer();
                if (!container) {
                    return false;
                }

                bool scaledChild = false;
                for (RE::hkpShapeKey key = container->GetFirstKey();
                     key != RE::HK_INVALID_SHAPE_KEY;
                     key = container->GetNextKey(key)) {
                    RE::hkpShapeBuffer shapeBuffer;
                    const RE::hkpShape* child =
                        container->GetChildShape(key, shapeBuffer);
                    scaledChild |=
                        ScaleHavokShape(child, scaleRatio, scaledShapes);
                }
                if (!scaledChild) {
                    return false;
                }
            }
            break;
        }

        scaledShapes.insert(shape);
        return true;
    }

    bool ScaleCollisionShapes(
        RE::TESObjectREFR* ref,
        RE::NiAVObject* root,
        float scaleRatio,
        std::unordered_set<const RE::hkpShape*>& scaledShapes) {
        if (!ref || !root || scaleRatio == 1.0f) {
            return scaleRatio == 1.0f;
        }

        RE::TESObjectCELL* cell = ref->GetParentCell();
        RE::bhkWorld* world = cell ? cell->GetbhkWorld() : nullptr;
        if (!world) {
            return false;
        }

        bool foundMovablePhysics = false;
        bool scaledCollision = false;
        RE::BSWriteLockGuard locker(world->worldLock);
        RE::BSVisit::TraverseScenegraphCollision(
            root,
            [&](RE::bhkNiCollisionObject* collisionObject) {
                RE::bhkWorldObject* body = collisionObject ?
                    collisionObject->body.get() : nullptr;
                RE::bhkRigidBody* rigidBody =
                    body ? body->AsBhkRigidBody() : nullptr;
                RE::hkpRigidBody* havokBody =
                    rigidBody ? rigidBody->GetRigidBody() : nullptr;
                if (!havokBody ||
                    !HasMovablePhysics(havokBody->motion.type.get())) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }

                foundMovablePhysics = true;
                const RE::hkpShape* shape =
                    havokBody->GetShape();
                if (ScaleHavokShape(shape, scaleRatio, scaledShapes)) {
                    havokBody->UpdateShape(nullptr);
                    scaledCollision = true;
                }
                return RE::BSVisit::BSVisitControl::kContinue;
            });

        // Fixed collision deliberately keeps the original transform behavior.
        return !foundMovablePhysics || scaledCollision;
    }

    void ApplyCollisionScale(
        RE::TESObjectREFR* ref,
        RE::NiAVObject* root,
        float targetScale,
        float& appliedScale,
        RE::NiAVObject*& trackedRoot,
        std::unordered_set<const RE::hkpShape*>& scaledShapes) {
        if (!root) {
            return;
        }
        if (trackedRoot != root) {
            // A replacement 3D has freshly initialized Havok shapes for the
            // reference's current scale.
            trackedRoot = root;
            appliedScale = targetScale;
            return;
        }
        if (targetScale == appliedScale ||
            targetScale <= 0.0f ||
            appliedScale <= 0.0f ||
            !std::isfinite(targetScale) ||
            !std::isfinite(appliedScale)) {
            return;
        }

        const float scaleRatio = targetScale / appliedScale;
        if (ScaleCollisionShapes(ref, root, scaleRatio, scaledShapes)) {
            appliedScale = targetScale;
        }
    }

    void ApplyLoadedReferenceScale(RE::NiAVObject* root, float scale) {
        if (!root) {
            return;
        }

        root->local.scale = scale;
    }
}

RE::ObjectRefHandle Placer::GetMoveHandle() {
    if (!moveHandle.get()) {
        moveHandle.reset();
    }
    return moveHandle;
}

RE::NiPoint3 Placer::Cast() {
    const RE::ObjectRefHandle movingHandle = GetMoveHandle();
    const RayOutput ray = RayCast::Cast(
        [movingHandle](RE::NiAVObject* obj) {
            if (obj) {
                if (obj->GetUserData()) {
                    const RE::ObjectRefHandle handle = obj->GetUserData()->GetHandle();
                    const RE::NiPointer<RE::TESObjectREFR> data = handle.get();
                    return data && !data->IsPlayerRef() && handle != movingHandle && !IsGroupMember(handle);
                }
            }
            return true;
        },
        raycastDistance);
    return ray.position;
}
void Placer::Rotate() {
    const std::pair<RE::NiPoint3, RE::NiPoint3> cameraData = RayCast::GetCameraData();

    const float horizontalAngle =
        cameraData.first.z - initialCameraYaw;

    RE::NiMatrix3 horizontalRotationDelta;
    horizontalRotationDelta.SetEulerAnglesXYZ(
        0.0f,
        0.0f,
        horizontalAngle - appliedHorizontalAngle);

    currentOrientation = horizontalRotationDelta * currentOrientation;
    appliedHorizontalAngle = horizontalAngle;
    currentAngle = MatrixToEulerXYZ(currentOrientation);

    const float translationRotation = -cameraData.first.z;
    const float translatedX =
        translation.x * std::cos(translationRotation) -
        translation.y * std::sin(translationRotation);
    const float translatedY =
        translation.x * std::sin(translationRotation) +
        translation.y * std::cos(translationRotation);
    currentPosition += {translatedX, translatedY, translation.z};
}

void Placer::ClearHints() {
    // controls->Hide();
    HUD::HidePlace();
}

void Placer::PlaceEvent() {
    std::unique_lock lock(mtx);
    FinishGroupMove(false);
    Picker::ShowSelectionHighlights();
    ClearHints();
    moveHandle.reset();
    inventorySource = false;
}

void Placer::FixInPlaceEvent() {
    std::unique_lock lock(mtx);

    std::vector<RE::ObjectRefHandle> handles;
    handles.reserve(groupMembers.size());
    for (const GroupMember& member : groupMembers) {
        handles.push_back(member.handle);
    }

    FinishGroupMove(false);
    for (const RE::ObjectRefHandle& handle : handles) {
        const RE::NiPointer<RE::TESObjectREFR> ref = handle.get();
        FixDynamicBodiesInPlace(ref.get());
    }

    Picker::ShowSelectionHighlights();
    ClearHints();
    moveHandle.reset();
    inventorySource = false;
}

void Placer::TranslateUpDownEvent(RE::NiPoint2 delta) {
    Translate({0.0f, 0.0f, -delta.y * 0.1f});
}

void Placer::TranslateLeftRightEvent(RE::NiPoint2 delta) {
    Translate({delta.x * 0.1f, 0.0f, 0.0f});
}

void Placer::TranslateForwardBackwardEvent(RE::NiPoint2 delta) {
    Translate({0.0f, -delta.y * 0.1f, 0.0f});
}

void Placer::TranslateDepthEvent(RE::NiPoint2 delta) {
    raycastDistance = std::clamp(
        raycastDistance - delta.y * 0.5f,
        minimumRaycastDistance,
        maximumRaycastDistance);
}

void Placer::SetRaycastDistanceFromCurrentPosition() {
    if (!GetMoveHandle()) {
        return;
    }

    const std::pair<RE::NiPoint3, RE::NiPoint3> cameraData = RayCast::GetCameraData();
    raycastDistance = std::clamp(
        RayMath::pointDistance(cameraData.second, currentRaycastPosition),
        minimumRaycastDistance,
        maximumRaycastDistance);
}

void Placer::OrbitRotateEvent(RE::NiPoint2 delta) {
    if (!GetMoveHandle() || (delta.x == 0.0f && delta.y == 0.0f)) {
        return;
    }

    RE::PlayerCamera* camera = RE::PlayerCamera::GetSingleton();
    RE::NiNode* cameraRoot = camera ? camera->cameraRoot.get() : nullptr;
    if (!cameraRoot) {
        return;
    }

    // Skyrim's player camera faces local +Y, so its screen-right and screen-up
    // axes are local X and Z. The UI3D preview camera uses a different basis.
    RE::NiMatrix3 horizontalDrag;
    horizontalDrag.MakeZRotation(-delta.x * 0.01f);

    RE::NiMatrix3 verticalDrag;
    verticalDrag.MakeXRotation(-delta.y * 0.01f);

    const RE::NiMatrix3 cameraRotation = cameraRoot->world.rotate;
    const RE::NiMatrix3 cameraVerticalDelta =
        cameraRotation * verticalDrag * cameraRotation.Transpose();

    // Keep horizontal rotation around Skyrim's world-up axis. Conjugating it
    // through the full camera rotation tilts this axis with the camera pitch.
    // The final multiplication order remains incremental against the object's
    // accumulated orientation.
    const RE::NiMatrix3 cameraDelta =
        cameraVerticalDelta * horizontalDrag;

    currentOrientation = cameraDelta * currentOrientation;
    currentAngle = MatrixToEulerXYZ(currentOrientation);
}

void Placer::ScaleEvent(RE::NiPoint2 delta) {
    if (!GetMoveHandle() || delta.y == 0.0f) {
        return;
    }

    groupScale *= std::exp(-delta.y * 0.005f);
}

void Placer::ResetTransformEvent() {
    std::unique_lock lock(mtx);
    if (!GetMoveHandle()) {
        return;
    }

    const std::pair<RE::NiPoint3, RE::NiPoint3> cameraData =
        RayCast::GetCameraData();
    const float cameraYaw = cameraData.first.z;
    const float cameraRotationAngle = cameraYaw - initialCameraYaw;
    RE::NiMatrix3 cameraRotation;
    cameraRotation.SetEulerAnglesXYZ(
        0.0f,
        0.0f,
        cameraRotationAngle);

    translation = {};
    raycastDistance = defaultRaycastDistance;
    currentRaycastPosition = Cast();
    currentPosition = currentRaycastPosition;
    currentOrientation = cameraRotation * initialOrientation;
    currentAngle = MatrixToEulerXYZ(currentOrientation);
    groupScale = 1.0f;
    appliedHorizontalAngle = cameraRotationAngle;

    ApplyGroupTransform();
}

void Placer::PickEvent() {
    if (GetMoveHandle()) {
        std::vector<RE::ObjectRefHandle> pickHandles;
        pickHandles.reserve(groupMembers.size());
        for (const GroupMember& member : groupMembers) {
            pickHandles.push_back(member.handle);
        }

        FinishGroupMove(false);
        Picker::DeselectAllEvent();
        Picker::PickObjects(pickHandles);

        ClearHints();
        moveHandle.reset();
        inventorySource = false;
    }
}
void Placer::CancelPlaceEvent() {
    if (GetMoveHandle()) {
        if (inventorySource) {
            std::vector<RE::ObjectRefHandle> unpackedHandles;
            unpackedHandles.reserve(groupMembers.size());
            for (const GroupMember& member : groupMembers) {
                unpackedHandles.push_back(member.handle);
            }

            FinishGroupMove(true);
            Picker::DeselectAllEvent();
            Picker::PickObjects(unpackedHandles);
            ClearHints();
            moveHandle.reset();
            inventorySource = false;
        } else {
            ClearHints();
            FinishGroupMove(true);
            Picker::ShowSelectionHighlights();
            moveHandle.reset();
        }
    }
}


void Placer::SaveChangeEvent() {
    FinishGroupMove(false);
    moveHandle.reset();
    inventorySource = false;
    HUD::HidePlace();
}
// https://github.com/ianpatt/skse64/blob/9222d48195f6d1896701d56095ff56b5890514c3/skse64/GameObjects.h#L1995C1-L2011C3
class BGSPrimitive {
public:
    BGSPrimitive();
    virtual ~BGSPrimitive();
    enum { kNone = 0, kBox = 1, kSphere = 2 };
    uint32_t type;
    RE::NiPoint3 bound;
};




void Placer::Tick() {
    ProcessPendingDrop();
    ProcessPendingMaterializedMove();

    std::unique_lock lock(mtx);
    if (GetMoveHandle() &&
        !Menu::IsOpen() &&
        RayCast::IsGameplayCameraActive()) {
        currentRaycastPosition = Cast();
        currentPosition = currentRaycastPosition;
        Rotate();
        ApplyGroupTransform();
        PreventFloorClipping();
        ApplyGroupTransform();
        ShowGroupPlacementHighlights();
    }
}
float ComputeScaleForTargetVolume(RE::NiPoint3 min, RE::NiPoint3 max, float scale, float targetVolume) {
    float width = max.x - min.x;
    float height = max.y - min.y;
    float depth = max.z - min.z;

    float currentVolume = width * height * depth;
    if (currentVolume <= 0.0f) return 0.0f;

    return std::cbrt((targetVolume * targetVolume * targetVolume) / currentVolume) * scale;
}
void Placer::PreventFloorClipping() {
    const float floorZ = currentPosition.z;

    bool foundGeometry = false;
    float lowestPoint = 0.0f;

    for (const GroupMember& member : groupMembers) {
        const RE::NiPointer<RE::TESObjectREFR> memberRef = member.handle.get();
        RE::NiAVObject* member3D = memberRef ? memberRef->Get3D() : nullptr;
        if (!member3D) {
            continue;
        }

        RE::NiUpdateData updateData;
        updateData.time = 0.0f;
        updateData.flags.set(RE::NiUpdateData::Flag::kDirty);
        member3D->UpdateTransformAndBounds(updateData);

        const Geometry geometry(member3D);
        if (geometry.Empty()) {
            continue;
        }

        const std::pair<RE::NiPoint3, RE::NiPoint3> bounds =
            geometry.GetWorldBoundingBox();

        #ifndef NDEBUG
        DrawWorldBoundingBox(bounds);
        #endif  // NDEBUG

        const float memberLowestPoint = bounds.first.z;

        if (!foundGeometry || memberLowestPoint < lowestPoint) {
            lowestPoint = memberLowestPoint;
            foundGeometry = true;
        }
    }

    if (foundGeometry) {
        currentPosition.z += floorZ - lowestPoint;
    }
}

RE::ObjectRefHandle GetRoomAtPosition(const RE::ObjectRefHandle& handle) {
    const RE::NiPointer<RE::TESObjectREFR> ref = handle.get();
    if (!ref) {
        return {};
    }

    auto cell = ref->GetParentCell();
    if (!cell) {
        return {};
    }

    auto loadedData = cell->GetRuntimeData().loadedData;
    if (!loadedData) {
        return {};
    }

    auto position = ref->GetPosition();

    for (auto it = loadedData->multiboundRefMap.begin(); it != loadedData->multiboundRefMap.end(); ++it) {
        auto& item = *it;

        auto node = item.second.get();

        if (!node) {
            continue;
        }

        if (!node->QPointWithin(position)) {
            continue;
        }

        return item.first;
    }

    return {};
}

void Placer::UpdateObjectRoom(const RE::ObjectRefHandle& handle) {
    const RE::NiPointer<RE::TESObjectREFR> ref = handle.get();
    if (!ref ||
        !ref->Is3DLoaded()) {
        return;
    }
    if (const RE::ObjectRefHandle roomHandle = GetRoomAtPosition(handle)) {
        const RE::NiPointer<RE::TESObjectREFR> room = roomHandle.get();
        if (!room) {
            return;
        }
        if ( ref->extraList.HasType(RE::ExtraMultiBoundRef::EXTRADATATYPE)) {
            if (auto moveRefMultiBoundRef = ref->extraList.GetByType<RE::ExtraMultiBoundRef>()) {
                if (moveRefMultiBoundRef->boundRef != room.get()) {
                    ref->extraList.RemoveByType(RE::ExtraMultiBoundRef::EXTRADATATYPE);
                    WrapDown(handle);
                }
            }
        }
    } else {
        auto cell = RE::TES::GetSingleton()->GetCell(currentPosition);
        if (cell != ref->GetParentCell()) {
            WrapDown(handle);
        }
    }
}

void Placer::Move(const RE::ObjectRefHandle& handle) {
    const RE::NiPointer<RE::TESObjectREFR> ref = handle.get();
    if (!ref) {
        return;
    }

    moveHandle = handle;
    initialAngle = ref->GetAngle();
    initialPosition = ref->GetPosition();
    currentAngle = initialAngle;
    currentPosition = initialPosition;
    currentRaycastPosition = initialPosition;
    initialOrientation.SetEulerAnglesXYZ(initialAngle);
    currentOrientation = initialOrientation;

    const std::pair<RE::NiPoint3, RE::NiPoint3> cameraData = RayCast::GetCameraData();
    initialCameraYaw = cameraData.first.z;
    appliedHorizontalAngle = 0.0f;

    BeginGroupMove();

    roomFormId = 0;
    numNormals = 0;
    translation = {};
    raycastDistance = defaultRaycastDistance;
    groupScale = 1.0f;

    Picker::ShowPlacementHighlights(handle);
    ShowGroupPlacementHighlights();

    // controls->Show();
    HUD::ShowPlace();
    HUD::SetIsEnabled(true);
    inventorySource = false;
}

bool Placer::RequestDrop(RE::TESBoundObject* obj, bool itemRemoved) {
    if ((!ObjectGroup::IsGroupItem(obj) && !PlacementItems::IsItem(obj)) ||
        pendingDropFormID != 0) {
        return false;
    }

    pendingDropFormID = obj->GetFormID();
    pendingDropItemRemoved = itemRemoved;
    pendingDropSafeTicks = 0;

    return true;
}

void Placer::ProcessPendingDrop() {
    if (pendingDropFormID == 0) {
        return;
    }

    RE::UI* ui = RE::UI::GetSingleton();
    RE::Inventory3DManager* inventory3D = RE::Inventory3DManager::GetSingleton();
    if ((ui && ui->IsItemMenuOpen()) || (inventory3D && inventory3D->tempRef)) {
        pendingDropSafeTicks = 0;
        Menu::Close();
        return;
    }

    RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
    RE::TESObjectCELL* playerCell = player ? player->GetParentCell() : nullptr;
    RE::LOADED_CELL_DATA* loadedCellData =
        playerCell ? playerCell->GetRuntimeData().loadedData : nullptr;
    if (!playerCell ||
        !playerCell->IsAttached() ||
        !loadedCellData ||
        !loadedCellData->refsFullyLoaded ||
        loadedCellData->criticalQueuedRefCount > 0 ||
        loadedCellData->queuedRefCount > 0 ||
        loadedCellData->queuedDistantRefCount > 0) {
        pendingDropSafeTicks = 0;
        return;
    }

    // Allow queued render work that observed the inventory preview to finish.
    constexpr std::uint8_t requiredSafeTicks = 3;
    if (pendingDropSafeTicks < requiredSafeTicks) {
        ++pendingDropSafeTicks;
        return;
    }

    const RE::FormID formID = pendingDropFormID;
    const bool itemRemoved = pendingDropItemRemoved;
    pendingDropFormID = 0;
    pendingDropItemRemoved = false;
    pendingDropSafeTicks = 0;

    RE::TESBoundObject* object = RE::TESForm::LookupByID<RE::TESBoundObject>(formID);
    if (!object || !Drop(object, itemRemoved)) {
        logger::error("Failed to place deferred placement item {:08X}", formID);
    }
}

void Placer::ProcessPendingMaterializedMove() {
    if (pendingMaterializedHandles.empty()) {
        return;
    }

    std::vector<RE::NiPointer<RE::NiAVObject>> loadedScenegraphs;
    loadedScenegraphs.reserve(pendingMaterializedHandles.size());
    for (const RE::ObjectRefHandle& handle : pendingMaterializedHandles) {
        const RE::NiPointer<RE::TESObjectREFR> reference = handle.get();
        if (!reference) {
            logger::error("A materialized object group member was released before its 3D loaded");
            pendingMaterializedHandles.clear();
            pendingMaterializedSafeTicks = 0;
            return;
        }

        RE::NiPointer<RE::NiAVObject> scenegraph{reference->Get3D()};
        if (!scenegraph) {
            pendingMaterializedSafeTicks = 0;
            return;
        }
        loadedScenegraphs.push_back(std::move(scenegraph));
    }

    constexpr std::uint8_t requiredSafeTicks = 2;
    if (pendingMaterializedSafeTicks < requiredSafeTicks) {
        ++pendingMaterializedSafeTicks;
        return;
    }

    std::vector<RE::ObjectRefHandle> materializedHandles =
        std::move(pendingMaterializedHandles);
    pendingMaterializedHandles.clear();
    pendingMaterializedSafeTicks = 0;

    if (materializedHandles.size() > 1) {
        Picker::SetSelection(materializedHandles);
    } else {
        Picker::SetSelection({});
    }

    Move(materializedHandles.front());
    inventorySource = true;
}

bool Placer::Drop(RE::TESBoundObject* obj, bool itemRemoved) {
    const bool isGroupItem = ObjectGroup::IsGroupItem(obj);
    const bool isConfiguredItem = PlacementItems::IsItem(obj);
    if (!isGroupItem && !isConfiguredItem) {
        return false;
    }

    if (IsPlacing()) {
        if (itemRemoved) {
            RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                player->AddObjectToContainer(obj, nullptr, 1, nullptr);
            }
        }
        RE::SendHUDMessage::ShowHUDMessage(Translations::Get("Placer.AlreadyPlacing"), 0, false);
        return true;
    }

    RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return false;
    }

    if (!itemRemoved && player->GetItemCount(obj) <= 0) {
        return false;
    }

    std::vector<RE::ObjectRefHandle> materializedHandles;
    const bool materialized = isGroupItem ?
        ObjectGroup::Materialize(obj, materializedHandles) :
        PlacementItems::Materialize(obj, materializedHandles);
    if (!materialized || materializedHandles.empty()) {
        if (itemRemoved) {
            player->AddObjectToContainer(obj, nullptr, 1, nullptr);
        }
        return false;
    }

    if (!itemRemoved) {
        player->RemoveItem(obj, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
    }
    if (isGroupItem) {
        ObjectGroup::RetireIfUnused(obj);
    }

    if (!materializedHandles.front().get()) {
        return false;
    }

    pendingMaterializedHandles = std::move(materializedHandles);
    pendingMaterializedSafeTicks = 0;
    Menu::Close();
    return true;
}

bool Placer::IsPlacing() {
    return static_cast<bool>(GetMoveHandle()) ||
        !pendingMaterializedHandles.empty();
}

void Placer::Translate(RE::NiPoint3 offset) { translation += offset; }

bool Placer::IsGroupMember(const RE::ObjectRefHandle& handle) {
    if (!handle) {
        return false;
    }

    for (const GroupMember& member : groupMembers) {
        if (member.handle == handle) {
            return true;
        }
    }

    return false;
}

void Placer::BeginGroupMove() {
    const RE::ObjectRefHandle movingHandle = GetMoveHandle();
    const RE::NiPointer<RE::TESObjectREFR> moveRef = movingHandle.get();
    if (!moveRef) {
        return;
    }

    groupMembers.clear();
    Picker::HideSelectionHighlights();

    const std::vector<RE::ObjectRefHandle> selectedHandles = Picker::GetSelectedHandles();
    std::vector<RE::ObjectRefHandle> handles;
    handles.reserve(selectedHandles.size() + 1);
    handles.push_back(movingHandle);

    const RE::FormID moveFormID = moveRef->GetFormID();
    for (const RE::ObjectRefHandle& handle : selectedHandles) {
        const RE::NiPointer<RE::TESObjectREFR> selectedRef = handle.get();
        if (selectedRef && selectedRef->GetFormID() != moveFormID) {
            handles.push_back(handle);
        }
    }

    groupMembers.reserve(handles.size());
    RE::NiPoint3 groupPosition;
    bool foundHorizontalBounds = false;
    float leftPoint = 0.0f;
    float rightPoint = 0.0f;
    float backPoint = 0.0f;
    float frontPoint = 0.0f;

    for (const RE::ObjectRefHandle& handle : handles) {
        const RE::NiPointer<RE::TESObjectREFR> selectedRef = handle.get();
        if (!selectedRef) {
            continue;
        }
        RE::NiAVObject* selected3D = selectedRef->Get3D();
        GroupMember member{
            handle,
            selectedRef->GetPosition(),
            selectedRef->GetAngle(),
            selectedRef->GetPosition(),
            selectedRef->GetAngle(),
            selectedRef->GetScale(),
            selectedRef->GetScale(),
            1.0f,
            selected3D,
            false
        };

        if (selected3D) {
            const Geometry geometry(selected3D);
            if (!geometry.Empty()) {
                const std::pair<RE::NiPoint3, RE::NiPoint3> bounds =
                    geometry.GetWorldBoundingBox();
                if (foundHorizontalBounds) {
                    leftPoint = std::min(leftPoint, bounds.first.x);
                    rightPoint = std::max(rightPoint, bounds.second.x);
                    backPoint = std::min(backPoint, bounds.first.y);
                    frontPoint = std::max(frontPoint, bounds.second.y);
                } else {
                    leftPoint = bounds.first.x;
                    rightPoint = bounds.second.x;
                    backPoint = bounds.first.y;
                    frontPoint = bounds.second.y;
                    foundHorizontalBounds = true;
                }
            }
        }

        groupMembers.push_back(member);
        groupPosition += member.initialPosition;
    }

    if (groupMembers.empty()) {
        return;
    }

    groupPosition /= static_cast<float>(groupMembers.size());
    if (foundHorizontalBounds) {
        // Keep the placement pivot fixed in group-local space. Re-centering from
        // the world AABB every frame makes asymmetric objects sway as they rotate.
        groupPosition.x = (leftPoint + rightPoint) * 0.5f;
        groupPosition.y = (backPoint + frontPoint) * 0.5f;
    }
    initialPosition = groupPosition;
    currentPosition = groupPosition;
    initialAngle = {};
    currentAngle = {};
    initialOrientation = RE::NiMatrix3();
    currentOrientation = initialOrientation;
}

void Placer::ApplyGroupTransform() {
    const glm::mat4 initialPivotRotation = glm::eulerAngleXYZ(-initialAngle.x, -initialAngle.y, -initialAngle.z);
    const glm::mat4 currentPivotRotation = glm::eulerAngleXYZ(-currentAngle.x, -currentAngle.y, -currentAngle.z);
    const glm::mat4 rotationDelta = currentPivotRotation * glm::inverse(initialPivotRotation);

    std::unordered_set<const RE::hkpShape*> scaledShapes;
    for (GroupMember& member : groupMembers) {
        const RE::NiPointer<RE::TESObjectREFR> memberRef = member.handle.get();
        if (!memberRef) {
            continue;
        }

        const RE::NiPoint3 initialOffset =
            (member.initialPosition - initialPosition) * groupScale;
        const glm::vec4 rotatedOffset = rotationDelta * glm::vec4(initialOffset.x, initialOffset.y, initialOffset.z, 0.0f);
        member.currentPosition = currentPosition + RE::NiPoint3(rotatedOffset.x, rotatedOffset.y, rotatedOffset.z);
        member.currentScale = member.initialScale * groupScale;

        const glm::mat4 initialMemberRotation = glm::eulerAngleXYZ(-member.initialAngle.x, -member.initialAngle.y, -member.initialAngle.z);
        const glm::mat4 currentMemberRotation = rotationDelta * initialMemberRotation;
        RE::NiPoint3 extractedAngle;
        glm::extractEulerAngleXYZ(
            currentMemberRotation,
            extractedAngle.x,
            extractedAngle.y,
            extractedAngle.z);
        member.currentAngle = -extractedAngle;

        RE::NiAVObject* member3D = memberRef->Get3D();
        const RE::NiTransform oldTransform =
            member3D ? member3D->world : RE::NiTransform();
        RE::NiTransform newTransform = oldTransform;
        newTransform.translate = member.currentPosition;
        newTransform.rotate.SetEulerAnglesXYZ(member.currentAngle);
        newTransform.scale = member.currentScale;

        Transform::SetPosition(member.handle, member.currentPosition);
        Transform::SetAngle(member.handle, member.currentAngle);
        if (member3D) {
            MoveCollisionBodies(member3D, oldTransform, newTransform);
            memberRef->SetScale(member.currentScale);
            ApplyLoadedReferenceScale(member3D, member.currentScale);
            memberRef->Update3DPosition(true);
            ApplyCollisionScale(
                memberRef.get(),
                member3D,
                groupScale,
                member.appliedCollisionScale,
                member.collisionRoot,
                scaledShapes);
        } else {
            memberRef->SetScale(member.currentScale);
        }
        UpdateObjectRoom(member.handle);
    }
}

void Placer::ShowGroupPlacementHighlights() {
    if (groupMembers.size() < 2) {
        return;
    }

    for (GroupMember& member : groupMembers) {
        if (member.hasPlacementHighlight) {
            continue;
        }

        const RE::NiPointer<RE::TESObjectREFR> memberRef = member.handle.get();
        if (!memberRef || !memberRef->Get3D()) {
            continue;
        }

        Shader::ApplySelectedHighlight(member.handle);
        member.hasPlacementHighlight = true;
    }
}

void Placer::FinishGroupMove(bool restoreOriginalTransform) {
    const RE::ObjectRefHandle movingHandle = GetMoveHandle();
    const float collisionScale = restoreOriginalTransform ? 1.0f : groupScale;
    std::unordered_set<const RE::hkpShape*> scaledShapes;
    for (GroupMember& member : groupMembers) {
        const RE::NiPointer<RE::TESObjectREFR> memberRef = member.handle.get();
        if (!memberRef) {
            continue;
        }

        const RE::NiPoint3 position = restoreOriginalTransform ? member.initialPosition : member.currentPosition;
        const RE::NiPoint3 memberAngle = restoreOriginalTransform ? member.initialAngle : member.currentAngle;
        const float memberScale = restoreOriginalTransform ? member.initialScale : member.currentScale;

        Transform::Wrap(member.handle, position, memberAngle);
        memberRef->SetScale(memberScale);
        RE::NiAVObject* member3D = memberRef->Get3D();
        if (member3D) {
            ApplyLoadedReferenceScale(member3D, memberScale);
            memberRef->Update3DPosition(true);
            ApplyCollisionScale(
                memberRef.get(),
                member3D,
                collisionScale,
                member.appliedCollisionScale,
                member.collisionRoot,
                scaledShapes);
        }
        UpdateObjectRoom(member.handle);
        if (member.hasPlacementHighlight) {
            Shader::RefreshReferenceHighlight(member.handle);
        }
    }

    groupMembers.clear();
    Picker::HidePlacementHighlight(movingHandle);
}
