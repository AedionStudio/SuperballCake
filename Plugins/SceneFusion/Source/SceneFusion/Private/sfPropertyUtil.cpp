#include "sfPropertyUtil.h"
#include "SceneFusion.h"

#include <Runtime/CoreUObject/Public/UObject/UnrealType.h>
#include <Runtime/CoreUObject/Public/UObject/EnumProperty.h>
#include <Runtime/CoreUObject/Public/UObject/TextProperty.h>

#define LOG_CHANNEL "sfPropertyUtil"

std::unordered_map<int, sfPropertyUtil::TypeHandler> sfPropertyUtil::m_typeHandlers;

using namespace KS;
   
sfUPropertyInstance sfPropertyUtil::FindUProperty(UObject* uobjPtr, sfProperty::SPtr propPtr)
{
    if (uobjPtr == nullptr || propPtr == nullptr)
    {
        return sfUPropertyInstance();
    }
    // Push property and its ancestors into a stack, so we can then iterate them from the top down
    // We don't need to push the root dictionary into the stack
    std::stack<sfProperty::SPtr> stack;
    while (propPtr->GetDepth() > 0)
    {
        stack.push(propPtr);
        propPtr = propPtr->GetParentProperty();
    }
    UProperty* upropPtr = nullptr;
    void* ptr = nullptr;// pointer to UProperty instance data
    TSharedPtr<FScriptMapHelper> mapPtr = nullptr;
    TSharedPtr<FScriptSetHelper> setPtr = nullptr;
    // Traverse properties from the top down, finding the UProperty at each level until we reach the one we want or
    // don't find what we expect.
    while (stack.size() > 0)
    {
        propPtr = stack.top();
        stack.pop();
        if (upropPtr == nullptr)
        {
            // Get the first property from the object
            upropPtr = uobjPtr->GetClass()->FindPropertyByName(FName(UTF8_TO_TCHAR(propPtr->Key()->c_str())));
            if (upropPtr == nullptr)
            {
                return sfUPropertyInstance();
            }
            ptr = upropPtr->ContainerPtrToValuePtr<void>(uobjPtr);
            continue;
        }
        if (!GetStructField(propPtr->Key(), upropPtr, ptr) &&
            !GetArrayElement(propPtr->Index(), upropPtr, ptr) &&
            !GetMapElement(propPtr->Index(), upropPtr, ptr, mapPtr, stack) &&
            !GetSetElement(propPtr->Index(), upropPtr, ptr, setPtr))
        {
            // We were expecting the UProperty to be one of the above container types but it was not. Abort.
            return sfUPropertyInstance();
        }
        if (upropPtr == nullptr)
        {
            // We did not find the field or element we were looking for. Abort.
            return sfUPropertyInstance();
        }
    }
    return sfUPropertyInstance(upropPtr, ptr, mapPtr, setPtr);
}

bool sfPropertyUtil::GetStructField(const sfName& name, UProperty*& upropPtr, void*& ptr)
{
    UStructProperty* structPropPtr = Cast<UStructProperty>(upropPtr);
    if (structPropPtr == nullptr)
    {
        return false;
    }
    if (!name.IsValid())
    {
        upropPtr = nullptr;
        return true;
    }
    upropPtr = structPropPtr->Struct->FindPropertyByName(FName(UTF8_TO_TCHAR(name->c_str())));
    if (upropPtr != nullptr)
    {
        ptr = upropPtr->ContainerPtrToValuePtr<void>(ptr);
    }
    return true;
}

bool sfPropertyUtil::GetArrayElement(int index, UProperty*& upropPtr, void*& ptr)
{
    UArrayProperty* arrayPropPtr = Cast<UArrayProperty>(upropPtr);
    if (arrayPropPtr == nullptr)
    {
        return false;
    }
    FScriptArrayHelper array(arrayPropPtr, ptr);
    if (index < 0 || index >= array.Num())
    {
        upropPtr = nullptr;
    }
    else
    {
        upropPtr = arrayPropPtr->Inner;
        ptr = array.GetRawPtr(index);
    }
    return true;
}

