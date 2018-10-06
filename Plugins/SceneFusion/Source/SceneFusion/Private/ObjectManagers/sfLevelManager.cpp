#include "sfLevelManager.h"
#include "../Consts.h"
#include "../sfPropertyUtil.h"
#include "../SceneFusion.h"
#include "../sfUtils.h"

#include <Editor.h>
#include <Editor/UnrealEd/Classes/Editor/UnrealEdEngine.h>
#include <UnrealEdGlobals.h>
#include <LevelUtils.h>
#include <Runtime/Engine/Classes/Engine/LevelStreaming.h>
#include <FileHelpers.h>
#include <EditorLevelUtils.h>
#include <Classes/Settings/LevelEditorMiscSettings.h>
#include <EditorSupportDelegates.h>
#include <Engine/WorldComposition.h>
#include <EdMode.h>
#include <EditorModes.h>
#include <EditorModeManager.h>

#define LOG_CHANNEL "sfLevelManager"

sfLevelManager::sfLevelManager() :
    m_initialized { false }
{
    RegisterPropertyChangeHandlers();
}

sfLevelManager::~sfLevelManager()
{

}

void sfLevelManager::Initialize()
{
    if (m_initialized)
    {
        return;
    }

    m_sessionPtr = SceneFusion::Service->Session();
    m_worldPtr = GEditor->GetEditorWorldContext().World();

    // Register level event handlers
    m_onAddLevelToWorldHandle = FEditorDelegates::OnAddLevelToWorld.AddRaw(this, &sfLevelManager::OnAddLevelToWorld);
    m_onPrepareToCleanseEditorObjectHandle = FEditorSupportDelegates::PrepareToCleanseEditorObject.AddRaw(
        this,
        &sfLevelManager::OnPrepareToCleanseEditorObject);
    m_onObjectModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddRaw(this, &sfLevelManager::OnObjectModified);
    UTransBuffer* undoBufferPtr = Cast<UTransBuffer>(GEditor->Trans);
    if (undoBufferPtr != nullptr)
    {
        m_onUndoHandle = undoBufferPtr->OnUndo().AddRaw(this, &sfLevelManager::OnUndoRedo);
        m_onRedoHandle = undoBufferPtr->OnRedo().AddRaw(this, &sfLevelManager::OnUndoRedo);
    }

    m_destroyUnsyncedLevels = !SceneFusion::IsSessionCreator;

    // Upload levels
    if (SceneFusion::IsSessionCreator)
    {
        // Upload levels
        RequestLock();
        m_levelsToUpload.emplace(m_worldPtr->PersistentLevel); // Upload persistent level first
        for (FConstLevelIterator iter = m_worldPtr->GetLevelIterator(); iter; ++iter)
        {
            if (!(*iter)->IsPersistentLevel())
            {
                m_levelsToUpload.emplace(*iter);
            }
        }
    }

    m_initialized = true;
}

void sfLevelManager::CleanUp()
{
    // Unregister level event handlers
    FEditorDelegates::OnAddLevelToWorld.Remove(m_onAddLevelToWorldHandle);
    FEditorSupportDelegates::PrepareToCleanseEditorObject.Remove(m_onPrepareToCleanseEditorObjectHandle);
    FCoreUObjectDelegates::OnObjectModified.Remove(m_onObjectModifiedHandle);
    UTransBuffer* undoBufferPtr = Cast<UTransBuffer>(GEditor->Trans);
    if (undoBufferPtr != nullptr)
    {
        undoBufferPtr->OnUndo().Remove(m_onUndoHandle);
        undoBufferPtr->OnRedo().Remove(m_onRedoHandle);
    }

    m_lockObject = nullptr;
    m_levelsToUpload.clear();
    m_levelToObjectMap.Empty();
    m_objectToLevelMap.clear();
    m_movedLevels.clear();
    m_levelsNeedToBeLoaded.clear();

    m_initialized = false;
}

