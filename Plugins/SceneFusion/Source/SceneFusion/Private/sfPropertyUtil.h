#pragma once

#include "sfValueProperty.h"
#include "sfSession.h"
#include "sfUPropertyInstance.h"

#include <CoreMinimal.h>

using namespace KS;
using namespace KS::SceneFusion2;

/**
 * Utility for converting between SF properties and common Unreal types.
 */
class sfPropertyUtil
{
public:
    /**
     * Constructs a property from a vector.
     *
     * @param   const FVector& value
     * @return  sfValueProperty::SPtr
     */
    static sfValueProperty::SPtr FromVector(const FVector& value)
    {
        return ToProperty(value);
    }

    /**
     * Converts a property to a vector.
     *
     * @param   sfProperty::SPtr propertyPtr
     * @return  FVector
     */
    static FVector ToVector(sfProperty::SPtr propertyPtr)
    {
        return FromProperty<FVector>(propertyPtr);
    }

    /**
     * Constructs a property from a rotator.
     *
     * @param   const FRotator& value
     * @return  sfValueProperty::SPtr
     */
    static sfValueProperty::SPtr FromRotator(const FRotator& value)
    {
        return ToProperty(value);
    }

    /**
     * Converts a property to a rotator.
     *
     * @param   sfProperty::SPtr propertyPtr
     * @return  FRotator
     */
    static FRotator ToRotator(sfProperty::SPtr propertyPtr)
    {
        return FromProperty<FRotator>(propertyPtr);
    }

    /**
     * Constructs a property from a quat.
     *
     * @param   const FQuat& value
     * @return  sfValueProperty::SPtr
     */
    static sfValueProperty::SPtr FromQuat(const FQuat& value)
    {
        return ToProperty(value);
    }

    /**
     * Converts a property to a quat.
     *
     * @param   sfProperty::SPtr propertyPtr
     * @return  FQuat
     */
    static FQuat ToQuat(sfProperty::SPtr propertyPtr)
    {
        return FromProperty<FQuat>(propertyPtr);
    }

    /**
     * Constructs a property from a string, and registers the string in the string table.
     *
     * @param   const FString& value
     * @param   sfSession::SPtr sessionPtr
     * @return  sfValueProperty::SPtr
     */
    static sfValueProperty::SPtr FromString(const FString& value, sfSession::SPtr sessionPtr)
    {
        std::string str = TCHAR_TO_UTF8(*value);
        sessionPtr->AddToStringTable(str);
        return sfValueProperty::Create(str);
    }

    /**
     * Converts a property to a string.
     *
     * @param   sfProperty::SPtr propertyPtr
     * @return  FString
     */
    static FString ToString(sfProperty::SPtr propertyPtr)
    {
        sfValueProperty::SPtr valuePtr = propertyPtr->AsValue();
        if (valuePtr == nullptr)
        {
            return "";
        }
        std::string str = valuePtr->GetValue();
        return FString(UTF8_TO_TCHAR(str.c_str()));
    }

    /**
     * Finds a uproperty of a uobject corresponding to an sfproperty.
     *
     * @param   UObject* uobjPtr to find property on.
     * @param   sfProperty::SPtr propPtr to find corresponding uproperty for.
     * @return  sfUPropertyInstance
     */
    static sfUPropertyInstance FindUProperty(UObject* uobjPtr, sfProperty::SPtr propPtr);

    /**
     * Converts a UProperty to an sfProperty using reflection.
     *
     * @param   UObject* uobjPtr to get property from.
     * @param   UProperty* upropPtr
     * @return  sfProperty::SPtr
     */
    static sfProperty::SPtr GetValue(UObject* uobjPtr, UProperty* upropPtr);

    /**
     * Sets a UProperty using reflection to a value from an sfValueProperty.
     *
     * @param   const sfUPropertyInstance& uprop
     * @param   sfProperty::SPtr propPtr to get value from.
     */
    static void SetValue(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr);

    /**
     * Checks if an object has the default value for a property using reflection. Returns false if the property cannot
     * be synced by Scene Fusion.
     *
     * @param   UObject* uobjPtr to check property on.
     * @param   UProperty* upropPtr to check.
     * @return  bool false if the property does not have it's default value, or if the property type cannot be synced
     *          by Scene Fusion.
     */
    static bool IsDefaultValue(UObject* uobjPtr, UProperty* upropPtr);

    /**
     * Sets a property on an object to the default value using reflection. Does nothing if the property cannot be
     * synced by Scene Fusion.
     *
     * @param   UObject* objPtr to set property on.
     * @param   UProperty* upropPtr to set.
     */
    static void SetToDefaultValue(UObject* uobjPtr, UProperty* upropPtr);