bool sfPropertyUtil::GetMapElement(
    int index,
    UProperty*& upropPtr,
    void*& ptr,
    TSharedPtr<FScriptMapHelper>& outMapPtr,
    std::stack<sfProperty::SPtr>& propertyStack)
{
    UMapProperty* mapPropPtr = Cast<UMapProperty>(upropPtr);
    if (mapPropPtr == nullptr)
    {
        return false;
    }
    // Because maps are serialized as lists of key values, we expect at least one more property in the stack
    if (propertyStack.size() <= 0)
    {
        upropPtr = nullptr;
        return true;
    }
    outMapPtr = MakeShareable(new FScriptMapHelper(mapPropPtr, ptr));
    if (index < 0 || index >= outMapPtr->Num())
    {
        upropPtr = nullptr;
        return true;
    }
    int sparseIndex = -1;
    while (index >= 0)
    {
        sparseIndex++;
        if (sparseIndex >= outMapPtr->GetMaxIndex())
        {
            upropPtr = nullptr;
            return true;
        }
        if (outMapPtr->IsValidIndex(sparseIndex))
        {
            index--;
        }
    }
    // Get the next property in the stack, and check its index to determine if we want the map key or value.
    sfProperty::SPtr propPtr = propertyStack.top();
    propertyStack.pop();
    if (propPtr->Index() == 0)
    {
        upropPtr = mapPropPtr->KeyProp;
        ptr = outMapPtr->GetKeyPtr(sparseIndex);
    }
    else if (propPtr->Index() == 1)
    {
        upropPtr = mapPropPtr->ValueProp;
        ptr = outMapPtr->GetValuePtr(sparseIndex);
        outMapPtr = nullptr;
    }
    else
    {
        upropPtr = nullptr;
    }
    return true;
}

bool sfPropertyUtil::GetSetElement(
    int index,
    UProperty*& upropPtr,
    void*& ptr,
    TSharedPtr<FScriptSetHelper>& outSetPtr)
{
    USetProperty* setPropPtr = Cast<USetProperty>(upropPtr);
    if (setPropPtr == nullptr)
    {
        return false;
    }
    outSetPtr = MakeShareable(new FScriptSetHelper(setPropPtr, ptr));
    if (index < 0 || index >= outSetPtr->Num())
    {
        upropPtr = nullptr;
        return true;
    }
    int sparseIndex = -1;
    while (index >= 0)
    {
        sparseIndex++;
        if (sparseIndex >= outSetPtr->GetMaxIndex())
        {
            upropPtr = nullptr;
            return true;
        }
        if (outSetPtr->IsValidIndex(sparseIndex))
        {
            index--;
        }
    }
    upropPtr = setPropPtr->ElementProp;
    ptr = outSetPtr->GetElementPtr(sparseIndex);
    return true;
}

sfProperty::SPtr sfPropertyUtil::GetValue(UObject* uobjPtr, UProperty* upropPtr)
{
    if (uobjPtr == nullptr || upropPtr == nullptr)
    {
        return nullptr;
    }
    if (m_typeHandlers.size() == 0)
    {
        Initialize();
    }
    auto iter = m_typeHandlers.find(upropPtr->GetClass()->GetFName().GetComparisonIndex());
    return iter == m_typeHandlers.end() ? nullptr : iter->second.Get(sfUPropertyInstance(upropPtr,
        upropPtr->ContainerPtrToValuePtr<void>(uobjPtr)));
}

void sfPropertyUtil::SetValue(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr)
{
    if (!uprop.IsValid() || propPtr == nullptr)
    {
        return;
    }
    if (m_typeHandlers.size() == 0)
    {
        Initialize();
    }
    auto iter = m_typeHandlers.find(uprop.Property()->GetClass()->GetFName().GetComparisonIndex());
    if (iter != m_typeHandlers.end())
    {
        iter->second.Set(uprop, propPtr);
    }
}

bool sfPropertyUtil::IsDefaultValue(UObject* uobjPtr, UProperty* upropPtr)
{
    if (uobjPtr == nullptr || upropPtr == nullptr)
    {
        return false;
    }
    if (m_typeHandlers.size() == 0)
    {
        Initialize();
    }
    if (m_typeHandlers.find(upropPtr->GetClass()->GetFName().GetComparisonIndex()) != m_typeHandlers.end())
    {
        return upropPtr->Identical_InContainer(uobjPtr, uobjPtr->GetClass()->GetDefaultObject());
    }
    return false;
}

