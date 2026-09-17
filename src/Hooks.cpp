#include "Shader.h"
#include "Detour.h"
#include "Hooks.h"
#include "DrawDebug.h"
#include "HUD.h"
#include "InputEventHandler.h"
#include "Menu.h"
#include "ObjectGroup.h"
#include "PlacementItems.h"
#include "Persistence.h"
#include "Picker.h"
#include "Placer.h"
#include "ScreenLog.h"
#include "Transform.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <thread>

namespace {
    std::string NormalizeModelPath(std::string_view path) {
        std::string result(path);
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

    class GroupModelData {
    public:
        std::uint64_t unk00 = 0;
        std::uint64_t unk08 = 0;
        std::uint32_t unk10 = 2;
        std::uint32_t pad14 = 0;
        std::uint64_t unk18 = 0;
        void* ptr20 = nullptr;
        RE::NiPointer<RE::NiNode> modelRoot;
        std::uint64_t unk30 = 0;
        std::uint64_t unk38 = 0;
    };
    static_assert(sizeof(GroupModelData) == 0x40);

    struct GroupModelHook {
        static RE::NiPointer<RE::NiNode> LoadModel(const char* path) {
            RE::NiPointer<RE::NiNode> loadedModel;

            RE::BSModelDB::DBTraits::ArgsType args;
            const RE::BSResource::ErrorCode error = RE::BSModelDB::Demand(path, loadedModel, args);
            if (error != RE::BSResource::ErrorCode::kNone || !loadedModel) {
                logger::error(
                    "Failed to load grouped object model {} ({})",
                    path,
                    static_cast<std::uint32_t>(error));
                return nullptr;
            }

            RE::NiPointer<RE::NiNode> result;
            result.reset(loadedModel->Clone()->AsNode());
            result->local = RE::NiTransform();
            result->SetMotionType(RE::hkpMotion::MotionType::kKeyframed, true, false, true);
            result->SetCollisionLayer(RE::COL_LAYER::kNonCollidable);
            return result;
        }

        static RE::NiPointer<RE::NiNode> BuildGroupModel(const ObjectGroup::Data& group) {
            RE::NiPointer<RE::NiNode> root{ RE::NiNode::Create(
                static_cast<std::uint16_t>(group.members.size())) };
            if (!root) {
                return nullptr;
            }
            root->SetMotionType(RE::hkpMotion::MotionType::kKeyframed, true, false, true);
            root->SetCollisionLayer(RE::COL_LAYER::kNonCollidable);

            for (const ObjectGroup::Member& member : group.members) {
                RE::TESForm* form = RE::TESForm::LookupByID(member.objectFormID);
                RE::TESModel* model = form ? form->As<RE::TESModel>() : nullptr;
                if (!model || !model->GetModel() || model->GetModel()[0] == '\0') {
                    continue;
                }

                RE::NiPointer<RE::NiNode> memberModel = LoadModel(model->GetModel());
                if (!memberModel) {
                    continue;
                }


                RE::NiPointer<RE::NiNode> transformNode{ RE::NiNode::Create(1) };
                if (!transformNode) {
                    continue;
                }
                transformNode->SetMotionType(RE::hkpMotion::MotionType::kKeyframed, true, false, true);
                transformNode->SetCollisionLayer(RE::COL_LAYER::kNonCollidable);

                transformNode->local.translate = member.previewPosition;
                transformNode->local.rotate = member.previewRotation;
                transformNode->local.scale = member.previewScale;
                transformNode->AttachChild(memberModel.get(), true);
                root->AttachChild(transformNode.get(), true);
            }

            RE::NiUpdateData updateData;
            updateData.time = 0.0f;
            updateData.flags.set(RE::NiUpdateData::Flag::kDirty);
            root->Update(updateData);
            return root;
        }

        static bool CacheGroupModel(const ObjectGroup::Data& group) {
            const std::string cacheKey = NormalizeModelPath(group.meshPath);
            {
                std::scoped_lock lock(cacheMutex);
                if (cache.contains(cacheKey)) {
                    return true;
                }
            }

            RE::NiPointer<RE::NiNode> model = BuildGroupModel(group);
            if (!model) {
                return false;
            }

            std::scoped_lock lock(cacheMutex);
            if (cache.contains(cacheKey)) {
                return true;
            }

            GroupModelData* modelData = new GroupModelData();
            modelData->modelRoot = model;
            cache[cacheKey] = modelData;
            return true;
        }

        static RE::BSResource::ErrorCode thunk(const char* source, GroupModelData** result, void* context) {
            if (!source || !result) {
                return RE::BSResource::ErrorCode::kInvalidParam;
            }

            {
                std::scoped_lock lock(cacheMutex);
                const auto cached = cache.find(NormalizeModelPath(source));
                if (cached != cache.end()) {
                    *result = cached->second;
                    return RE::BSResource::ErrorCode::kNone;
                }
            }

            return originalFunction(source, result, context);
        }

        static void ClearCache() {
            std::scoped_lock lock(cacheMutex);
            for (const auto& [meshPath, modelData] : cache) {
                // BSModelDB may retain the raw entry after this hook returns.
                // Keep retired entries alive until process exit.
                retainedModelData.push_back(modelData);
            }
            cache.clear();
        }

        static void Install() {
            originalFunction = Detour::write_prologue_hook(
                REL::RelocationID(74039, 75781).address(),
                thunk);
        }

        static inline std::map<std::string, GroupModelData*> cache;
        static inline std::vector<GroupModelData*> retainedModelData;
        static inline std::mutex cacheMutex;
        static inline REL::Relocation<decltype(thunk)> originalFunction;
    };

    struct UpdateLoop {
        static constexpr std::chrono::milliseconds tickInterval{ 10 };

        static void Tick() {
            DrawDebug::Clean();
            Picker::Tick();
            Placer::Tick();
            Shader::ProcessPendingReferenceChanges();
            if (Menu::IsOpen()) {
                HUD::OnMenuOpen();
            } else {
                HUD::OnMenuClose();
            }
        }

        static void QueueTick() {
            if (tickPending.exchange(true)) {
                return;
            }

            const SKSE::TaskInterface* taskInterface = SKSE::GetTaskInterface();
            if (!taskInterface) {
                tickPending.store(false);
                return;
            }

            taskInterface->AddTask([]() {
                Tick();
                tickPending.store(false);
            });
        }

        static void Run(std::stop_token stopToken) {
            while (!stopToken.stop_requested()) {
                std::this_thread::sleep_for(tickInterval);
                if (!stopToken.stop_requested()) {
                    QueueTick();
                }
            }
        }

        static void Start() {
            if (!worker.joinable()) {
                worker = std::jthread(Run);
            }
        }

        static inline std::atomic_bool tickPending = false;
        static inline std::jthread worker;
    };

    struct NiAVObjectUpdateHook {
        static void thunk(RE::NiAVObject* a_object, RE::NiUpdateData& a_data) {
            originalFunction(a_object, a_data);
            if (!a_object) {
                return;
            }

            RE::TESObjectREFR* reference = a_object->GetUserData();
            if (!reference || reference->Get3D() != a_object) {
                return;
            }

            const RE::ObjectRefHandle handle = reference->GetHandle();
            if (!HUD::GetIsEnabled() || !Shader::IsMovable(handle)) {
                return;
            }

            Transform::ClearFade(handle);
            Shader::QueueReferenceHighlight(handle, a_object);
        }

        static inline REL::Relocation<decltype(thunk)> originalFunction;

        static void Install() {
            originalFunction = Detour::write_prologue_hook(
                REL::RelocationID(68900, 70251).address(),
                thunk);
        }
    };

    struct ObjectReferenceRelease3DHook {
        static void thunk(RE::TESObjectREFR* a_reference) {
            if (a_reference) {
                Shader::QueueReferenceRelease(a_reference->GetHandle());
            }
            originalFunction(a_reference);
        }

        static inline REL::Relocation<decltype(thunk)> originalFunction;

        static void Install() {
            originalFunction =
                REL::Relocation<std::uintptr_t>(RE::TESObjectREFR::VTABLE[0]).write_vfunc(0x6B, thunk);
        }
    };

    struct ProcessQueueHook
    {
        static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent* const* a_event)
        {
            if (!a_event) {
                originalFunction(a_dispatcher, a_event);
                return;
            }

            RE::InputEvent* filteredEvent = InputEventHandler::Process(*a_event);
            originalFunction(a_dispatcher, std::addressof(filteredEvent));
        }

        static inline REL::Relocation<decltype(thunk)> originalFunction;

        static void Install()
        {
            auto& trampoline = SKSE::GetTrampoline();
            originalFunction = trampoline.write_call<5>(
                REL::RelocationID(67315, 68617).address() + REL::Relocate(0x7B, 0x7B), thunk);
        }
    };