void sfLevelManager::Tick()
{
    // After joining a session, destroy levels that don't exist on the server
    if (m_destroyUnsyncedLevels && m_levelToObjectMap.Num() > 0)
    {
        m_destroyUnsyncedLevels = false;
        DestroyUnsyncedLevels();
    }

    // Send level transform change
    for (auto& levelPtr : m_movedLevels)
    {
        SendTransformUpdate(levelPtr);
    }
    m_movedLevels.clear();

    // Send level folder change
    for (ULevelStreaming* streamingLevelPtr : m_dirtyStreamingLevels)
    {
        SendFolderChange(streamingLevelPtr);
    }
    m_dirtyStreamingLevels.Empty();

    // Load levels that were removed but locked by other users
    for (sfObject::SPtr levelObjPtr : m_levelsNeedToBeLoaded)
    {
        OnCreate(levelObjPtr, 0);
    }
    m_levelsNeedToBeLoaded.clear();
}

sfObject::SPtr sfLevelManager::GetOrCreateLevelObject(ULevel* levelPtr)
{
    if (levelPtr == nullptr)
    {
        return nullptr;
    }

    // Try finding level sfObject in the map
    sfObject::SPtr levelObjectPtr = m_levelToObjectMap.FindRef(levelPtr);

    // Create sfObject for level if we could not find one
    if (levelObjectPtr != nullptr)
    {
        return levelObjectPtr;
    }
    else
    {
        RequestLock();
        m_levelsToUpload.emplace(levelPtr);
    }
    return nullptr;
}

ULevel* sfLevelManager::FindLevelByObject(sfObject::SPtr levelObjectPtr)
{
    if (levelObjectPtr->Type() != sfType::Level)
    {
        return nullptr;
    }

    auto iter = m_objectToLevelMap.find(levelObjectPtr);
    if (iter != m_objectToLevelMap.end())
    {
        return iter->second;
    }
    return nullptr;
}

void sfLevelManager::OnCreate(sfObject::SPtr objPtr, int childIndex)
{
    if (objPtr->Type() == sfType::LevelLock)
    {
        m_lockObject = objPtr;
        if (m_levelsToUpload.size() > 0)
        {
            m_lockObject->RequestLock();
        }
        return;
    }

    sfDictionaryProperty::SPtr propertiesPtr = objPtr->Property()->AsDict();
    FString levelPath = *sfPropertyUtil::ToString(propertiesPtr->Get(sfProp::Name));
    bool isPersistentLevel = propertiesPtr->Get(sfProp::IsPersistentLevel)->AsValue()->GetValue();

    // Temporarily remove event handlers
    FEditorSupportDelegates::PrepareToCleanseEditorObject.Remove(m_onPrepareToCleanseEditorObjectHandle);
    FEditorDelegates::OnAddLevelToWorld.Remove(m_onAddLevelToWorldHandle);
    FCoreUObjectDelegates::OnObjectModified.Remove(m_onObjectModifiedHandle);

    ULevel* levelPtr = FindLevelInLoadedLevels(levelPath, isPersistentLevel);
    if (levelPtr == nullptr && !levelPath.StartsWith("/Temp") && FPackageName::DoesPackageExist(levelPath))
    {
        levelPtr = TryLoadLevelFromFile(levelPath, isPersistentLevel);
    }
    if (levelPtr == nullptr)
    {
        KS::Log::Warning("Could not find level " + std::string(TCHAR_TO_UTF8(*levelPath)) +
            ". Please make sure that your project is up to date.");
        levelPtr = CreateMap(levelPath, isPersistentLevel);
    }

    if (levelPtr != nullptr)
    {
        m_levelToObjectMap.Add(levelPtr, objPtr);
        m_objectToLevelMap[objPtr] = levelPtr;
        m_levelsToUpload.erase(levelPtr);
    }
    else
    {
        KS::Log::Error("Failed to load or create level " + std::string(TCHAR_TO_UTF8(*levelPath)) +
            ". Disconnect.");
        SceneFusion::Service->LeaveSession();
        return;
    }

    sfProperty::SPtr propPtr;
    if (!isPersistentLevel) // If it is a streaming level, set transform and folder path on it.
    {
        ULevelStreaming* streamingLevelPtr = FLevelUtils::FindStreamingLevel(levelPtr);
        if (streamingLevelPtr != nullptr)
        {
            // Set level transform
            if (propertiesPtr->TryGet(sfProp::Location, propPtr))
            {
                FTransform transform = streamingLevelPtr->LevelTransform;
                transform.SetLocation(sfPropertyUtil::ToVector(propPtr));
                FRotator rotation = transform.Rotator();
                rotation.Yaw = propertiesPtr->Get(sfProp::Rotation)->AsValue()->GetValue();
                transform.SetRotation(rotation.Quaternion());
                sfUtils::PreserveUndoStack([streamingLevelPtr, transform]()
                {
                    FLevelUtils::SetEditorTransform(streamingLevelPtr, transform);
                });
                FDelegateHandle handle = levelPtr->OnApplyLevelTransform.AddLambda(
                    [this, levelPtr](const FTransform& transform) {
                    m_movedLevels.emplace(levelPtr);
                });
                m_onLevelTransformChangeHandles.Add(levelPtr, handle);
            }

            // Set folder path
            if (propertiesPtr->TryGet(sfProp::Folder, propPtr))
            {
                sfUtils::PreserveUndoStack([streamingLevelPtr, propPtr]()
                {
                    streamingLevelPtr->SetFolderPath(*sfPropertyUtil::ToString(propPtr));
                });
            }
        }
    }

    // Refresh levels window
    FEditorDelegates::RefreshLevelBrowser.Broadcast();

    // Add event handlers back
    m_onPrepareToCleanseEditorObjectHandle = FEditorSupportDelegates::PrepareToCleanseEditorObject.AddRaw(
        this,
        &sfLevelManager::OnPrepareToCleanseEditorObject);
    m_onAddLevelToWorldHandle = FEditorDelegates::OnAddLevelToWorld.AddRaw(this, &sfLevelManager::OnAddLevelToWorld);
    m_onObjectModifiedHandle
        = FCoreUObjectDelegates::OnObjectModified.AddRaw(this, &sfLevelManager::OnObjectModified);

    SceneFusion::ActorManager->OnSFLevelObjectCreate(objPtr, levelPtr);

    SceneFusion::RedrawActiveViewport();
}

