#include "ObjectGroup.h"

#include <glm/ext.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <random>

#include "DynamicForm.h"
#include "FormManager.h"
#include "Hooks.h"
#include "InventoryChest.h"
#include "PlacementItems.h"
#include "Raycast.h"
#include "Transform.h"
#include "Translations.h"

namespace {
    std::map<RE::FormID, ObjectGroup::Data> groupsByItem;
    std::map<std::string, RE::FormID> itemsByMesh;
    std::vector<RE::FormID> retiredItems;

    void QueueRemoval(
        std::vector<RE::ObjectRefHandle> handles,
        std::function<void()> onRemoved = {})
    {
        const SKSE::TaskInterface* taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            return;
        }

        taskInterface->AddTask([handles = std::move(handles), onRemoved = std::move(onRemoved)]() mutable {
            for (const RE::ObjectRefHandle& handle : handles) {
                const RE::NiPointer<RE::TESObjectREFR> reference = handle.get();
                if (reference && !reference->IsDeleted()) {
                    reference->Disable();
                }
            }

            const SKSE::TaskInterface* deleteTaskInterface = SKSE::GetTaskInterface();
            if (!deleteTaskInterface) {
                return;
            }

            deleteTaskInterface->AddTask(
                [handles = std::move(handles), onRemoved = std::move(onRemoved)]() mutable {
                for (const RE::ObjectRefHandle& handle : handles) {
                    const RE::NiPointer<RE::TESObjectREFR> reference = handle.get();
                    if (reference && !reference->IsDeleted()) {
                        reference->SetDelete(true);
                    }
                }

                if (onRemoved) {
                    const SKSE::TaskInterface* cleanupTaskInterface = SKSE::GetTaskInterface();
                    if (cleanupTaskInterface) {
                        cleanupTaskInterface->AddTask(std::move(onRemoved));
                    }
                }
            });
        });
    }

    std::string NormalizeMeshPath(std::string_view meshPath) {
        std::string result(meshPath);
        std::ranges::transform(result, result.begin(), [](unsigned char character) {
            if (character == '/') {
                return '\\';
            }
            return static_cast<char>(std::tolower(character));
        });
        constexpr std::string_view meshesPrefix = "meshes\\";
        if (result.starts_with(meshesPrefix)) {
            result.erase(0, meshesPrefix.size());
        }
        return result;
    }

    std::string CreateGuid() {
        std::array<std::uint8_t, 16> bytes;
        std::random_device randomDevice;
        std::mt19937_64 generator(randomDevice());
        std::uniform_int_distribution<std::uint32_t> distribution(0, 255);

        for (std::uint8_t& byte : bytes) {
            byte = static_cast<std::uint8_t>(distribution(generator));
        }

        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);

        return fmt::format(
            "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-"
            "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
            bytes[0], bytes[1], bytes[2], bytes[3],
            bytes[4], bytes[5], bytes[6], bytes[7],
            bytes[8], bytes[9], bytes[10], bytes[11],
            bytes[12], bytes[13], bytes[14], bytes[15]);
    }

    RE::TESBoundObject* GetOriginalObject(
        const RE::ObjectRefHandle& handle) {
        const RE::NiPointer<RE::TESObjectREFR> reference = handle.get();
        if (!reference) {
            return nullptr;
        }

        RE::TESBoundObject* baseObject = reference->GetBaseObject();
        if (!baseObject) {
            return nullptr;
        }

        return baseObject;
    }

    std::string GetItemName(
        RE::TESBoundObject* object,
        std::size_t objectCount)
    {
        if (objectCount != 1 || !object) {
            return Translations::Get("ObjectGroup.DefaultName");
        }

        const char* objectName = object->GetName();
        if (objectName && objectName[0] != '\0') {
            return objectName;
        }

        const std::optional<FormManagerData> configuredData =
            FormManager::Get(object->GetFormID());
        if (configuredData && !configuredData->name.empty()) {
            return configuredData->name;
        }

        return Translations::Get("ObjectGroup.DefaultName");
    }

    void AddObjectValue(const RE::TESBoundObject* object, ObjectGroup::Data& data) {
        if (!object) {
            return;
        }

        const std::optional<FormManagerData> configuredData = FormManager::Get(object->GetFormID());
        if (configuredData) {
            data.weight += configuredData->weight;
            data.value += configuredData->value;
            return;
        }

        const RE::TESWeightForm* weightForm = object->As<RE::TESWeightForm>();
        if (weightForm) {
            data.weight += weightForm->weight;
        }

        const RE::TESValueForm* valueForm = object->As<RE::TESValueForm>();
        if (valueForm) {
            data.value += valueForm->value;
        }
    }

    ObjectGroup::Member CreateMember(
        const RE::ObjectRefHandle& handle,
        const RE::NiPoint3& groupPosition,
        const RE::NiTransform& inverseGroupPreviewTransform)
    {
        ObjectGroup::Member member;
        const RE::NiPointer<RE::TESObjectREFR> reference = handle.get();
        if (!reference) {
            return member;
        }
        RE::TESBoundObject* object = GetOriginalObject(handle);
        member.objectFormID = object ? object->GetFormID() : 0;
        member.scale = reference->GetScale();

        const RE::NiPoint3 positionOffset = reference->GetPosition() - groupPosition;
        member.relativePosition = positionOffset;
        member.relativeAngle = reference->GetAngle();

        // The live scene root can contain temporary local transforms applied while
        // placing an object. Build preview data from the reference state instead,
        // so selection/placement rotation cannot leak into the inventory model.
        RE::NiTransform referenceTransform;
        referenceTransform.translate = reference->GetPosition();
        referenceTransform.rotate.SetEulerAnglesXYZ(member.relativeAngle);
        referenceTransform.scale = member.scale;

        const RE::NiTransform previewTransform =
            inverseGroupPreviewTransform * referenceTransform;
        member.previewPosition = previewTransform.translate;
        member.previewRotation = previewTransform.rotate;
        member.previewScale = previewTransform.scale;

        return member;
    }

    void RestoreStoredInventories(
        ObjectGroup::Data& data,
        const std::vector<RE::ObjectRefHandle>& handles)
    {
        const std::size_t count = std::min(data.members.size(), handles.size());
        for (std::size_t index = 0; index < count; ++index) {
            ObjectGroup::Member& member = data.members[index];
            if (member.inventoryChestRefID == 0) {
                continue;
            }

            InventoryChest::Restore(member.inventoryChestRefID, handles[index]);
            member.inventoryChestRefID = 0;
        }
    }

    bool StoreInventories(
        ObjectGroup::Data& data,
        const std::vector<RE::ObjectRefHandle>& handles)
    {
        const std::size_t count = std::min(data.members.size(), handles.size());
        for (std::size_t index = 0; index < count; ++index) {
            const RE::ObjectRefHandle& handle = handles[index];
            if (!InventoryChest::ShouldStore(handle)) {
                continue;
            }

            const RE::FormID chestRefID = InventoryChest::Store(handle);
            if (chestRefID == 0) {
                RestoreStoredInventories(data, handles);
                return false;
            }

            data.members[index].inventoryChestRefID = chestRefID;
        }

        return true;
    }

    void ApplyItemData(RE::TESObjectMISC* item, const ObjectGroup::Data& data) {
        if (!item) {
            return;
        }

        RE::TESModel* model = item->As<RE::TESModel>();
        if (model) {
            model->SetModel(data.meshPath.c_str());
        }

        RE::TESFullName* fullName = item->As<RE::TESFullName>();
        if (fullName) {
            fullName->SetFullName(data.name.c_str());
        }

        RE::TESWeightForm* weightForm = item->As<RE::TESWeightForm>();
        if (weightForm) {
            weightForm->weight = data.weight;
        }

        RE::TESValueForm* valueForm = item->As<RE::TESValueForm>();
        if (valueForm) {
            valueForm->value = data.value;
        }
    }

    void Register(ObjectGroup::Data data) {
        if (data.itemFormID == 0 || data.meshPath.empty()) {
            return;
        }

        if (!Hooks::CacheGroupModel(data)) {
            logger::error("Failed to pre-cache group model {}", data.meshPath);
        }

        itemsByMesh[NormalizeMeshPath(data.meshPath)] = data.itemFormID;
        groupsByItem[data.itemFormID] = std::move(data);
    }

    RE::FormID AcquireItem(RE::FormID templateFormID) {
        while (!retiredItems.empty()) {
            const RE::FormID itemFormID = retiredItems.back();
            retiredItems.pop_back();
            if (RE::TESForm::LookupByID<RE::TESObjectMISC>(itemFormID)) {
                return itemFormID;
            }
        }

        return DynamicForm::Create(templateFormID);
    }

    void Retire(RE::FormID itemFormID, std::string_view expectedMeshPath) {
        const auto group = groupsByItem.find(itemFormID);
        if (group == groupsByItem.end() || group->second.meshPath != expectedMeshPath) {
            return;
        }

        const std::string meshPath = group->second.meshPath;
        itemsByMesh.erase(NormalizeMeshPath(meshPath));
        groupsByItem.erase(group);

        if (RE::TESForm::LookupByID<RE::TESObjectMISC>(itemFormID)) {
            retiredItems.push_back(itemFormID);
        }
        Hooks::RetireGroupModel(meshPath);
    }

    RE::NiPoint3 GetPlacementPosition() {
        const RayOutput ray = RayCast::Cast(
            [](RE::NiAVObject* object) {
                if (object) {
                    if (object->GetUserData()) {
                        const RE::ObjectRefHandle handle = object->GetUserData()->GetHandle();
                        const RE::NiPointer<RE::TESObjectREFR> reference = handle.get();
                        return !reference || !reference->IsPlayerRef();
                    }
                    return true;
                }
                return true;
            },
            500.0f);

        if (ray.hasHit) {
            return ray.position;
        }

        const std::pair<RE::NiPoint3, RE::NiPoint3> cameraData = RayCast::GetCameraData();
        return cameraData.second + RayMath::angles2dir(cameraData.first) * 150.0f;
    }
}

