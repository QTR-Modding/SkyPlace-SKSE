#include "Shader.h"
#include "FormManager.h"
#include "HUD.h"
#include "PlacementItems.h"
#include "Picker.h"
#include "Placer.h"

#include <mutex>

namespace {
    enum class PendingReferenceChangeType {
        kLoad,
        kRelease,
        kClearAll
    };

    struct PendingReferenceChange {
        PendingReferenceChangeType type = PendingReferenceChangeType::kLoad;
        RE::FormID formID = 0;
        RE::ObjectRefHandle handle;
        RE::NiPointer<RE::NiAVObject> scenegraph;
    };

    std::unordered_map<RE::FormID, RE::ObjectRefHandle> highlightedReferences;
    std::mutex highlightedReferencesMutex;
    std::vector<PendingReferenceChange> pendingReferenceChanges;
    std::mutex pendingReferenceChangesMutex;

    void TrackReference(const RE::ObjectRefHandle& handle) {
        const RE::NiPointer<RE::TESObjectREFR> reference = handle.get();
        if (reference) {
            std::scoped_lock lock(highlightedReferencesMutex);
            highlightedReferences[reference->GetFormID()] = handle;
        }
    }

    void ForgetReference(RE::FormID formID) {
        std::scoped_lock lock(highlightedReferencesMutex);
        highlightedReferences.erase(formID);
    }

    void ApplyHighlight(
        const RE::ObjectRefHandle& handle,
        const RE::NiColorA& fillColor,
        const RE::NiColorA& rimColor) {
        if (!HUD::GetIsEnabled() || !Shader::IsMovable(handle)) {
            Shader::ClearReferenceHighlight(handle);
            return;
        }

        const RE::NiPointer<RE::TESObjectREFR> reference = handle.get();
        if (!reference) {
            return;
        }
        RE::NiPointer<RE::NiAVObject> reference3D{reference->Get3D()};
        if (!reference3D) {
            ForgetReference(reference->GetFormID());
            return;
        }

        Shader::TintScenegraph(reference3D.get(), fillColor, rimColor);
        TrackReference(handle);
    }
}

void Shader::TintScenegraph(RE::NiAVObject* a_obj, const RE::NiColorA& a_color_1, const RE::NiColorA& a_color_2) {
    if (!a_obj) {
        return;
    }

    auto gState = RE::BSGraphics::State::GetSingleton();
    RE::BSTSmartPointer<RE::BSEffectShaderData> newShaderData(new RE::BSEffectShaderData());
    newShaderData->fillColor = a_color_1;
    newShaderData->rimColor = a_color_2;
    newShaderData->ignoreBaseGeomTexAlpha = true;
    newShaderData->ignoreTextureAlpha = true;
    newShaderData->lighting = false;
    newShaderData->baseTexture = gState->GetRuntimeData().defaultTextureWhite;

    RE::BSVisit::TraverseScenegraphGeometries(a_obj, [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
        auto shaderProp = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
        if (shaderProp && shaderProp->AcceptsEffectData()) {
            auto shaderData = shaderProp->effectData;
            if (!shaderData || shaderData->baseTexture == gState->GetRuntimeData().defaultTextureWhite) {
                shaderProp->SetEffectShaderData(newShaderData);
            }
        }

        return RE::BSVisit::BSVisitControl::kContinue;
    });
}
void Shader::RemoveTintScenegraph(RE::NiAVObject* a_obj) {
    if (!a_obj) {
        return;
    }

    auto gState = RE::BSGraphics::State::GetSingleton();
    auto defaultWhite = gState->GetRuntimeData().defaultTextureWhite;

    RE::BSVisit::TraverseScenegraphGeometries(a_obj, [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
        if (!a_geometry) {
            return RE::BSVisit::BSVisitControl::kContinue;
        }

        auto shaderProp = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
        if (!shaderProp || !shaderProp->AcceptsEffectData()) {
            return RE::BSVisit::BSVisitControl::kContinue;
        }

        auto shaderData = shaderProp->effectData;
        if (!shaderData) {
            return RE::BSVisit::BSVisitControl::kContinue;
        }

        if (shaderData->baseTexture == defaultWhite && shaderData->ignoreBaseGeomTexAlpha && shaderData->ignoreTextureAlpha) {
            shaderProp->SetEffectShaderData(nullptr);
        }

        return RE::BSVisit::BSVisitControl::kContinue;
    });
}

bool Shader::IsMovable(const RE::ObjectRefHandle& handle) {
    const RE::NiPointer<RE::TESObjectREFR> refr = handle.get();
    if (!refr) {
        return false;
    }

    RE::TESBoundObject* baseObject = refr->GetBaseObject();
    if (!baseObject) {
        return false;
    }

    return FormManager::Get(baseObject->GetFormID()).has_value() ||
        PlacementItems::HasItemForObject(baseObject);
}