void sfLevelManager::OnDelete(sfObject::SPtr objPtr)
{
    auto iter = m_objectToLevelMap.find(objPtr);
    if (iter == m_objectToLevelMap.end())
    {
        return;
    }
    ULevel* levelPtr = iter->second;
    m_objectToLevelMap.erase(iter);
    m_levelToObjectMap.Remove(levelPtr);
    m_onLevelTransformChangeHandles.Remove(levelPtr);

    // Temporarily remove PrepareToCleanseEditorObject event handler
    FEditorSupportDelegates::PrepareToCleanseEditorObject.Remove(m_onPrepareToCleanseEditorObjectHandle);

    SceneFusion::ActorManager->OnRemoveLevel(levelPtr); // Remove actors in this level from actor manager

    // When a level is unloaded, any actors you had selected will be unselected.
    // We need to record those actors that are not in the level to be unloaded and reselect them after.
    TArray<AActor*> selectedActors;
    for (auto iter = GEditor->GetSelectedActorIterator(); iter; ++iter)
    {
        AActor* actorPtr = Cast<AActor>(*iter);
        if (actorPtr && actorPtr->GetLevel() != levelPtr)
        {
            selectedActors.Add(actorPtr);
        }
    }

    FEdMode* activeMode = GLevelEditorModeTools().GetActiveMode(FBuiltinEditorModes::EM_StreamingLevel);
    ULevelStreaming* streamingLevelPtr = FLevelUtils::FindStreamingLevel(levelPtr);
    if (activeMode != nullptr && streamingLevelPtr != nullptr)
    {
        // Toggle streaming level viewport transform editing off
        GLevelEditorModeTools().DeactivateMode(FBuiltinEditorModes::EM_StreamingLevel);
    }
    UEditorLevelUtils::RemoveLevelFromWorld(levelPtr); // Remove/unload level from world

    // Reselect actors
    for (AActor* actorPtr : selectedActors)
    {
        GEditor->SelectActor(actorPtr, true, true, true);
    }

    // Add PrepareToCleanseEditorObject event handler back
    m_onPrepareToCleanseEditorObjectHandle = FEditorSupportDelegates::PrepareToCleanseEditorObject.AddRaw(
        this,
        &sfLevelManager::OnPrepareToCleanseEditorObject);

    // Refresh levels window
    FEditorDelegates::RefreshLevelBrowser.Broadcast();
}