    struct RemoveItemHook
    {
        // The raw vtable function uses the MSVC hidden return parameter for
        // ObjectRefHandle immediately after the instance pointer.
        static RE::ObjectRefHandle* thunk(
            RE::PlayerCharacter* a_player,
            RE::ObjectRefHandle& a_result,
            RE::TESBoundObject* a_item,
            std::int32_t a_count,
            RE::ITEM_REMOVE_REASON a_reason,
            RE::ExtraDataList* a_extraList,
            RE::TESObjectREFR* a_moveToRef,
            const RE::NiPoint3* a_dropLoc = nullptr,
            const RE::NiPoint3* a_rotate = nullptr)
        {
            if (a_reason == RE::ITEM_REMOVE_REASON::kDropping &&
                (ObjectGroup::IsGroupItem(a_item) ||
                    PlacementItems::IsItem(a_item)) &&
                a_player) {
                const std::int32_t itemCount = a_player->GetItemCount(a_item);
                RE::ObjectRefHandle* result = originalFunction(
                    a_player,
                    a_result,
                    a_item,
                    1,
                    RE::ITEM_REMOVE_REASON::kRemove,
                    a_extraList,
                    nullptr,
                    nullptr,
                    nullptr);

                if (a_player->GetItemCount(a_item) < itemCount &&
                    !Placer::RequestDrop(a_item, true)) {
                    a_player->AddObjectToContainer(a_item, nullptr, 1, nullptr);
                }

                return result;
            }

            return originalFunction(
                a_player,
                a_result,
                a_item,
                a_count,
                a_reason,
                a_extraList,
                a_moveToRef,
                a_dropLoc,
                a_rotate);
        }