void ObjectGroup::Clear() {
    groupsByItem.clear();
    itemsByMesh.clear();
    retiredItems.clear();
}

bool ObjectGroup::PickUp(const std::vector<RE::ObjectRefHandle>& handles) {
    std::vector<RE::ObjectRefHandle> references;
    references.reserve(handles.size());

    for (const RE::ObjectRefHandle& handle : handles) {
        if (handle.get() && GetOriginalObject(handle)) {
            references.push_back(handle);
        }
    }

    if (references.empty()) {
        return false;
    }

    RE::TESBoundObject* templateObject = GetOriginalObject(references.front());
    if (!templateObject) {
        return false;
    }

    if (references.size() == 1 &&
        !InventoryChest::ShouldStore(references.front())) {
        RE::TESObjectMISC* placementItem =
            PlacementItems::GetItemForObject(templateObject);
        RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
        if (placementItem && player) {
            const std::int32_t previousCount = player->GetItemCount(placementItem);
            player->AddObjectToContainer(placementItem, nullptr, 1, nullptr);
            if (player->GetItemCount(placementItem) <= previousCount) {
                return false;
            }

            QueueRemoval({references.front()});
            return true;
        }
    }

    Data data;
    data.guid = CreateGuid();
    data.meshPath = data.guid + ".nif";
    data.name = GetItemName(templateObject, references.size());
    data.playerFacingYaw = RayCast::GetCameraData().first.z;
    data.preservesPlayerFacing = true;
    data.members.reserve(references.size());

    RE::NiPoint3 groupPosition;
    for (const RE::ObjectRefHandle& handle : references) {
        const RE::NiPointer<RE::TESObjectREFR> reference = handle.get();
        if (reference) {
            groupPosition += reference->GetPosition();
        }
    }
    groupPosition /= static_cast<float>(references.size());

    RE::NiTransform groupPreviewTransform;
    groupPreviewTransform.translate = groupPosition;
    groupPreviewTransform.rotate.SetEulerAnglesXYZ(
        0.0f,
        0.0f,
        data.playerFacingYaw + glm::pi<float>());
    const RE::NiTransform inverseGroupPreviewTransform = groupPreviewTransform.Invert();

    for (const RE::ObjectRefHandle& handle : references) {
        RE::TESBoundObject* memberObject = GetOriginalObject(handle);
        if (!memberObject) {
            continue;
        }

        data.members.push_back(CreateMember(
            handle,
            groupPosition,
            inverseGroupPreviewTransform));
        AddObjectValue(memberObject, data);
    }

    if (data.members.empty()) {
        return false;
    }

    data.itemFormID = AcquireItem(templateObject->GetFormID());
    RE::TESObjectMISC* groupItem = RE::TESForm::LookupByID<RE::TESObjectMISC>(data.itemFormID);
    if (!groupItem) {
        return false;
    }

    if (!StoreInventories(data, references)) {
        if (RE::TESForm::LookupByID<RE::TESObjectMISC>(data.itemFormID)) {
            retiredItems.push_back(data.itemFormID);
        }
        return false;
    }

    ApplyItemData(groupItem, data);
    Register(data);

    RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        RestoreStoredInventories(data, references);
        Retire(data.itemFormID, data.meshPath);
        return false;
    }

    const RE::NiPointer<RE::TESObjectREFR> reference =
        player->PlaceObjectAtMe(groupItem, true);
    if (reference) {
        if (auto cell = reference->GetParentCell()) {
            if (auto owner = cell->GetOwner()) {
                reference->SetOwner(owner);
            }
        }
        player->PickUpObject(reference.get(), 1);
    } else {
        RestoreStoredInventories(data, references);
        Retire(data.itemFormID, data.meshPath);
        return false;
    }


    std::vector<RE::ObjectRefHandle> removalHandles;
    removalHandles.reserve(references.size());
    for (const RE::ObjectRefHandle& handle : references) {
        removalHandles.push_back(handle);
    }
    QueueRemoval(std::move(removalHandles));

    return true;
}