void sfLevelManager::OnPropertyChange(sfProperty::SPtr propertyPtr)
{
    auto levelIter = m_objectToLevelMap.find(propertyPtr->GetContainerObject());
    if (levelIter == m_objectToLevelMap.end())
    {
        return;
    }
    ULevel* levelPtr = levelIter->second;

    if (propertyPtr->GetDepth() == 1)
    {
        auto handlerIter = m_propertyChangeHandlers.find(propertyPtr->Key());
        if (handlerIter != m_propertyChangeHandlers.end())
        {
            // Call property change handler
            ULevelStreaming* streamingLevelPtr = FLevelUtils::FindStreamingLevel(levelPtr);
            if (streamingLevelPtr != nullptr)
            {
                handlerIter->second(streamingLevelPtr, propertyPtr);
            }
            return;
        }
    }

    KS::Log::Warning("Could not find property " + propertyPtr->GetPath() + " on level" +
        std::string(TCHAR_TO_UTF8(*levelPtr->GetOutermost()->GetName())), LOG_CHANNEL);
}


ULevel* sfLevelManager::FindLevelInLoadedLevels(FString levelPath, bool isPersistentLevel)
{
    // Try to find level in loaded levels
    if (isPersistentLevel)
    {
        if (m_worldPtr->PersistentLevel->GetOutermost()->GetName() == levelPath)
        {
            return m_worldPtr->PersistentLevel;
        }
    }
    else
    {
        ULevelStreaming* streamingLevelPtr = FLevelUtils::FindStreamingLevel(m_worldPtr, *levelPath);
        if (streamingLevelPtr != nullptr)
        {
            return streamingLevelPtr->GetLoadedLevel();
        }
    }
    return nullptr;
}

ULevel* sfLevelManager::TryLoadLevelFromFile(FString levelPath, bool isPersistentLevel)
{
    // Load map if the level is the persistent level
    if (isPersistentLevel)
    {
        // Prompts the user to save the dirty levels before load map
        // Prompts the user to save the dirty levels before load map
        if (FEditorFileUtils::SaveDirtyPackages(true, true, false) &&
            FEditorFileUtils::LoadMap(levelPath, false, true))
        {
            // When a new map was loaded as the persistent level, all avatar actors were destroyed.
            // We need to recreate them.
            SceneFusion::AvatarManager->RecreateAllAvatars();
            m_worldPtr = GEditor->GetEditorWorldContext().World();
            return m_worldPtr->PersistentLevel;
        }
    }
    else // Add level to world if it is a streaming level
    {
        ULevelStreaming* streamingLevelPtr = UEditorLevelUtils::AddLevelToWorld(m_worldPtr,
            *levelPath,
            GetDefault<ULevelEditorMiscSettings>()->DefaultLevelStreamingClass);
        if (streamingLevelPtr)
        {
            return streamingLevelPtr->GetLoadedLevel();
        }
    }
    return nullptr;
}

ULevel* sfLevelManager::CreateMap(FString levelPath, bool isPersistentLevel)
{
    if (isPersistentLevel)
    {
        // Prompts the user to save the dirty levels before load map
        if (FEditorFileUtils::SaveDirtyPackages(true, true, false))
        {
            m_worldPtr = GUnrealEd->NewMap();
            if (!levelPath.StartsWith("/Temp/"))
            {
                FEditorFileUtils::SaveLevel(m_worldPtr->PersistentLevel, levelPath);
            }
            // When the new map was created as the persistent level, all avatar actors were destroyed.
            // We need to recreate them.
            SceneFusion::AvatarManager->RecreateAllAvatars();
            return m_worldPtr->PersistentLevel;
        }
    }
    else
    {
        ULevelStreaming* streamingLevelPtr = UEditorLevelUtils::CreateNewStreamingLevel(
            GetDefault<ULevelEditorMiscSettings>()->DefaultLevelStreamingClass, levelPath, false);
        if (streamingLevelPtr)
        {
            return streamingLevelPtr->GetLoadedLevel();
        }
    }
    return nullptr;
}