    /**
     * Iterates all properties of an object using reflection and creates sfProperties for properties with non-default
     * values as fields in an sfDictionaryProperty.
     *
     * @param   UObject* uobjPtr to create properties for.
     * @param   sfDictionaryProperty::SPtr dictPtr to add properties to.
     */
    static void CreateProperties(UObject* uobjPtr, sfDictionaryProperty::SPtr dictPtr);

    /**
     * Applies property values from an sfDictionaryProperty to an object using reflection.
     *
     * @param   UObject* uobjPtr to apply property values to.
     * @param   sfDictionaryProperty::SPtr dictPtr to get property values from. If a value for a property is not in the
     *          dictionary, sets the property to its default value.
     */
    static void ApplyProperties(UObject* uobjPtr, sfDictionaryProperty::SPtr dictPtr);

    /**
     * Iterates all properties of an object using reflection and updates an sfDictionaryProperty when its values are
     * different from those on the object. Removes fields from the dictionary for properties that have their default
     * value.
     *
     * @param   UObject* uobjPtr to iterate properties on.
     * @param   sfDictionaryProperty::SPtr dictPtr to update.
     */
    static void SendPropertyChanges(UObject* uobjPtr, sfDictionaryProperty::SPtr dictPtr);

    /**
     * Copies the data from one property into another if they are the same property type.
     *
     * @param   sfProperty::SPtr destPtr to copy into.
     * @param   sfProperty::SPtr srcPtr to copy from.
     * @return  bool false if the properties were not the same type.
     */
    static bool Copy(sfProperty::SPtr destPtr, sfProperty::SPtr srcPtr);

private:
    /**
     * Holds getter and setter delegates for converting between a UProperty type and sfValueProperty.
     */
    struct TypeHandler
    {
    public:
        /**
         * Gets a UProperty value using reflection and converts it to an sfProperty.
         *
         * @param   const sfUPropertyInstance& to get value for.
         * @return  sfProperty::SPtr
         */
        typedef std::function<sfProperty::SPtr(const sfUPropertyInstance&)> Getter;

        /**
         * Sets a UProperty value using reflection to a value from an sfProperty.
         *
         * @param   const sfUPropertyInstance& to set value for.
         * @param   sfProperty::SPtr to get value from.
         */
        typedef std::function<void(const sfUPropertyInstance&, sfProperty::SPtr)> Setter;

        /**
         * Getter
         */
        Getter Get;

        /**
         * Setter
         */
        Setter Set;

        /**
         * Constructor
         *
         * @param   Getter getter
         * @param   Setter setter
         */
        TypeHandler(Getter getter, Setter setter) :
            Get{ getter },
            Set{ setter }
        {

        }
    };

    // TMaps seem buggy and I don't trust them. Dereferencing the pointer returned by TMap.find causes an access
    // violation, so we use std::unordered_map which works fine.
    // Keys are UProperty class name ids.
    static std::unordered_map<int, TypeHandler> m_typeHandlers;

    /**
     * Registers UProperty type handlers.
     */
    static void Initialize();

    /**
     * Creates a property type handler.
     *
     * @param   UClass* typePtr to create handler for.
     * @param   TypeHandler::Getter getter for getting properties of the given type.
     * @param   TypeHandler::Setter setter for setting properties of the given type.
     */
    static void CreateTypeHandler(UClass* typePtr, TypeHandler::Getter getter, TypeHandler::Setter setter);

    /**
     * Gets a double property value using reflection converted to an sfProperty.
     *
     * @param   const sfUPropertyInstance& uprop to get.
     * @return  sfProperty::SPtr
     */
    static sfProperty::SPtr GetDouble(const sfUPropertyInstance& uprop);

    /**
     * Sets a double property value using reflection.
     *
     * @param   const sfUPropertyInstance& uprop to set.
     * @param   sfProperty::SPtr propPtr to get value from.
     */
    static void SetDouble(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr);

    /**
     * Gets a string property value using reflection converted to an sfProperty.
     *
     * @param   const sfUPropertyInstance& uprop to get.
     * @return  sfProperty::SPtr
     */
    static sfProperty::SPtr GetFString(const sfUPropertyInstance& uprop);

    /**
     * Sets a string property value using reflection.
     *
     * @param   const sfUPropertyInstance& uprop to set.
     * @param   sfProperty::SPtr propPtr to get value from.
     */
    static void SetFString(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr);

    /**
     * Gets a text property value using reflection converted to an sfProperty.
     *
     * @param   const sfUPropertyInstance& uprop to get.
     * @return  sfProperty::SPtr
     */
    static sfProperty::SPtr GetFText(const sfUPropertyInstance& uprop);

    /**
     * Sets a text property value using reflection.
     *
     * @param   const sfUPropertyInstance& uprop to set.
     * @param   sfProperty::SPtr propPtr to get value from.
     */
    static void SetFText(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr);