void sfPropertyUtil::SetToDefaultValue(UObject* uobjPtr, UProperty* upropPtr)
{
    if (uobjPtr == nullptr || upropPtr == nullptr)
    {
        return;
    }
    if (m_typeHandlers.size() == 0)
    {
        Initialize();
    }
    if (m_typeHandlers.find(upropPtr->GetClass()->GetFName().GetComparisonIndex()) != m_typeHandlers.end())
    {
        upropPtr->CopyCompleteValue_InContainer(uobjPtr, uobjPtr->GetClass()->GetDefaultObject());
    }
}

void sfPropertyUtil::CreateProperties(UObject* uobjPtr, sfDictionaryProperty::SPtr dictPtr)
{
    if (uobjPtr == nullptr || dictPtr == nullptr)
    {
        return;
    }
    for (TFieldIterator<UProperty> iter(uobjPtr->GetClass()); iter; ++iter)
    {
        if (iter->PropertyFlags & CPF_Edit && !(iter->PropertyFlags & CPF_DisableEditOnInstance) &&
            !IsDefaultValue(uobjPtr, *iter))
        {
            sfProperty::SPtr propPtr = GetValue(uobjPtr, *iter);
            if (propPtr != nullptr)
            {
                std::string name = std::string(TCHAR_TO_UTF8(*iter->GetName()));
                dictPtr->Set(name, propPtr);
            }
        }
    }
}

void sfPropertyUtil::ApplyProperties(UObject* uobjPtr, sfDictionaryProperty::SPtr dictPtr)
{
    if (uobjPtr == nullptr || dictPtr == nullptr)
    {
        return;
    }
    for (TFieldIterator<UProperty> iter(uobjPtr->GetClass()); iter; ++iter)
    {
        if (iter->PropertyFlags & CPF_Edit && !(iter->PropertyFlags & CPF_DisableEditOnInstance))
        {
            std::string name = std::string(TCHAR_TO_UTF8(*iter->GetName()));
            sfProperty::SPtr propPtr;
            if (!dictPtr->TryGet(name, propPtr))
            {
                SetToDefaultValue(uobjPtr, *iter);
            }
            else
            {
                SetValue(sfUPropertyInstance(*iter, iter->ContainerPtrToValuePtr<void>(uobjPtr)), propPtr);
            }
        }
    }
}

void sfPropertyUtil::SendPropertyChanges(UObject* uobjPtr, sfDictionaryProperty::SPtr dictPtr)
{
    if (uobjPtr == nullptr || dictPtr == nullptr)
    {
        return;
    }
    for (TFieldIterator<UProperty> iter(uobjPtr->GetClass()); iter; ++iter)
    {
        if (iter->PropertyFlags & CPF_Edit && !(iter->PropertyFlags & CPF_DisableEditOnInstance))
        {
            if (IsDefaultValue(uobjPtr, *iter))
            {
                std::string name = std::string(TCHAR_TO_UTF8(*iter->GetName()));
                dictPtr->Remove(name);
            }
            else
            {
                sfProperty::SPtr propPtr = GetValue(uobjPtr, *iter);
                if (propPtr == nullptr)
                {
                    continue;
                }

                std::string name = std::string(TCHAR_TO_UTF8(*iter->GetName()));
                sfProperty::SPtr oldPropPtr = nullptr;
                if (!dictPtr->TryGet(name, oldPropPtr) || !Copy(oldPropPtr, propPtr))
                {
                    dictPtr->Set(name, propPtr);
                }
            }
        }
    }
}

bool sfPropertyUtil::Copy(sfProperty::SPtr destPtr, sfProperty::SPtr srcPtr)
{
    if (destPtr == nullptr || srcPtr == nullptr || destPtr->Type() != srcPtr->Type())
    {
        return false;
    }
    switch (destPtr->Type())
    {
        case sfProperty::VALUE:
        {
            if (!destPtr->Equals(srcPtr))
            {
                destPtr->AsValue()->SetValue(srcPtr->AsValue()->GetValue());
            }
            break;
        }
        case sfProperty::LIST:
        {
            CopyList(destPtr->AsList(), srcPtr->AsList());
            break;
        }
        case sfProperty::DICTIONARY:
        {
            CopyDict(destPtr->AsDict(), srcPtr->AsDict());
            break;
        }
    }
    return true;
}

// private functions