void sfLevelManager::UploadLevel(ULevel* levelPtr)
{
    // Ignore buffer level. The buffer level is a temporary level used when moving actors to a different level.
    if (levelPtr->GetOutermost() == GetTransientPackage())
    {
        return;
    }

    // Get level path
    FString levelPath = levelPtr->GetOutermost()->GetName();

    // Create level object
    sfDictionaryProperty::SPtr propertiesPtr = sfDictionaryProperty::Create();
    sfObject::SPtr levelObjectPtr = sfObject::Create(sfType::Level, propertiesPtr);

    propertiesPtr->Set(sfProp::Name, sfPropertyUtil::FromString(levelPath, m_sessionPtr));
    propertiesPtr->Set(sfProp::IsPersistentLevel, sfValueProperty::Create(levelPtr->IsPersistentLevel()));

    // Only streaming levels have transform and folders
    if (!levelPtr->IsPersistentLevel())
    {
        // Make sure persistent level is created before streaming levels
        GetOrCreateLevelObject(m_worldPtr->PersistentLevel);

        ULevelStreaming* streamingLevelPtr = FLevelUtils::FindStreamingLevel(levelPtr);
        if (streamingLevelPtr != nullptr)
        {
            // Set transform properties
            FTransform transform = streamingLevelPtr->LevelTransform;
            propertiesPtr->Set(sfProp::Location, sfPropertyUtil::FromVector(transform.GetLocation()));
            propertiesPtr->Set(sfProp::Rotation, sfValueProperty::Create(transform.Rotator().Yaw));
            FDelegateHandle handle = levelPtr->OnApplyLevelTransform.AddLambda(
                [this, levelPtr](const FTransform& transform) {
                m_movedLevels.emplace(levelPtr);
            });
            m_onLevelTransformChangeHandles.Add(levelPtr, handle);// Add transform change handler on level

            // Set folder property
            propertiesPtr->Set(sfProp::Folder,
                sfPropertyUtil::FromString(streamingLevelPtr->GetFolderPath().ToString(), m_sessionPtr));
        }
    }

    for (AActor* actorPtr : levelPtr->Actors)
    {
        if (SceneFusion::ActorManager->IsSyncable(actorPtr) && actorPtr->GetAttachParentActor() == nullptr)
        {
            sfObject::SPtr objPtr = SceneFusion::ActorManager->CreateObject(actorPtr);
            if (objPtr != nullptr)
            {
                levelObjectPtr->AddChild(objPtr);
            }
        }
    }

    // Add level to maps
    m_levelToObjectMap.Add(levelPtr, levelObjectPtr);
    m_objectToLevelMap[levelObjectPtr] = levelPtr;

    // Create
    m_sessionPtr->Create(levelObjectPtr);
}

void sfLevelManager::OnAddLevelToWorld(ULevel* newLevelPtr)
{
    RequestLock();
    m_levelsToUpload.emplace(newLevelPtr);
}

void sfLevelManager::OnPrepareToCleanseEditorObject(UObject* uobjPtr)
{
    // Disconnect if the world is going to be destroyed
    UWorld* worldPtr = Cast<UWorld>(uobjPtr);
    if (worldPtr == m_worldPtr)
    {
        KS::Log::Info("World destroyed. Disconnect from server.", LOG_CHANNEL);
        SceneFusion::Service->LeaveSession();
        return;
    }

    // If the object is a level, delete it on the server side
    ULevel* levelPtr = Cast<ULevel>(uobjPtr);
    if (levelPtr == nullptr)
    {
        return;
    }
    SceneFusion::ActorManager->OnRemoveLevel(levelPtr); // Delete objects for all actors in this level
    sfObject::SPtr levelObjPtr = m_levelToObjectMap.FindRef(levelPtr);
    if (levelObjPtr)
    {
        m_levelsToUpload.erase(levelPtr);
        m_levelToObjectMap.Remove(levelPtr);
        m_objectToLevelMap.erase(levelObjPtr);
        m_onLevelTransformChangeHandles.Remove(levelPtr);
        if (levelObjPtr->IsLocked())
        {
            m_levelsNeedToBeLoaded.emplace(levelObjPtr);
        }
        else
        {
            m_sessionPtr->Delete(levelObjPtr);
        }
    }
}

// Remove level from world if it does not exist on the server
void sfLevelManager::DestroyUnsyncedLevels()
{
    for (FConstLevelIterator iter = m_worldPtr->GetLevelIterator(); iter; ++iter)
    {
        if (!m_levelToObjectMap.Contains(*iter))
        {
            UEditorLevelUtils::RemoveLevelFromWorld(*iter);
        }
    }

    // Refresh levels window
    FEditorDelegates::RefreshLevelBrowser.Broadcast();
}