    /**
     * Gets a name property value using reflection converted to an sfProperty.
     *
     * @param   const sfUPropertyInstance& uprop to get.
     * @return  sfProperty::SPtr
     */
    static sfProperty::SPtr GetFName(const sfUPropertyInstance& uprop);

    /**
     * Sets a name property value using reflection.
     *
     * @param   const sfUPropertyInstance& uprop to set.
     * @param   sfProperty::SPtr propPtr to get value from.
     */
    static void SetFName(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr);

    /**
     * Gets an enum property value from an object using reflection converted to an sfProperty.
     *
     * @param   const sfUPropertyInstance& uprop to get.
     * @return  sfProperty::SPtr
     */
    static sfProperty::SPtr GetEnum(const sfUPropertyInstance& uprop);

    /**
     * Sets an enum property value using reflection.
     *
     * @param   const sfUPropertyInstance& uprop to set.
     * @param   sfProperty::SPtr propPtr to get value from.
     */
    static void SetEnum(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr);

    /**
     * Gets an array property value from an object using reflection converted to an sfProperty.
     *
     * @param   const sfUPropertyInstance& uprop to get.
     * @return  sfProperty::SPtrr
     */
    static sfProperty::SPtr GetArray(const sfUPropertyInstance& uprop);

    /**
     * Sets an array property value using reflection.
     *
     * @param   const sfUPropertyInstance& uprop to set.
     * @param   sfProperty::SPtr propPtr to get value from.
     */
    static void SetArray(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr);

    /**
     * Gets a map property value from an object using reflection converted to an sfProperty.
     *
     * @param   const sfUPropertyInstance& uprop to get.
     * @return  sfProperty::SPtr
     */
    static sfProperty::SPtr GetMap(const sfUPropertyInstance& uprop);

    /**
     * Sets a map property value using reflection.
     *
     * @param   const sfUPropertyInstance& uprop to set.
     * @param   sfProperty::SPtr propPtr to get value from.
     */
    static void SetMap(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr);

    /**
     * Gets a set property value from an object using reflection converted to an sfProperty.
     *
     * @param   const sfUPropertyInstance& uprop to get.
     * @return  sfProperty::SPtr
     */
    static sfProperty::SPtr GetSet(const sfUPropertyInstance& uprop);

    /**
     * Sets a set property value using reflection.
     *
     * @param   const sfUPropertyInstance& uprop to set.
     * @param   sfProperty::SPtr propPtr to get value from.
     */
    static void SetSet(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr);

    /**
     * Gets a struct property value from an object using reflection converted to an sfProperty.
     *
     * @param   const sfUPropertyInstance& uprop to get.
     * @return  sfProperty::SPtr
     */
    static sfProperty::SPtr GetStruct(const sfUPropertyInstance& uprop);

    /**
     * Sets a struct property value using reflection.
     *
     * @param   const sfUPropertyInstance& uprop to set.
     * @param   sfProperty::SPtr propPtr to get value from.
     */
    static void SetStruct(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr);

    /**
     * Gets an object property value from an object using reflection converted to an sfProperty.
     *
     * @param   const sfUPropertyInstance& uprop to get.
     * @return  sfProperty::SPtr
     */
    static sfProperty::SPtr GetObject(const sfUPropertyInstance& uprop);

    /**
     * Sets an object property value using reflection.
     *
     * @param   const sfUPropertyInstance& uprop to set.
     * @param   sfProperty::SPtr propPtr to get value from.o.
     */
    static void SetObject(const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr);

    /**
     * Takes a pointer to a struct and sets it to point at a field of the struct using reflection. Returns false if the
     * uproperty is not a struct property.
     *
     * @param   const sfName& name of field to get.
     * @param   UProperty*& upropPtr - if not a struct property, returns false. Otherwise this is updated to point to
     *          the field property. If the field is not found, this is set to nullptr.
     * @param   void*& ptr to the struct data. Will be updated to point to the field data, if found.
     * @return  bool true if upropPtr is a struct property.
     */
    static bool GetStructField(const sfName& name, UProperty*& upropPtr, void*& ptr);

    /**
     * Takes a pointer to an array and sets it to point at an element of the array using reflection. Returns false if
     * the uproperty is not an array property.
     *
     * @param   index of element to get.
     * @param   UProperty*& upropPtr - if not an array property, returns false. Otherwise this is updated to point to
     *          the element property. If the element is not found, this is set to nullptr.
     * @param   void*& ptr to the array data. Will be updated to point to the element data, if found.
     * @return  bool true if upropPtr is an array property.
     */
    static bool GetArrayElement(int index, UProperty*& upropPtr, void*& ptr);