bool ObjectGroup::IsGroupItem(RE::TESBoundObject* item) {
    return item && groupsByItem.contains(item->GetFormID());
}

RE::TESBoundObject* ObjectGroup::CloneEmpty(RE::TESBoundObject* item) {
    if (!item) {
        return nullptr;
    }

    const auto sourceGroup = groupsByItem.find(item->GetFormID());
    if (sourceGroup == groupsByItem.end()) {
        return nullptr;
    }

    Data clonedData = sourceGroup->second;
    if (clonedData.members.empty()) {
        return nullptr;
    }

    clonedData.itemFormID = AcquireItem(clonedData.members.front().objectFormID);
    clonedData.guid = CreateGuid();
    clonedData.meshPath = clonedData.guid + ".nif";
    for (Member& member : clonedData.members) {
        member.inventoryChestRefID = 0;
    }

    RE::TESObjectMISC* clonedItem =
        RE::TESForm::LookupByID<RE::TESObjectMISC>(clonedData.itemFormID);
    if (!clonedItem) {
        return nullptr;
    }

    ApplyItemData(clonedItem, clonedData);
    Register(std::move(clonedData));
    return clonedItem;
}

const ObjectGroup::Data* ObjectGroup::GetByMesh(std::string_view meshPath) {
    const auto item = itemsByMesh.find(NormalizeMeshPath(meshPath));
    if (item == itemsByMesh.end()) {
        return nullptr;
    }

    const auto group = groupsByItem.find(item->second);
    return group != groupsByItem.end() ? &group->second : nullptr;
}