void sfLevelManager::SendTransformUpdate(ULevel* levelPtr)
{
    sfObject::SPtr objPtr = m_levelToObjectMap.FindRef(levelPtr);
    if (objPtr == nullptr)
    {
        return;
    }

    ULevelStreaming* streamingLevelPtr = FLevelUtils::FindStreamingLevel(levelPtr);
    if (streamingLevelPtr == nullptr)
    {
        return;
    }

    FTransform transform = streamingLevelPtr->LevelTransform;
    sfDictionaryProperty::SPtr propertiesPtr = objPtr->Property()->AsDict();
    sfProperty::SPtr oldPropPtr;

    if (objPtr->IsLocked())
    {
        // Revert level offset transform
        if (!propertiesPtr->TryGet(sfProp::Location, oldPropPtr) ||
            transform.GetLocation() != sfPropertyUtil::ToVector(oldPropPtr))
        {
            transform.SetLocation(sfPropertyUtil::ToVector(oldPropPtr));
            ModifyLevelWithoutTriggerEvent(levelPtr, [streamingLevelPtr, transform]()
            {
                FLevelUtils::SetEditorTransform(streamingLevelPtr, transform);
            });
        }
        if (!propertiesPtr->TryGet(sfProp::Rotation, oldPropPtr) ||
            transform.Rotator().Yaw != oldPropPtr->AsValue()->GetValue().GetFloat())
        {
            FRotator rotation = transform.Rotator();
            rotation.Yaw = oldPropPtr->AsValue()->GetValue();
            transform.SetRotation(rotation.Quaternion());
            ModifyLevelWithoutTriggerEvent(levelPtr, [streamingLevelPtr, transform]()
            {
                FLevelUtils::SetEditorTransform(streamingLevelPtr, transform);
            });
        }
    }
    else
    {
        if (!propertiesPtr->TryGet(sfProp::Location, oldPropPtr) ||
            transform.GetLocation() != sfPropertyUtil::ToVector(oldPropPtr))
        {
            propertiesPtr->Set(sfProp::Location, sfPropertyUtil::FromVector(transform.GetLocation()));
        }

        if (!propertiesPtr->TryGet(sfProp::Rotation, oldPropPtr) ||
            transform.Rotator().Yaw != oldPropPtr->AsValue()->GetValue().GetFloat())
        {
            propertiesPtr->Set(sfProp::Rotation, sfValueProperty::Create(transform.Rotator().Yaw));
        }

        // Moving a level changes transforms of all actors under the level.
        // We need to send transform changes for all actors under the level.
        for (AActor* actorPtr : levelPtr->Actors)
        {
            if (actorPtr != nullptr)
            {
                SceneFusion::ActorManager->SyncTransform(actorPtr);
            }
        }
    }
}

void sfLevelManager::RegisterPropertyChangeHandlers()
{
    m_propertyChangeHandlers[sfProp::Location] =
        [this](ULevelStreaming* streamingLevelPtr, sfProperty::SPtr propertyPtr)
    {
        FTransform transform = streamingLevelPtr->LevelTransform;
        transform.SetLocation(sfPropertyUtil::ToVector(propertyPtr));
        ModifyLevelWithoutTriggerEvent(streamingLevelPtr->GetLoadedLevel(), [streamingLevelPtr, transform]()
        {
            FLevelUtils::SetEditorTransform(streamingLevelPtr, transform);
        });
        SceneFusion::RedrawActiveViewport();
    };

    m_propertyChangeHandlers[sfProp::Rotation] =
        [this](ULevelStreaming* streamingLevelPtr, sfProperty::SPtr propertyPtr)
    {
        FTransform transform = streamingLevelPtr->LevelTransform;
        FRotator rotation = transform.Rotator();
        rotation.Yaw = propertyPtr->AsValue()->GetValue();
        transform.SetRotation(rotation.Quaternion());
        ModifyLevelWithoutTriggerEvent(streamingLevelPtr->GetLoadedLevel(), [streamingLevelPtr, transform]()
        {
            FLevelUtils::SetEditorTransform(streamingLevelPtr, transform);
        });
        SceneFusion::RedrawActiveViewport();
    };

    m_propertyChangeHandlers[sfProp::Folder] =
        [this](ULevelStreaming* streamingLevelPtr, sfProperty::SPtr propertyPtr)
    {
        ModifyLevelWithoutTriggerEvent(streamingLevelPtr->GetLoadedLevel(), [streamingLevelPtr, propertyPtr]()
        {
            streamingLevelPtr->SetFolderPath(*sfPropertyUtil::ToString(propertyPtr));
        });
        FEditorDelegates::RefreshLevelBrowser.Broadcast();
    };
}