void sfPropertyUtil::Initialize()
{
    CreateTypeHandler<UBoolProperty>();
    CreateTypeHandler<UFloatProperty>();
    CreateTypeHandler<UIntProperty>();
    CreateTypeHandler<UUInt32Property>();
    CreateTypeHandler<UByteProperty>();
    CreateTypeHandler<UInt64Property>();

    CreateTypeHandler<UInt8Property, uint8_t>();
    CreateTypeHandler<UInt16Property, int>();
    CreateTypeHandler<UUInt16Property, int>();
    CreateTypeHandler<UUInt64Property, int64_t>();

    CreateTypeHandler(UDoubleProperty::StaticClass(), &GetDouble, &SetDouble);
    CreateTypeHandler(UStrProperty::StaticClass(), &GetFString, &SetFString);
    CreateTypeHandler(UTextProperty::StaticClass(), &GetFText, &SetFText);
    CreateTypeHandler(UNameProperty::StaticClass(), &GetFName, &SetFName);
    CreateTypeHandler(UEnumProperty::StaticClass(), &GetEnum, &SetEnum);
    CreateTypeHandler(UArrayProperty::StaticClass(), &GetArray, &SetArray);
    CreateTypeHandler(UMapProperty::StaticClass(), &GetMap, &SetMap);
    CreateTypeHandler(USetProperty::StaticClass(), &GetSet, &SetSet);
    CreateTypeHandler(UStructProperty::StaticClass(), &GetStruct, &SetStruct);
    CreateTypeHandler(UObjectProperty::StaticClass(), &GetObject, &SetObject);
}

void sfPropertyUtil::CreateTypeHandler(UClass* typePtr, TypeHandler::Getter getter, TypeHandler::Setter setter)
{
    int key = typePtr->GetFName().GetComparisonIndex();
    if (m_typeHandlers.find(key) != m_typeHandlers.end())
    {
        KS::Log::Warning("Duplicate handler for type " + std::string(TCHAR_TO_UTF8(*typePtr->GetName())), LOG_CHANNEL);
    }
    m_typeHandlers.emplace(key, TypeHandler(getter, setter));
}

sfProperty::SPtr sfPropertyUtil::GetDouble(const sfUPropertyInstance& uprop)
{
    return sfValueProperty::Create(ksMultiType(ksMultiType::BYTE_ARRAY, (uint8_t*)uprop.Data(), sizeof(double),
        sizeof(double)));
}

void sfPropertyUtil::SetDouble(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr)
{
    const ksMultiType& value = propPtr->AsValue()->GetValue();
    if (value.GetData().size() != sizeof(double))
    {
        KS::Log::Error("Error setting double property " + std::string(TCHAR_TO_UTF8(*uprop.Property()->GetName())) +
            ". Expected " + std::to_string(sizeof(double)) + " bytes, but got " + std::to_string(value.GetData().size()) +
            ".", LOG_CHANNEL);
        return;
    }
    std::memcpy((uint8_t*)uprop.Data(), value.GetData().data(), sizeof(double));
}

sfProperty::SPtr sfPropertyUtil::GetFString(const sfUPropertyInstance& uprop)
{
    return FromString(*(FString*)uprop.Data(), SceneFusion::Service->Session());
}

void sfPropertyUtil::SetFString(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr)
{
    FString* strPtr = (FString*)uprop.Data();
    *strPtr = ToString(propPtr);
}

sfProperty::SPtr sfPropertyUtil::GetFText(const sfUPropertyInstance& uprop)
{
    return FromString(((FText*)uprop.Data())->ToString(), SceneFusion::Service->Session());
}

void sfPropertyUtil::SetFText(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr)
{
    FText* textPtr = (FText*)uprop.Data();
    *textPtr = FText::FromString(ToString(propPtr));
}

sfProperty::SPtr sfPropertyUtil::GetFName(const sfUPropertyInstance& uprop)
{
    return FromString(((FName*)uprop.Data())->ToString(), SceneFusion::Service->Session());
}

void sfPropertyUtil::SetFName(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr)
{
    FName* namePtr = (FName*)uprop.Data();
    *namePtr = *ToString(propPtr);
}

sfProperty::SPtr sfPropertyUtil::GetEnum(const sfUPropertyInstance& uprop)
{
    UEnumProperty* tPtr = Cast<UEnumProperty>(uprop.Property());
    int64_t value = tPtr->GetUnderlyingProperty()->GetSignedIntPropertyValue(uprop.Data());
    if (value >= 0 && value < 256)
    {
        return sfValueProperty::Create((uint8_t)value);
    }
    return sfValueProperty::Create(value);
}