bool ObjectGroup::Materialize(
    RE::TESBoundObject* item,
    std::vector<RE::ObjectRefHandle>& materializedHandles)
{
    materializedHandles.clear();

    if (!item) {
        return false;
    }

    const auto group = groupsByItem.find(item->GetFormID());
    if (group == groupsByItem.end()) {
        return false;
    }

    RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return false;
    }

    const Data& data = group->second;
    const RE::NiPoint3 groupPosition = GetPlacementPosition();
    constexpr float previewScale = 1.0f;
    glm::mat4 groupRotation;
    if (data.preservesPlayerFacing) {
        const float currentPlayerFacingYaw = RayCast::GetCameraData().first.z;
        const float playerFacingYawDelta =
            currentPlayerFacingYaw - data.playerFacingYaw;
        groupRotation = glm::eulerAngleXYZ(
            0.0f,
            0.0f,
            -playerFacingYawDelta);
    } else {
        const RE::NiPoint3 groupAngle{0.0f, 0.0f, 0.0f};
        groupRotation = glm::eulerAngleXYZ(
            -groupAngle.x,
            -groupAngle.y,
            -groupAngle.z);
    }

    for (const Member& member : data.members) {
        RE::TESBoundObject* object = RE::TESForm::LookupByID<RE::TESBoundObject>(member.objectFormID);
        if (!object) {
            continue;
        }

        RE::NiPointer<RE::TESObjectREFR> placedReference = player->PlaceObjectAtMe(object, true);
        if (!placedReference) {
            continue;
        }
        const RE::ObjectRefHandle placedHandle = placedReference->GetHandle();

        const glm::vec4 worldOffset = groupRotation *
            glm::vec4(
                member.relativePosition.x,
                member.relativePosition.y,
                member.relativePosition.z,
                0.0f) * previewScale;
        const RE::NiPoint3 position =
            groupPosition + RE::NiPoint3(worldOffset.x, worldOffset.y, worldOffset.z);

        const glm::mat4 relativeRotation =
            glm::eulerAngleXYZ(
                -member.relativeAngle.x,
                -member.relativeAngle.y,
                -member.relativeAngle.z);
        const glm::mat4 worldRotation = groupRotation * relativeRotation;
        RE::NiPoint3 extractedAngle;
        glm::extractEulerAngleXYZ(
            worldRotation,
            extractedAngle.x,
            extractedAngle.y,
            extractedAngle.z);
        const RE::NiPoint3 placedAngle = -extractedAngle;

        placedReference->SetScale(member.scale * previewScale);
        Transform::Wrap(placedHandle, position, placedAngle);
        InventoryChest::Restore(member.inventoryChestRefID, placedHandle);
        materializedHandles.push_back(placedHandle);
    }

    if (materializedHandles.empty()) {
        return false;
    }

    return true;
}

