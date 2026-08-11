#include "FormManager.h"
#include "FormsByModelPath.h"
#include "SkyPlaceConfig.h"

 std::optional<FormManagerData> FormManager::Get(RE::FormID id) {

    auto form = RE::TESForm::LookupByID(id);

    if (!form) {
         return {};
    }


    FormManagerData result;

    bool found = false;

    if (auto data = FormsByModelPath::Get(form)) {
        result.weight = data->weight;
        result.name = data->name;
        result.value = data->value;
        found = true;
    }

    if (found) {
        return result;
    }

    if (!SkyPlaceConfig::ShouldIgnoreFilters()) {
        return {};
    }

    if (RE::TESFullName* fullName = form->As<RE::TESFullName>()) {
        result.name = fullName->GetFullName();
    }
    if (RE::TESWeightForm* weightForm = form->As<RE::TESWeightForm>()) {
        result.weight = weightForm->weight;
    }
    if (RE::TESValueForm* valueForm = form->As<RE::TESValueForm>()) {
        result.value = valueForm->value;
    }

    return result;
 }