void sfPropertyUtil::SetEnum(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr)
{
    UEnumProperty* tPtr = Cast<UEnumProperty>(uprop.Property());
    int64_t value = propPtr->AsValue()->GetValue();
    tPtr->GetUnderlyingProperty()->SetIntPropertyValue(uprop.Data(), value);
}

sfProperty::SPtr sfPropertyUtil::GetArray(const sfUPropertyInstance& uprop)
{
    UArrayProperty* tPtr = Cast<UArrayProperty>(uprop.Property());
    auto iter = m_typeHandlers.find(tPtr->Inner->GetClass()->GetFName().GetComparisonIndex());
    if (iter == m_typeHandlers.end())
    {
        return nullptr;
    }
    sfListProperty::SPtr listPtr = sfListProperty::Create();
    FScriptArrayHelper array(tPtr, uprop.Data());
    for (int i = 0; i < array.Num(); i++)
    {
        sfProperty::SPtr elementPtr = iter->second.Get(sfUPropertyInstance(tPtr->Inner, (void*)array.GetRawPtr(i)));
        if (elementPtr == nullptr)
        {
            return nullptr;
        }
        listPtr->Add(elementPtr);
    }
    return listPtr;
}

void sfPropertyUtil::SetArray(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr)
{
    UArrayProperty* tPtr = Cast<UArrayProperty>(uprop.Property());
    auto iter = m_typeHandlers.find(tPtr->Inner->GetClass()->GetFName().GetComparisonIndex());
    if (iter == m_typeHandlers.end())
    {
        return;
    }
    sfListProperty::SPtr listPtr = propPtr->AsList();
    FScriptArrayHelper array(tPtr, uprop.Data());
    array.Resize(listPtr->Size());
    for (int i = 0; i < listPtr->Size(); i++)
    {
        iter->second.Set(sfUPropertyInstance(tPtr->Inner, (void*)array.GetRawPtr(i)), listPtr->Get(i));
    }
}

sfProperty::SPtr sfPropertyUtil::GetMap(const sfUPropertyInstance& uprop)
{
    UMapProperty* tPtr = Cast<UMapProperty>(uprop.Property());
    auto keyIter = m_typeHandlers.find(tPtr->KeyProp->GetClass()->GetFName().GetComparisonIndex());
    if (keyIter == m_typeHandlers.end())
    {
        return nullptr;
    }
    auto valueIter = m_typeHandlers.find(tPtr->ValueProp->GetClass()->GetFName().GetComparisonIndex());
    if (valueIter == m_typeHandlers.end())
    {
        return nullptr;
    }
    sfListProperty::SPtr listPtr = sfListProperty::Create();
    FScriptMapHelper map(tPtr, uprop.Data());
    for (int i = 0; i < map.GetMaxIndex(); i++)
    {
        if (!map.IsValidIndex(i))
        {
            continue;
        }
        sfListProperty::SPtr pairPtr = sfListProperty::Create();
        sfProperty::SPtr keyPtr = keyIter->second.Get(sfUPropertyInstance(tPtr->KeyProp, (void*)map.GetKeyPtr(i)));
        if (keyPtr == nullptr)
        {
            return nullptr;
        }
        sfProperty::SPtr valuePtr = valueIter->second.Get(
            sfUPropertyInstance(tPtr->ValueProp, (void*)map.GetValuePtr(i)));
        if (valuePtr == nullptr)
        {
            return nullptr;
        }
        pairPtr->Add(keyPtr);
        pairPtr->Add(valuePtr);
        listPtr->Add(pairPtr);
    }
    return listPtr;
}