        static inline REL::Relocation<decltype(thunk)> originalFunction;

        static void Install()
        {
            originalFunction =
                REL::Relocation<std::uintptr_t>(RE::PlayerCharacter::VTABLE[0])
                    .write_vfunc(0x56, thunk);
        }
    };

    struct LoadMiscItemHook
    {
        static void thunk(RE::TESObjectMISC* a_object, RE::BGSLoadFormBuffer* a_buffer)
        {
            originalFunction(a_object, a_buffer);
            if (a_object) {
                ObjectGroup::ReviveItem(a_object, reinterpret_cast<std::uint32_t&>(a_object->value));
            }

        }

        static inline REL::Relocation<decltype(thunk)> originalFunction;

        static void Install()
        {
            originalFunction =
                REL::Relocation<std::uintptr_t>(RE::TESObjectMISC::VTABLE[0]).write_vfunc(0x0F, thunk);
        }
    };

    struct SaveMiscItemHook {
        static void thunk(RE::TESObjectMISC* a_object, RE::BGSSaveFormBuffer* a_buffer) {
            if (a_object && ObjectGroup::IsGroupItem(a_object)) {
                auto value = a_object->value;
                auto id = a_object->GetFormID();
                a_object->value = reinterpret_cast<int32_t&>(id);
                originalFunction(a_object, a_buffer);
                a_object->value = value;
                return;
            }

            originalFunction(a_object, a_buffer);
        }

        static inline REL::Relocation<decltype(thunk)> originalFunction;

        static void Install() { originalFunction = REL::Relocation<std::uintptr_t>(RE::TESObjectMISC::VTABLE[0]).write_vfunc(0x0E, thunk); }
    };

    struct SaveGameHook
    {
        static char* thunk(
            RE::BGSSaveLoadManager* a_manager, void* a_2, char* a_fileName, void* a_4, std::int32_t a_5)
        {
            RE::BSWin32SaveDataSystemUtility* utility = RE::BSWin32SaveDataSystemUtility::GetSingleton();
            char fullPath[242];
            utility->PrepareFileSavePath(a_fileName, fullPath, 0, 0);
            Persistence::Save(fullPath);
            return originalFunction(a_manager, a_2, a_fileName, a_4, a_5);
        }

