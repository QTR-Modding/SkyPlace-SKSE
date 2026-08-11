#include "PlacementItems.h"

#include <charconv>
#include <system_error>

#include "Config.h"
#include "Raycast.h"
#include "Transform.h"

namespace {
    constexpr std::string_view configSuffix = "_PlacementItem_SP.json";
    constexpr std::uint32_t maximumLocalFormID = 0x00FFFFFF;

    std::map<RE::FormID, RE::FormID> objectsByItem;
    std::map<RE::FormID, RE::FormID> itemsByObject;

    struct FormReference {
        std::string fileName;
        RE::FormID localFormID = 0;
    };

    std::optional<RE::FormID> ParseLocalFormID(std::string_view text) {
        if (text.starts_with("0x") || text.starts_with("0X")) {
            text.remove_prefix(2);
        }
        if (text.empty()) {
            return {};
        }

        std::uint32_t value = 0;
        const std::from_chars_result result =
            std::from_chars(text.data(), text.data() + text.size(), value, 16);
        if (result.ec != std::errc() ||
            result.ptr != text.data() + text.size() ||
            value == 0 ||
            value > maximumLocalFormID) {
            return {};
        }

        return value;
    }

    std::optional<FormReference> ReadFormReference(
        const json& entry,
        std::string_view fieldName,
        std::string_view configFile,
        std::size_t index)
    {
        const json::const_iterator field = entry.find(std::string(fieldName));
        if (field == entry.end() || !field->is_object()) {
            logger::error(
                "{}[{}] is missing object field '{}'",
                configFile,
                index,
                fieldName);
            return {};
        }

        const json::const_iterator fileName = field->find("FileName");
        const json::const_iterator localFormID = field->find("LocalFormID");
        if (fileName == field->end() || !fileName->is_string() ||
            localFormID == field->end() || !localFormID->is_string()) {
            logger::error(
                "{}[{}].{} requires string fields FileName and LocalFormID",
                configFile,
                index,
                fieldName);
            return {};
        }

        FormReference result;
        result.fileName = fileName->get<std::string>();
        const std::optional<RE::FormID> parsedFormID =
            ParseLocalFormID(localFormID->get<std::string>());
        if (result.fileName.empty() || !parsedFormID) {
            logger::error(
                "{}[{}].{} contains an invalid file name or local form ID",
                configFile,
                index,
                fieldName);
            return {};
        }

        result.localFormID = *parsedFormID;
        return result;
    }

    RE::FormID ResolveFormID(
        const FormReference& reference,
        std::string_view configFile,
        std::size_t index,
        std::string_view fieldName)
    {
        RE::TESDataHandler* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            return 0;
        }

        const RE::FormID formID =
            dataHandler->LookupFormID(reference.localFormID, reference.fileName);
        if (formID == 0) {
            logger::error(
                "{}[{}].{} could not resolve {}:{:06X}",
                configFile,
                index,
                fieldName,
                reference.fileName,
                reference.localFormID);
        }
        return formID;
    }

    RE::NiPoint3 GetPlacementPosition() {
        const RayOutput ray = RayCast::Cast(
            [](RE::NiAVObject* object) {
                if (!object || !object->GetUserData()) {
                    return true;
                }

                const RE::ObjectRefHandle handle = object->GetUserData()->GetHandle();
                const RE::NiPointer<RE::TESObjectREFR> reference = handle.get();
                return !reference || !reference->IsPlayerRef();
            },
            500.0f);

        if (ray.hasHit) {
            return ray.position;
        }

        const std::pair<RE::NiPoint3, RE::NiPoint3> cameraData =
            RayCast::GetCameraData();
        return cameraData.second + RayMath::angles2dir(cameraData.first) * 150.0f;
    }

    void LoadConfig(std::string configFile, json data) {
        logger::info("Loading placement item file: {}", configFile);
        if (!data.is_array()) {
            logger::error("{} must contain a JSON array", configFile);
            return;
        }

        for (std::size_t index = 0; index < data.size(); ++index) {
            const json& entry = data[index];
            if (!entry.is_object()) {
                logger::error("{}[{}] must be an object", configFile, index);
                continue;
            }

            const std::optional<FormReference> itemReference =
                ReadFormReference(entry, "Item", configFile, index);
            const std::optional<FormReference> objectReference =
                ReadFormReference(entry, "Object", configFile, index);
            if (!itemReference || !objectReference) {
                continue;
            }

            const RE::FormID itemFormID =
                ResolveFormID(*itemReference, configFile, index, "Item");
            const RE::FormID objectFormID =
                ResolveFormID(*objectReference, configFile, index, "Object");
            RE::TESObjectMISC* item =
                RE::TESForm::LookupByID<RE::TESObjectMISC>(itemFormID);
            RE::TESBoundObject* object =
                RE::TESForm::LookupByID<RE::TESBoundObject>(objectFormID);
            if (!item || !object) {
                logger::error(
                    "{}[{}] requires Item to be MISC and Object to be a bound object",
                    configFile,
                    index);
                continue;
            }

            if (objectsByItem.contains(itemFormID)) {
                logger::error(
                    "{}[{}] repeats placement item {:08X}",
                    configFile,
                    index,
                    itemFormID);
                continue;
            }
            if (itemsByObject.contains(objectFormID)) {
                logger::error(
                    "{}[{}] repeats placement object {:08X}",
                    configFile,
                    index,
                    objectFormID);
                continue;
            }

            objectsByItem[itemFormID] = objectFormID;
            itemsByObject[objectFormID] = itemFormID;
            logger::info(
                "Registered placement item {:08X} for object {:08X}",
                itemFormID,
                objectFormID);
        }
    }
}

void PlacementItems::Install() {
    objectsByItem.clear();
    itemsByObject.clear();
    Config::Each("Data/", std::string(configSuffix), LoadConfig);
}

bool PlacementItems::IsItem(RE::TESBoundObject* item) {
    return item && objectsByItem.contains(item->GetFormID());
}

bool PlacementItems::HasItemForObject(RE::TESBoundObject* object) {
    return object && itemsByObject.contains(object->GetFormID());
}

RE::TESObjectMISC* PlacementItems::GetItemForObject(RE::TESBoundObject* object) {
    if (!object) {
        return nullptr;
    }

    const std::map<RE::FormID, RE::FormID>::const_iterator item =
        itemsByObject.find(object->GetFormID());
    if (item == itemsByObject.end()) {
        return nullptr;
    }

    return RE::TESForm::LookupByID<RE::TESObjectMISC>(item->second);
}

bool PlacementItems::Materialize(
    RE::TESBoundObject* item,
    std::vector<RE::ObjectRefHandle>& materializedHandles)
{
    materializedHandles.clear();
    if (!item) {
        return false;
    }

    const std::map<RE::FormID, RE::FormID>::const_iterator objectEntry =
        objectsByItem.find(item->GetFormID());
    if (objectEntry == objectsByItem.end()) {
        return false;
    }

    RE::TESBoundObject* object =
        RE::TESForm::LookupByID<RE::TESBoundObject>(objectEntry->second);
    RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
    if (!object || !player) {
        return false;
    }

    RE::NiPointer<RE::TESObjectREFR> placedReference =
        player->PlaceObjectAtMe(object, true);
    if (!placedReference) {
        return false;
    }

    const RE::ObjectRefHandle placedHandle = placedReference->GetHandle();
    Transform::Wrap(
        placedHandle,
        GetPlacementPosition(),
        RE::NiPoint3{});
    materializedHandles.push_back(placedHandle);
    return true;
}