void sfPropertyUtil::SetMap(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr)
{
    UMapProperty* tPtr = Cast<UMapProperty>(uprop.Property());
    auto keyIter = m_typeHandlers.find(tPtr->KeyProp->GetClass()->GetFName().GetComparisonIndex());
    if (keyIter == m_typeHandlers.end())
    {
        return;
    }
    auto valueIter = m_typeHandlers.find(tPtr->ValueProp->GetClass()->GetFName().GetComparisonIndex());
    if (valueIter == m_typeHandlers.end())
    {
        return;
    }
    sfListProperty::SPtr listPtr = propPtr->AsList();
    FScriptMapHelper map(tPtr, uprop.Data());
    map.EmptyValues(listPtr->Size());
    for (int i = 0; i < listPtr->Size(); i++)
    {
        map.AddDefaultValue_Invalid_NeedsRehash();
        sfListProperty::SPtr pairPtr = listPtr->Get(i)->AsList();
        keyIter->second.Set(sfUPropertyInstance(tPtr->KeyProp, (void*)map.GetKeyPtr(i)), pairPtr->Get(0));
        valueIter->second.Set(sfUPropertyInstance(tPtr->ValueProp, (void*)map.GetValuePtr(i)), pairPtr->Get(1));
    }
    map.Rehash();
}

sfProperty::SPtr sfPropertyUtil::GetSet(const sfUPropertyInstance& uprop)
{
    USetProperty* tPtr = Cast<USetProperty>(uprop.Property());
    auto iter = m_typeHandlers.find(tPtr->ElementProp->GetClass()->GetFName().GetComparisonIndex());
    if (iter == m_typeHandlers.end())
    {
        return nullptr;
    }
    sfListProperty::SPtr listPtr = sfListProperty::Create();
    FScriptSetHelper set(tPtr, uprop.Data());
    for (int i = 0; i < set.GetMaxIndex(); i++)
    {
        if (!set.IsValidIndex(i))
        {
            continue;
        }
        sfProperty::SPtr elementPtr = iter->second.Get(
            sfUPropertyInstance(tPtr->ElementProp, (void*)set.GetElementPtr(i)));
        if (elementPtr == nullptr)
        {
            return nullptr;
        }
        listPtr->Add(elementPtr);
    }
    return listPtr;
}

void sfPropertyUtil::SetSet(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr)
{
    USetProperty* tPtr = Cast<USetProperty>(uprop.Property());
    auto iter = m_typeHandlers.find(tPtr->ElementProp->GetClass()->GetFName().GetComparisonIndex());
    if (iter == m_typeHandlers.end())
    {
        return;
    }
    sfListProperty::SPtr listPtr = propPtr->AsList();
    FScriptSetHelper set(tPtr, uprop.Data());
    set.EmptyElements(listPtr->Size());
    for (int i = 0; i < listPtr->Size(); i++)
    {
        set.AddDefaultValue_Invalid_NeedsRehash();
        iter->second.Set(sfUPropertyInstance(tPtr->ElementProp, (void*)set.GetElementPtr(i)), listPtr->Get(i));
    }
    set.Rehash();
}

sfProperty::SPtr sfPropertyUtil::GetStruct(const sfUPropertyInstance& uprop)
{
    UStructProperty* tPtr = Cast<UStructProperty>(uprop.Property());
    sfDictionaryProperty::SPtr dictPtr = sfDictionaryProperty::Create();
    UField* fieldPtr = tPtr->Struct->Children;
    while (fieldPtr)
    {
        UProperty* subPropPtr = Cast<UProperty>(fieldPtr);
        if (subPropPtr != nullptr)
        {
            auto iter = m_typeHandlers.find(subPropPtr->GetClass()->GetFName().GetComparisonIndex());
            if (iter != m_typeHandlers.end())
            {
                sfProperty::SPtr valuePtr = iter->second.Get(
                    sfUPropertyInstance(subPropPtr, subPropPtr->ContainerPtrToValuePtr<void>(uprop.Data())));
                if (valuePtr != nullptr)
                {
                    std::string name = TCHAR_TO_UTF8(*subPropPtr->GetName());
                    dictPtr->Set(name, valuePtr);
                }
            }
        }
        fieldPtr = fieldPtr->Next;
    }
    return dictPtr;
}

void sfPropertyUtil::SetStruct(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr)
{
    UStructProperty* tPtr = Cast<UStructProperty>(uprop.Property());
    sfDictionaryProperty::SPtr dictPtr = propPtr->AsDict();
    UField* fieldPtr = tPtr->Struct->Children;
    while (fieldPtr)
    {
        UProperty* subPropPtr = Cast<UProperty>(fieldPtr);
        if (subPropPtr != nullptr)
        {
            auto iter = m_typeHandlers.find(subPropPtr->GetClass()->GetFName().GetComparisonIndex());
            if (iter != m_typeHandlers.end())
            {
                std::string name = TCHAR_TO_UTF8(*subPropPtr->GetName());
                sfProperty::SPtr valuePtr;
                if (dictPtr->TryGet(name, valuePtr))
                {
                    iter->second.Set(sfUPropertyInstance(subPropPtr,
                        subPropPtr->ContainerPtrToValuePtr<void>(uprop.Data())), valuePtr);
                }
            }
        }
        fieldPtr = fieldPtr->Next;
    }
}