        static inline REL::Relocation<decltype(thunk)> originalFunction;

        static void Install()
        {
            auto& trampoline = SKSE::GetTrampoline();
            originalFunction = trampoline.write_call<5>(
                REL::RelocationID(34818, 35727).address() + REL::Relocate(0x112, 0x1ce), thunk);
        }
    };

    struct LoadGameHook
    {
        static std::int32_t thunk(
            RE::BSWin32SaveDataSystemUtility* a_utility, char* a_fileName, void* a_unknown)
        {
            GroupModelHook::ClearCache();
            char fullPath[242];
            a_utility->PrepareFileSavePath(a_fileName, fullPath, 0, 0);
            Persistence::Load(fullPath);
            return originalFunction(a_utility, a_fileName, a_unknown);
        }

        static inline REL::Relocation<decltype(thunk)> originalFunction;

        static void Install()
        {
            auto& trampoline = SKSE::GetTrampoline();
            originalFunction = trampoline.write_call<5>(
                REL::RelocationID(34677, 35600).address() + REL::Relocate(0xab, 0xab), thunk);
        }
    };


    struct InventoryHoverHook
    {
        static std::int64_t thunk(RE::InventoryEntryData* a_entry)
        {
            if (a_entry && a_entry->object) {
                RE::Inventory3DManager* manager = RE::Inventory3DManager::GetSingleton();
                if (manager->tempRef) {
                    HUD::ShowInventoryClone(manager->tempRef->GetHandle(), a_entry->object);
                } else {
                    HUD::HideInventoryClone();
                }
            } else {
                HUD::HideInventoryClone();
            }

            return originalFunction(a_entry);
        }

        static inline REL::Relocation<decltype(thunk)> originalFunction;

        static void Install()
        {
            auto& trampoline = SKSE::GetTrampoline();
            const REL::Relocation<std::uintptr_t> function{ REL::RelocationID(51019, 51897) };
            originalFunction =
                trampoline.write_call<5>(function.address() + REL::Relocate(0x114, 0x22c), thunk);
        }
    };

}

void Hooks::Install()
{
    SKSE::AllocTrampoline(14*6);
    GroupModelHook::Install();
    ProcessQueueHook::Install();
    RemoveItemHook::Install();
    LoadMiscItemHook::Install();
    SaveMiscItemHook::Install();
    SaveGameHook::Install();
    LoadGameHook::Install();
    InventoryHoverHook::Install();
    NiAVObjectUpdateHook::Install();
    ObjectReferenceRelease3DHook::Install();
    UpdateLoop::Start();
}

bool Hooks::CacheGroupModel(const ObjectGroup::Data& group)
{
    return GroupModelHook::CacheGroupModel(group);
}

void Hooks::RefreshLoadedReferenceHighlights()
{
    RE::TES* tes = RE::TES::GetSingleton();
    if (!tes) {
        return;
    }

    tes->ForEachReference([](RE::TESObjectREFR* a_reference) {
        if (a_reference && a_reference->Get3D()) {
            Shader::QueueReferenceHighlight(
                a_reference->GetHandle(),
                a_reference->Get3D());
        }
        return RE::BSContainer::ForEachResult::kContinue;
    });
}

void Hooks::RetireGroupModel(std::string_view meshPath)
{
    std::scoped_lock lock(GroupModelHook::cacheMutex);
    const auto cached = GroupModelHook::cache.find(NormalizeModelPath(meshPath));
    if (cached == GroupModelHook::cache.end()) {
        return;
    }

    // The resource/render thread may still be cloning this raw model entry.
    // Removing it from active lookup is safe; freeing it here is not.
    GroupModelHook::retainedModelData.push_back(cached->second);
    GroupModelHook::cache.erase(cached);
}

std::size_t Hooks::GetGroupModelCacheSize()
{
    std::scoped_lock lock(GroupModelHook::cacheMutex);
    return GroupModelHook::cache.size();
}

void Hooks::VisitGroupModelCache(const std::function<void(std::string_view)>& visitor)
{
    std::scoped_lock lock(GroupModelHook::cacheMutex);
    for (const auto& cacheEntry : GroupModelHook::cache) {
        visitor(cacheEntry.first);
    }
}