void Shader::ApplyPickableHighlight(const RE::ObjectRefHandle& handle) {
    ApplyHighlight(handle, pickableFillColor, pickableRimColor);
}

void Shader::ApplyHoverHighlight(const RE::ObjectRefHandle& handle) {
    ApplyHighlight(handle, hoverFillColor, hoverRimColor);
}

void Shader::ApplySelectedHighlight(const RE::ObjectRefHandle& handle) {
    ApplyHighlight(handle, selectedFillColor, selectedRimColor);
}

void Shader::RefreshReferenceHighlight(const RE::ObjectRefHandle& handle) {
    if (!HUD::GetIsEnabled() || !IsMovable(handle)) {
        ClearReferenceHighlight(handle);
        return;
    }

    if (Picker::IsSelected(handle)) {
        ApplySelectedHighlight(handle);
        return;
    }

    if (Placer::IsPlacing() && Placer::IsGroupMember(handle)) {
        ApplyHoverHighlight(handle);
        return;
    }

    if (Picker::GetLastHoverHandle() == handle) {
        ApplyHoverHighlight(handle);
        return;
    }

    ApplyPickableHighlight(handle);
}

void Shader::QueueReferenceHighlight(
    const RE::ObjectRefHandle& handle,
    RE::NiAVObject* loaded3D)
{
    if (!handle || !loaded3D) {
        return;
    }

    PendingReferenceChange change;
    change.type = PendingReferenceChangeType::kLoad;
    change.handle = handle;
    change.scenegraph.reset(loaded3D);

    std::scoped_lock lock(pendingReferenceChangesMutex);
    pendingReferenceChanges.push_back(std::move(change));
}

void Shader::QueueReferenceRelease(const RE::ObjectRefHandle& handle) {
    const RE::NiPointer<RE::TESObjectREFR> reference = handle.get();
    if (!reference) {
        return;
    }

    PendingReferenceChange change;
    change.type = PendingReferenceChangeType::kRelease;
    change.formID = reference->GetFormID();
    change.handle = handle;

    std::scoped_lock lock(pendingReferenceChangesMutex);
    pendingReferenceChanges.push_back(std::move(change));
}

void Shader::QueueClearAllReferenceHighlights() {
    PendingReferenceChange change;
    change.type = PendingReferenceChangeType::kClearAll;

    std::scoped_lock lock(pendingReferenceChangesMutex);
    pendingReferenceChanges.push_back(std::move(change));
}

void Shader::ProcessPendingReferenceChanges() {
    std::vector<PendingReferenceChange> changes;
    {
        std::scoped_lock lock(pendingReferenceChangesMutex);
        changes.swap(pendingReferenceChanges);
    }

    for (const PendingReferenceChange& change : changes) {
        if (change.type == PendingReferenceChangeType::kRelease) {
            ForgetReference(change.formID);
            const RE::NiPointer<RE::TESObjectREFR> reference = change.handle.get();
            // Room changes transiently release the 3D without deleting the
            // reference. Keep its logical selection for the replacement 3D.
            if (!reference || reference->IsDeleted()) {
                Picker::RemoveReference(change.formID);
            }
            continue;
        }
        if (change.type == PendingReferenceChangeType::kClearAll) {
            ClearAllReferenceHighlights();
            continue;
        }

        const RE::NiPointer<RE::TESObjectREFR> reference = change.handle.get();
        if (!reference ||
            reference->IsDeleted() ||
            reference->Get3D() != change.scenegraph.get()) {
            continue;
        }

        RefreshReferenceHighlight(change.handle);
    }
}

void Shader::ClearReferenceHighlight(const RE::ObjectRefHandle& handle) {
    const RE::NiPointer<RE::TESObjectREFR> refr = handle.get();
    if (!refr) {
        return;
    }

    const RE::NiPointer<RE::NiAVObject> reference3D{refr->Get3D()};
    RemoveTintScenegraph(reference3D.get());
    ForgetReference(refr->GetFormID());
}

void Shader::ClearAllReferenceHighlights() {
    std::unordered_map<RE::FormID, RE::ObjectRefHandle> references;
    {
        std::scoped_lock lock(highlightedReferencesMutex);
        references.swap(highlightedReferences);
    }

    for (const auto& entry : references) {
        const RE::NiPointer<RE::TESObjectREFR> reference = entry.second.get();
        if (reference) {
            const RE::NiPointer<RE::NiAVObject> reference3D{reference->Get3D()};
            RemoveTintScenegraph(reference3D.get());
        }
    }
}

std::unordered_map<RE::FormID, RE::ObjectRefHandle> Shader::GetHighlightedReferences() {
    std::scoped_lock lock(highlightedReferencesMutex);
    return highlightedReferences;
}