sfProperty::SPtr sfPropertyUtil::GetObject(const sfUPropertyInstance& uprop)
{
    UObjectProperty* tPtr = Cast<UObjectProperty>(uprop.Property());
    UObject* uobjPtr = tPtr->GetObjectPropertyValue(uprop.Data());
    if (uobjPtr == nullptr)
    {
        return sfValueProperty::Create((uint8_t)0);
    }
    if (uobjPtr->GetTypedOuter<ULevel>() != nullptr)
    {
        // Empty string means keep your current value
        return sfValueProperty::Create("");
    }
    FString path = uobjPtr->GetPathName();
    return FromString(path, SceneFusion::Service->Session());
}


void sfPropertyUtil::SetObject(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr)
{
    UObjectProperty* tPtr = Cast<UObjectProperty>(uprop.Property());
    sfValueProperty::SPtr valuePtr = propPtr->AsValue();
    if (valuePtr->GetValue().GetType() == ksMultiType::STRING)
    {
        FString path = ToString(valuePtr);
        // If path is empty we keep our current value
        if (!path.IsEmpty())
        {
            // Disable loading dialog that causes a crash if we are dragging objects
            GIsSlowTask = true;
            UObject* uobjPtr = LoadObject<UObject>(nullptr, *path);
            if (uobjPtr != nullptr)
            {
                tPtr->SetObjectPropertyValue(uprop.Data(), uobjPtr);
            }
            GIsSlowTask = false;
        }
        return;
    }
    tPtr->SetObjectPropertyValue(uprop.Data(), nullptr);
}

// Compares the src list values in lock step with the dest list values. When there is a discrepancy we first check for
// an element removal (Current src value = Next dest value). Next we check for an element insertion (Next src value =
// Current dest value). Finally if neither of the above cases were found, we replace the current dest value with the
// current src value.
void sfPropertyUtil::CopyList(sfListProperty::SPtr destPtr, sfListProperty::SPtr srcPtr)
{
    std::vector<sfProperty::SPtr> toAdd;
    for (int i = 0; i < srcPtr->Size(); i++)
    {
        sfProperty::SPtr elementPtr = srcPtr->Get(i);
        if (destPtr->Size() <= i)
        {
            toAdd.push_back(elementPtr);
            continue;
        }
        if (elementPtr->Equals(destPtr->Get(i)))
        {
            continue;
        }
        // if the current src element matches the next next element, remove the current dest element.
        if (destPtr->Size() > i + 1 && elementPtr->Equals(destPtr->Get(i + 1)))
        {
            destPtr->Remove(i);
            continue;
        }
        // if the current dest element matches the next src element, insert the current src element.
        if (srcPtr->Size() > i + 1 && destPtr->Get(i)->Equals(srcPtr->Get(i + 1)))
        {
            destPtr->Insert(i, elementPtr);
            i++;
            continue;
        }
        if (!Copy(destPtr->Get(i), elementPtr))
        {
            destPtr->Set(i, elementPtr);
        }
    }
    if (toAdd.size() > 0)
    {
        destPtr->AddRange(toAdd);
    }
    else if (destPtr->Size() > srcPtr->Size())
    {
        destPtr->Resize(srcPtr->Size());
    }
}

void sfPropertyUtil::CopyDict(sfDictionaryProperty::SPtr destPtr, sfDictionaryProperty::SPtr srcPtr)
{
    std::vector<sfName> toRemove;
    for (const auto& iter : *destPtr)
    {
        if (!srcPtr->HasKey(iter.first))
        {
            toRemove.push_back(iter.second->Key());
        }
    }
    for (const sfName key : toRemove)
    {
        destPtr->Remove(key);
    }
    for (const auto& iter : *srcPtr)
    {
        sfProperty::SPtr destPropPtr;
        if (!destPtr->TryGet(iter.first, destPropPtr) || !Copy(destPropPtr, iter.second))
        {
            destPtr->Set(iter.first, iter.second);
        }
    }
}

#undef LOG_CHANNEL