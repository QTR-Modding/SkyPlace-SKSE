#include "Papyrus.h"

#include "FormManager.h"
#include "Picker.h"
#include "Placer.h"

namespace {
    constexpr std::string_view scriptName = "SkyPlace";

    bool IsMovableReference(RE::TESObjectREFR* reference) {
        if (!reference || reference->IsPlayerRef()) {
            return false;
        }

        RE::TESBoundObject* baseObject = reference->GetBaseObject();
        return baseObject && FormManager::Get(baseObject->GetFormID()).has_value();
    }

    bool PickUpObject(
        RE::StaticFunctionTag*,
        RE::TESObjectREFR* reference)
    {
        if (!IsMovableReference(reference)) {
            return false;
        }

        return Picker::PickObjects({reference->GetHandle()});
    }

    bool PickUpMovingObject(RE::StaticFunctionTag*) {
        if (!Placer::IsPlacing()) {
            return false;
        }

        Placer::PickEvent();
        return true;
    }

    bool PlaceMovingObject(RE::StaticFunctionTag*) {
        if (!Placer::IsPlacing()) {
            return false;
        }

        Placer::PlaceEvent();
        return true;
    }

    bool MoveObject(
        RE::StaticFunctionTag*,
        RE::TESObjectREFR* reference)
    {
        if (!IsMovableReference(reference) || Placer::IsPlacing()) {
            return false;
        }

        Picker::MoveEvent(reference->GetHandle());
        return true;
    }

    bool PlaceObjectFromPlayerInventory(
        RE::StaticFunctionTag*,
        RE::TESObjectMISC* item)
    {
        return item && Placer::RequestDrop(item);
    }
}

bool PapyrusAPI::Register(RE::BSScript::IVirtualMachine* virtualMachine) {
    if (!virtualMachine) {
        return false;
    }

    virtualMachine->RegisterFunction("PickUpObject", scriptName, PickUpObject);
    virtualMachine->RegisterFunction("PickUpMovingObject", scriptName, PickUpMovingObject);
    virtualMachine->RegisterFunction("PlaceMovingObject", scriptName, PlaceMovingObject);
    virtualMachine->RegisterFunction("MoveObject", scriptName, MoveObject);
    virtualMachine->RegisterFunction(
        "PlaceObjectFromPlayerInventory",
        scriptName,
        PlaceObjectFromPlayerInventory);

    return true;
}