    /**
     * Takes a pointer to a map and sets it to point at a key or value of the map using reflection. Returns false if
     * the uproperty is not a map property.
     *
     * @param   index of element to get.
     * @param   UProperty*& upropPtr - if not a map property, returns false. Otherwise this is updated to point to the
     *          element property. If the element is not found, this is set to nullptr.
     * @param   void*& ptr to the map data. Will be updated to point to the element data, if found.
     * @param   TSharedPtr<FScriptMapHelper> outMapPtr - will point to the map if the element we are getting is a key.
     * @param   std::stack<sfProperty::SPtr>& propertyStack containing the sub properties to look for next. If the
     *          uproperty is a map and the index is within its bounds, the top property will be popped off the stack.
     *          If it's index is 0, we'll get the key, If 1, we'll get the value.
     * @return  bool true if upropPtr is a map property.
     */
    static bool GetMapElement(
        int index,
        UProperty*& upropPtr,
        void*& ptr,
        TSharedPtr<FScriptMapHelper>& outMapPtr,
        std::stack<sfProperty::SPtr>& propertyStack);

    /**
     * Takes a pointer to a set and sets it to point at an element of the set using reflection. Returns false if the
     * uproperty is not a set property.
     *
     * @param   index of element to get.
     * @param   UProperty*& upropPtr - if not a set property, returns false. Otherwise this is updated to point to the
     *          element property. If the element is not found, this is set to nullptr.
     * @param   void*& ptr to the set data. Will be updated to point to the element data, if found.
     * @param   TSharedPtr<FScriptMapHelper> outSetPtr - will point to the set.
     * @return  bool true if upropPtr is a map property.
     */
    static bool GetSetElement(
        int index,
        UProperty*& upropPtr,
        void*& ptr,
        TSharedPtr<FScriptSetHelper>& outSetPtr);

    /**
     * Adds, removes, and/or sets elements in a destination list to make it the same as a source list.
     *
     * @param   sfListProperty::SPtr destPtr to modify.
     * @param   sfListProperty::SPtr srcPtr to make destPtr a copy of.
     */
    static void CopyList(sfListProperty::SPtr destPtr, sfListProperty::SPtr srcPtr);

    /**
     * Adds, removes, and/or sets fields in a destination dictionary so to make it the same as a source dictionary.
     *
     * @param   sfDictionaryProperty::SPtr destPtr to modify.
     * @param   sfDictionaryProperty::SPtr srcPtr to make destPtr a copy of.
     */
    static void CopyDict(sfDictionaryProperty::SPtr destPtr, sfDictionaryProperty::SPtr srcPtr);

    /**
     * Constructs a property from a T.
     *
     * @param   const T& value
     * @return  sfValueProperty::SPtr
     */
    template<typename T>
    static sfValueProperty::SPtr ToProperty(const T& value)
    {
        const uint8_t* temp = reinterpret_cast<const uint8_t*>(&value);
        ksMultiType multiType(ksMultiType::BYTE_ARRAY, temp, sizeof(T), sizeof(T));
        return sfValueProperty::Create(std::move(multiType));
    }

    /**
     * Converts a property to T.
     *
     * @param   sfProperty::SPtr propertyPtr
     * @return  T
     */
    template<typename T>
    static T FromProperty(sfProperty::SPtr propertyPtr)
    {
        if (propertyPtr == nullptr || propertyPtr->Type() != sfProperty::VALUE)
        {
            return T();
        }
        sfValueProperty::SPtr valuePtr = propertyPtr->AsValue();
        return *(reinterpret_cast<const T*>(valuePtr->GetValue().GetData().data()));
    }

    /**
     * Creates a property handler for type T.
     */
    template<typename T>
    static void CreateTypeHandler()
    {
        CreateTypeHandler(T::StaticClass(),
            [](const sfUPropertyInstance& uprop)
            {
                T* tPtr = Cast<T>(uprop.Property());
                return sfValueProperty::Create(tPtr->GetPropertyValue(uprop.Data()));
            },
                [](const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr)
            {
                T* tPtr = Cast<T>(uprop.Property());
                tPtr->SetPropertyValue(uprop.Data(), propPtr->AsValue()->GetValue());
            }
        );
    }

    /**
     * Creates a property handler for type T that casts the value to U, where U is a type supported by ksMultiType.
     */
    template<typename T, typename U>
    static void CreateTypeHandler()
    {
        CreateTypeHandler(T::StaticClass(),
            [](const sfUPropertyInstance& uprop)
            {
                T* tPtr = Cast<T>(uprop.Property());
                return sfValueProperty::Create((U)tPtr->GetPropertyValue(uprop.Data()));
            },
                [](const sfUPropertyInstance& uprop, sfProperty::SPtr propPtr)
            {
                T* tPtr = Cast<T>(uprop.Property());
                tPtr->SetPropertyValue(uprop.Data(), (U)propPtr->AsValue()->GetValue());
            }
        );
    }
};