void ObjectGroup::RetireIfUnused(RE::TESBoundObject* item) {
    if (!item) {
        return;
    }

    const auto group = groupsByItem.find(item->GetFormID());
    if (group == groupsByItem.end()) {
        return;
    }

    RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
    if (player && player->GetItemCount(item) > 0) {
        return;
    }

    const RE::FormID itemFormID = group->second.itemFormID;
    const std::string meshPath = group->second.meshPath;
    Retire(itemFormID, meshPath);
}

void ObjectGroup::ReviveItem(RE::TESObjectMISC* item, RE::FormID savedFormID) {
    if (!item) {
        return;
    }

    auto group = groupsByItem.find(savedFormID);
    if (group == groupsByItem.end()) {
        return;
    }

    Data data = group->second;
    if (item->GetFormID() != savedFormID) {
        groupsByItem.erase(group);
        data.itemFormID = item->GetFormID();
        Register(data);
    }

    ApplyItemData(item, data);
}

const std::map<RE::FormID, ObjectGroup::Data>& ObjectGroup::GetAll() {
    return groupsByItem;
}

const std::vector<RE::FormID>& ObjectGroup::GetRetiredItems() {
    return retiredItems;
}

void ObjectGroup::Restore(ObjectGroup::Data data) {
    Register(std::move(data));
}
