#include "DynamicForm.h"
#include "FormManager.h"

FormID DynamicForm::Create(FormID objId) {
    auto factory = RE::IFormFactory::GetFormFactoryByType(RE::TESObjectMISC::FORMTYPE);
    if (auto form = factory->Create()) {
        if (auto misc = form->As<RE::TESObjectMISC>()) {
            misc->AddChange(1 | 2);
            misc->SetFormID(misc->GetFormID(), true);
            Revive(objId, misc->GetFormID());
            return misc->GetFormID();
        }
    }
    return 0;
}
void DynamicForm::Revive(FormID objId, FormID miscId) {
    auto object = RE::TESForm::LookupByID(objId);
    auto misc = RE::TESForm::LookupByID(miscId);

    if (!object || !misc) {
        logger::trace("not found");
        return;
    }

    if (auto objectTrait = object->As<RE::TESModel>()) {
        if (auto miscTrait = misc->As<RE::TESModel>()) {
            logger::trace("set model");
            miscTrait->SetModel(objectTrait->GetModel());
        }
    }
    auto filter = FormManager::Get(object->GetFormID());

    if (auto miscTrait = misc->As<RE::TESFullName>()) {
        if (auto objectTrait = object->As<RE::TESFullName>()) {
            miscTrait->SetFullName(objectTrait->GetFullName());
        } else if (filter) {
            miscTrait->SetFullName(filter->name.c_str());
        }
    }

    if (auto miscTrait = misc->As<RE::TESValueForm>()) {
        auto objectTrait = object->As<RE::TESValueForm>();
        miscTrait->value = filter ?
            filter->value :
            (objectTrait ? objectTrait->value : 0);
    }

    if (auto miscTrait = misc->As<RE::TESWeightForm>()) {
        auto objectTrait = object->As<RE::TESWeightForm>();
        miscTrait->weight = filter ?
            filter->weight :
            (objectTrait ? objectTrait->weight : 0.0f);
    }
}