void sfLevelManager::OnObjectModified(UObject* uobjPtr)
{
    ULevelStreaming* streamingLevelPtr = Cast<ULevelStreaming>(uobjPtr);
    if (streamingLevelPtr != nullptr)
    {
        m_dirtyStreamingLevels.Add(streamingLevelPtr);
    }
}

void sfLevelManager::SendFolderChange(ULevelStreaming* streamingLevelPtr)
{
    if (streamingLevelPtr == nullptr || streamingLevelPtr->GetLoadedLevel() == nullptr)
    {
        return;
    }
    ULevel* levelPtr = streamingLevelPtr->GetLoadedLevel();

    sfObject::SPtr objPtr = m_levelToObjectMap.FindRef(levelPtr);
    if (objPtr == nullptr)
    {
        return;
    }

    sfDictionaryProperty::SPtr propertiesPtr = objPtr->Property()->AsDict();
    sfProperty::SPtr oldPropPtr;
    FString folder = streamingLevelPtr->GetFolderPath().ToString();

    if (!propertiesPtr->TryGet(sfProp::Folder, oldPropPtr) ||
        folder != sfPropertyUtil::ToString(oldPropPtr))
    {
        if (objPtr->IsLocked())
        {
            // Revert level folder
            ModifyLevelWithoutTriggerEvent(levelPtr, [streamingLevelPtr, oldPropPtr]()
            {
                streamingLevelPtr->SetFolderPath(*sfPropertyUtil::ToString(oldPropPtr));
                FEditorDelegates::RefreshLevelBrowser.Broadcast();
            });
        }
        else
        {
            propertiesPtr->Set(sfProp::Folder, sfPropertyUtil::FromString(folder, m_sessionPtr));
        }
    }
}

void sfLevelManager::OnUndoRedo(FUndoSessionContext context, bool success)
{
    if (!success)
    {
        return;
    }
    FString contextString = context.Title.ToString();
    if (contextString.Contains("Folder", ESearchCase::Type::CaseSensitive) ||
        contextString == "Move World Hierarchy Items")
    {
        for (FConstLevelIterator iter = m_worldPtr->GetLevelIterator(); iter; ++iter)
        {
            ULevelStreaming* streamingLevelPtr = FLevelUtils::FindStreamingLevel(*iter);
            if (streamingLevelPtr != nullptr)
            {
                SendFolderChange(streamingLevelPtr);
            }
        }
    }
}

void sfLevelManager::ModifyLevelWithoutTriggerEvent(ULevel* levelPtr, Callback callback)
{
    // Temporarily remove event handlers
    m_onLevelTransformChangeHandles.Remove(levelPtr);
    FCoreUObjectDelegates::OnObjectModified.Remove(m_onObjectModifiedHandle);

    // Invoke callback function and prevents any changes to the undo stack during the call.
    sfUtils::PreserveUndoStack(callback);

    // Add event handlers back
    FDelegateHandle handle = levelPtr->OnApplyLevelTransform.AddLambda(
        [this, levelPtr](const FTransform& transform) {
        m_movedLevels.emplace(levelPtr);
    });
    m_onLevelTransformChangeHandles.Add(levelPtr, handle);
    m_onObjectModifiedHandle
        = FCoreUObjectDelegates::OnObjectModified.AddRaw(this, &sfLevelManager::OnObjectModified);
}

void sfLevelManager::OnDirectLockChange(sfObject::SPtr objPtr)
{
    if (objPtr->Type() == sfType::LevelLock && objPtr->LockOwner() == m_sessionPtr->LocalUser())
    {
        for (ULevel* levelPtr : m_levelsToUpload)
        {
            if (!m_levelToObjectMap.Contains(levelPtr))
            {
                UploadLevel(levelPtr);
            }
        }
        m_levelsToUpload.clear();

        m_lockObject->ReleaseLock();
    }
}

void sfLevelManager::RequestLock()
{
    if (m_lockObject == nullptr && SceneFusion::IsSessionCreator)
    {
        m_lockObject = sfObject::Create(sfType::LevelLock);
        m_sessionPtr->Create(m_lockObject);
    }

    if (m_lockObject != nullptr)
    {
        m_lockObject->RequestLock();
    }
}

#undef LOG_CHANNEL