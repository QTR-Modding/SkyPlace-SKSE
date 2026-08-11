#pragma once

namespace PlacementItems {
    void Install();
    bool IsItem(RE::TESBoundObject* item);
    bool HasItemForObject(RE::TESBoundObject* object);
    RE::TESObjectMISC* GetItemForObject(RE::TESBoundObject* object);
    bool Materialize(
        RE::TESBoundObject* item,
        std::vector<RE::ObjectRefHandle>& materializedHandles);
}
