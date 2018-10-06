#include "sfActorManager.h"
#include "../Components/sfLockComponent.h"
#include "../sfPropertyUtil.h"
#include "../sfActorUtil.h"
#include "../SceneFusion.h"
#include "../Consts.h"
#include "../sfUtils.h"

#include <Editor.h>
#include <EngineUtils.h>
#include <ActorEditorUtils.h>
#include <Engine/StaticMeshActor.h>
#include <Classes/Engine/Selection.h>
#include <Materials/MaterialInstanceDynamic.h>
#include <Runtime/Engine/Classes/Particles/Emitter.h>
#include <Runtime/Engine/Classes/Particles/ParticleSystemComponent.h>
#include <Editor/UnrealEd/Public/EditorActorFolders.h>
#include <Runtime/Engine/Classes/Animation/SkeletalMeshActor.h>
#include <Runtime/Engine/Classes/Components/SkeletalMeshComponent.h>
#include <Runtime/CoreUObject/Public/UObject/ObjectMacros.h>
#include <Editor/LevelEditor/Public/LevelEditor.h>
#include <Editor/UnrealEd/Public/LevelEditorViewport.h>

// Change this to 1 to enable experimental partial syncing of actor properties
#define SYNC_ACTOR_PROPERTIES 0

// In seconds
#define BSP_REBUILD_DELAY 2.0f;
#define LOG_CHANNEL "sfObjectManager"

sfActorManager::sfActorManager(TSharedPtr<sfLevelManager> levelManagerPtr) :
    m_levelManagerPtr { levelManagerPtr }
{
    RegisterPropertyChangeHandlers();
    RegisterUndoTypes();
    m_lockMaterialPtr = LoadObject<UMaterialInterface>(nullptr, TEXT("/SceneFusion/LockMaterial"));
}

sfActorManager::~sfActorManager()
{
    
}

void sfActorManager::Initialize()
{
    m_sessionPtr = SceneFusion::Service->Session();
    m_onActorAddedHandle = GEngine->OnLevelActorAdded().AddRaw(this, &sfActorManager::OnActorAdded);
    m_onActorDeletedHandle = GEngine->OnLevelActorDeleted().AddRaw(this, &sfActorManager::OnActorDeleted);
    m_onActorAttachedHandle = GEngine->OnLevelActorAttached().AddRaw(this, &sfActorManager::OnAttachDetach);
    m_onActorDetachedHandle = GEngine->OnLevelActorDetached().AddRaw(this, &sfActorManager::OnAttachDetach);
    m_onFolderChangeHandle = GEngine->OnLevelActorFolderChanged().AddRaw(this, &sfActorManager::OnFolderChange);
    m_onMoveStartHandle = GEditor->OnBeginObjectMovement().AddRaw(this, &sfActorManager::OnMoveStart);
    m_onMoveEndHandle = GEditor->OnEndObjectMovement().AddRaw(this, &sfActorManager::OnMoveEnd);
    m_onPropertyChangeHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(this,
        &sfActorManager::OnUPropertyChange);
    m_onUserColorChangeEventPtr = m_sessionPtr->RegisterOnUserColorChangeHandler([this](sfUser::SPtr userPtr)
    {
        OnUserColorChange(userPtr);
    });
    m_onUserLeaveEventPtr = m_sessionPtr->RegisterOnUserLeaveHandler([this](sfUser::SPtr userPtr)
    {
        OnUserLeave(userPtr);
    });
    m_undoBufferPtr = Cast<UTransBuffer>(GEditor->Trans);
    if (m_undoBufferPtr != nullptr)
    {
        m_onUndoHandle = m_undoBufferPtr->OnUndo().AddRaw(this, &sfActorManager::OnUndo);
        m_onRedoHandle = m_undoBufferPtr->OnRedo().AddRaw(this, &sfActorManager::OnRedo);
        m_beforeUndoRedoHandle = m_undoBufferPtr->OnBeforeRedoUndo().AddRaw(this, &sfActorManager::BeforeUndoRedo);
    }

    m_movingActors = false;
    m_bspRebuildDelay = -1.0f;
}

void sfActorManager::CleanUp()
{
    GEngine->OnLevelActorAdded().Remove(m_onActorAddedHandle);
    GEngine->OnLevelActorDeleted().Remove(m_onActorDeletedHandle);
    GEngine->OnLevelActorAttached().Remove(m_onActorAttachedHandle);
    GEngine->OnLevelActorDetached().Remove(m_onActorDetachedHandle);
    GEngine->OnLevelActorFolderChanged().Remove(m_onFolderChangeHandle);
    GEditor->OnBeginObjectMovement().Remove(m_onMoveStartHandle);
    GEditor->OnEndObjectMovement().Remove(m_onMoveEndHandle);
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(m_onPropertyChangeHandle);
    m_onUserColorChangeEventPtr.reset();
    m_onUserLeaveEventPtr.reset();
    if (m_undoBufferPtr != nullptr)
    {
        m_undoBufferPtr->OnUndo().Remove(m_onUndoHandle);
        m_undoBufferPtr->OnRedo().Remove(m_onRedoHandle);
        m_undoBufferPtr->OnBeforeRedoUndo().Remove(m_beforeUndoRedoHandle);
    }

    UWorld* world = GEditor->GetEditorWorldContext().World();
    for (TActorIterator<AActor> iter(world); iter; ++iter)
    {
        sfObject::SPtr objPtr = m_actorToObjectMap.FindRef(*iter);
        if (objPtr != nullptr && objPtr->IsLocked())
        {
            Unlock(*iter);
        }
    }

    for (auto iter : m_lockMaterials)
    {
        iter.Value->ClearFlags(EObjectFlags::RF_Standalone);// Allow unreal to destroy the material instances
    }

    RehashProperties();

    m_actorToObjectMap.Empty();
    m_objectToActorMap.clear();
    m_lockMaterials.Empty();
    m_uploadList.Empty();
    m_propertyChangeMap.Empty();
    m_recreateQueue.Empty();
    m_syncLabelQueue.Empty();
    m_revertFolderQueue.Empty();
    m_syncParentList.Empty();
    m_foldersToCheck.Empty();
    m_selectedActors.clear();
}

void sfActorManager::Tick(float deltaTime)
{
    // Create server objects for actors in the upload list
    if (m_uploadList.Num() > 0)
    {
        UploadActors(m_uploadList);
        m_uploadList.Empty();
    }

    // Check for selection changes and request locks/unlocks
    UpdateSelection();

    // Rehash maps and sets that were changed by other users
    RehashProperties();

    // Send property changes to the server
    SendPropertyChanges();

    // Send label/name changes for renamed actors or reset them to server values if they are locked
    while (!m_syncLabelQueue.IsEmpty())
    {
        AActor* actorPtr;
        m_syncLabelQueue.Dequeue(actorPtr);
        sfObject::SPtr objPtr = m_actorToObjectMap.FindRef(actorPtr);
        if (objPtr == nullptr)
        {
            continue;
        }
        sfDictionaryProperty::SPtr propertiesPtr = objPtr->Property()->AsDict();
        SyncLabelAndName(actorPtr, objPtr, propertiesPtr);
    }

    // Revert folders to server values for actors whose folder changed while locked
    if (!m_revertFolderQueue.IsEmpty())
    {
        sfUtils::PreserveUndoStack([this]()
        {
            RevertLockedFolders();
        });
    }

    // Recreate actors that were deleted while locked.
    RecreateLockedActors();

    // Send parent changes for attached/detached actors or reset them to server values if they are locked
    for (AActor* actorPtr : m_syncParentList)
    {
        sfObject::SPtr objPtr = m_actorToObjectMap.FindRef(actorPtr);
        if (objPtr != nullptr)
        {
            SyncParent(actorPtr, objPtr);
        }
    }
    m_syncParentList.Empty();

    // Empty folders are gone when you reload a level, so we delete folders that become empty
    if (m_foldersToCheck.Num() > 0)
    {
        sfUtils::PreserveUndoStack([this]()
        {
            DeleteEmptyFolders();
        });
    }

    // Rebuild BSP
    RebuildBSPIfNeeded(deltaTime);
}

void sfActorManager::UpdateSelection()
{
    // Unreal doesn't have deselect events and doesn't fire select events when selecting through the World Outliner so
    // we have to iterate the selection to check for changes
    for (auto iter = m_selectedActors.cbegin(); iter != m_selectedActors.cend();)
    {
        if (m_movingActors)
        {
            SendTransformUpdate(iter->first, iter->second);
        }
        if (!iter->first->IsSelected())
        {
            iter->second->ReleaseLock();
            m_selectedActors.erase(iter++);
        }
        else
        {
            ++iter;
        }
    }
    for (auto iter = GEditor->GetSelectedActorIterator(); iter; ++iter)
    {
        AActor* actorPtr = Cast<AActor>(*iter);
        if (actorPtr == nullptr || m_selectedActors.find(actorPtr) != m_selectedActors.end())
        {
            continue;
        }
        sfObject::SPtr objPtr = m_actorToObjectMap.FindRef(actorPtr);
        if (objPtr != nullptr)
        {
            objPtr->RequestLock();
            m_selectedActors[actorPtr] = objPtr;
        }
    }
}

void sfActorManager::DestroyUnsyncedActorsInLevel(ULevel* levelPtr)
{
    UWorld* worldPtr = levelPtr->GetWorld();
    for (AActor* actorPtr : levelPtr->Actors)
    {
        if (IsSyncable(actorPtr) && !m_actorToObjectMap.Contains(actorPtr))
        {
            if (actorPtr->IsA<ABrush>())
            {
                m_bspRebuildDelay = BSP_REBUILD_DELAY;
            }
            worldPtr->EditorDestroyActor(actorPtr, true);
            SceneFusion::RedrawActiveViewport();
        }
    }
}

void sfActorManager::RevertLockedFolders()
{
    while (!m_revertFolderQueue.IsEmpty())
    {
        AActor* actorPtr;
        m_revertFolderQueue.Dequeue(actorPtr);
        sfObject::SPtr objPtr = m_actorToObjectMap.FindRef(actorPtr);
        if (objPtr != nullptr)
        {
            sfDictionaryProperty::SPtr propertiesPtr = objPtr->Property()->AsDict();
            GEngine->OnLevelActorFolderChanged().Remove(m_onFolderChangeHandle);
            actorPtr->SetFolderPath(FName(*sfPropertyUtil::ToString(propertiesPtr->Get(sfProp::Folder))));
            m_onFolderChangeHandle = GEngine->OnLevelActorFolderChanged().AddRaw(
                this, &sfActorManager::OnFolderChange);
        }
    }
}

void sfActorManager::RecreateLockedActors()
{
    while (!m_recreateQueue.IsEmpty())
    {
        sfObject::SPtr objPtr;
        m_recreateQueue.Dequeue(objPtr);
        if (m_objectToActorMap.find(objPtr) == m_objectToActorMap.end())
        {
            OnCreate(objPtr, 0);
        }
    }
}

void sfActorManager::DeleteEmptyFolders()
{
    // The only way to tell if a folder is empty is to iterate all the actors
    if (m_foldersToCheck.Num() > 0 && FActorFolders::IsAvailable())
    {
        UWorld* world = GEditor->GetEditorWorldContext().World();
        for (TActorIterator<AActor> iter(world); iter && m_foldersToCheck.Num() > 0; ++iter)
        {
            FString folder = iter->GetFolderPath().ToString();
            for (int i = m_foldersToCheck.Num() - 1; i >= 0; i--)
            {
                if (folder == m_foldersToCheck[i] || FActorFolders::Get().PathIsChildOf(folder, m_foldersToCheck[i]))
                {
                    m_foldersToCheck.RemoveAt(i);
                    break;
                }
            }
        }
        for (int i = 0; i < m_foldersToCheck.Num(); i++)
        {
            FActorFolders::Get().DeleteFolder(*world, FName(*m_foldersToCheck[i]));
        }
        m_foldersToCheck.Empty();
    }
}
void sfActorManager::RebuildBSPIfNeeded(float deltaTime)
{
    if (m_bspRebuildDelay >= 0.0f)
    {
        m_bspRebuildDelay -= deltaTime;
        if (m_bspRebuildDelay < 0.0f)
        {
            SceneFusion::RedrawActiveViewport();
            GEditor->RebuildAlteredBSP();
        }
    }
}

bool sfActorManager::IsSyncable(AActor* actorPtr)
{
    return actorPtr != nullptr &&
        !actorPtr->bHiddenEdLayer && actorPtr->IsEditable() && actorPtr->IsListedInSceneOutliner() &&
        !actorPtr->IsPendingKill() && (actorPtr->GetFlags() & EObjectFlags::RF_Transient) == 0 &&
        !FActorEditorUtils::IsABuilderBrush(actorPtr) &&
        !actorPtr->IsA(AWorldSettings::StaticClass());
}

void sfActorManager::OnActorAdded(AActor* actorPtr)
{
    // Ignore actors in the buffer level.
    // The buffer level is a temporary level used when moving actors to a different level.
    if (actorPtr->GetOutermost() == GetTransientPackage())
    {
        return;
    }

    // We add this to a list for processing later because the actor's properties may not be initialized yet.
    m_uploadList.Add(actorPtr);
}

void sfActorManager::UploadActors(const TArray<AActor*>& actors)
{
    std::list<sfObject::SPtr> objects;
    sfObject::SPtr parentPtr = nullptr;
    sfObject::SPtr currentParentPtr = nullptr;
    for (AActor* actorPtr : actors)
    {
        if (!IsSyncable(actorPtr))
        {
            continue;
        }

        AActor* parentActorPtr = actorPtr->GetAttachParentActor();
        if (parentActorPtr == nullptr)
        {
            currentParentPtr = m_levelManagerPtr->GetOrCreateLevelObject(actorPtr->GetLevel());
        }
        else
        {
            currentParentPtr = m_actorToObjectMap.FindRef(parentActorPtr);
        }

        if (currentParentPtr == nullptr)
        {
            continue;
        }
        else if (currentParentPtr->IsFullyLocked())
        {
            KS::Log::Warning("Failed to attach " + std::string(TCHAR_TO_UTF8(*actorPtr->GetName())) +
                " to " + std::string(TCHAR_TO_UTF8(*parentActorPtr->GetName())) +
                " because it is fully locked by another user.",
                LOG_CHANNEL);
            GEngine->OnLevelActorDetached().Remove(m_onActorDetachedHandle);
            actorPtr->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
            m_onActorDetachedHandle = GEngine->OnLevelActorDetached().AddRaw(this, &sfActorManager::OnAttachDetach);
            currentParentPtr = m_levelManagerPtr->GetOrCreateLevelObject(actorPtr->GetLevel());
        }

        if (parentPtr == nullptr)
        {
            parentPtr = currentParentPtr;
        }

        // All objects in one request must have the same parent, so if we encounter a different parent, send a request
        // for all objects we already processed and clear the objects list to start a new request.
        if (currentParentPtr != parentPtr)
        {
            if (objects.size() > 0)
            {
                m_sessionPtr->Create(objects, parentPtr, 0);
                // Pre-existing child objects can only be attached after calling Create.
                FindAndAttachChildren(objects);
                objects.clear();
            }
            parentPtr = currentParentPtr;
        }
        sfObject::SPtr objPtr = CreateObject(actorPtr);
        if (objPtr != nullptr)
        {
            objects.push_back(objPtr);
        }
    }
    if (objects.size() > 0)
    {
        m_sessionPtr->Create(objects, parentPtr, 0);
        // Pre-existing child objects can only be attached after calling Create.
        FindAndAttachChildren(objects);
    }
}

void sfActorManager::FindAndAttachChildren(const std::list<sfObject::SPtr>& objects)
{
    for (sfObject::SPtr objPtr : objects)
    {
        auto iter = objPtr->SelfAndDescendants();
        while (iter.Value() != nullptr)
        {
            sfObject::SPtr currentPtr = iter.Value();
            iter.Next();
            auto actorIter = m_objectToActorMap.find(currentPtr);
            if (actorIter != m_objectToActorMap.end())
            {
                TArray<AActor*> children;
                actorIter->second->GetAttachedActors(children);
                for (AActor* childPtr : children)
                {
                    sfObject::SPtr childObjPtr = m_actorToObjectMap.FindRef(childPtr);
                    if (childObjPtr != nullptr && childObjPtr->Parent() != currentPtr)
                    {
                        currentPtr->AddChild(childObjPtr);
                        SendTransformUpdate(childPtr, childObjPtr);
                    }
                }
            }
        }
    }
}

sfObject::SPtr sfActorManager::CreateObject(AActor* actorPtr)
{
    sfObject::SPtr objPtr = m_actorToObjectMap.FindRef(actorPtr);
    if (objPtr != nullptr)
    {
        return nullptr;
    }
    sfDictionaryProperty::SPtr propertiesPtr = sfDictionaryProperty::Create();
    objPtr = sfObject::Create(sfType::Actor, propertiesPtr);

    if (actorPtr->IsSelected())
    {
        objPtr->RequestLock();
        m_selectedActors[actorPtr] = objPtr;
    }

    propertiesPtr->Set(sfProp::Name, sfPropertyUtil::FromString(actorPtr->GetName(), m_sessionPtr));
    if (actorPtr->GetClass()->IsInBlueprint())
    {
        // Set path to blueprint
        propertiesPtr->Set(sfProp::Class,
            sfPropertyUtil::FromString(actorPtr->GetClass()->GetOuter()->GetName(), m_sessionPtr));
    }
    else
    {
        propertiesPtr->Set(sfProp::Class,
            sfPropertyUtil::FromString(actorPtr->GetClass()->GetName(), m_sessionPtr));
    }
    propertiesPtr->Set(sfProp::Label, sfPropertyUtil::FromString(actorPtr->GetActorLabel(), m_sessionPtr));
    propertiesPtr->Set(sfProp::Folder,
    sfPropertyUtil::FromString(actorPtr->GetFolderPath().ToString(), m_sessionPtr));
    USceneComponent* rootComponentPtr = actorPtr->GetRootComponent();
    if (rootComponentPtr != nullptr)
    {
        propertiesPtr->Set(sfProp::Location, sfPropertyUtil::FromVector(rootComponentPtr->RelativeLocation));
        propertiesPtr->Set(sfProp::Rotation, sfPropertyUtil::FromRotator(rootComponentPtr->RelativeRotation));
        propertiesPtr->Set(sfProp::Scale, sfPropertyUtil::FromVector(actorPtr->GetActorRelativeScale3D()));
    }

    CreateStaticMeshProperties(actorPtr, propertiesPtr) ||
    CreateSkeletalMeshProperties(actorPtr, propertiesPtr) ||
    CreateEmitterProperties(actorPtr, propertiesPtr);

#if SYNC_ACTOR_PROPERTIES
    sfPropertyUtil::CreateProperties(actorPtr, propertiesPtr);
#endif

    TArray<AActor*> children;
    actorPtr->GetAttachedActors(children);
    for (AActor* childPtr : children)
    {
        sfObject::SPtr childObjPtr = CreateObject(childPtr);
        if (childObjPtr != nullptr)
        {
            objPtr->AddChild(childObjPtr);
        }
    }

    m_actorToObjectMap.Add(actorPtr, objPtr);
    m_objectToActorMap[objPtr] = actorPtr;

    InvokeOnLockStateChange(objPtr, actorPtr);

    return objPtr;
}

void sfActorManager::OnCreate(sfObject::SPtr objPtr, int childIndex)
{
    sfObject::SPtr levelObjectPtr = objPtr->Parent();
    if (levelObjectPtr == nullptr)
    {
        LogNoParentErrorAndDisconnect(objPtr);
        return;
    }

    while (levelObjectPtr->Parent() != nullptr)
    {
        levelObjectPtr = levelObjectPtr->Parent();
    }

    ULevel* levelPtr = m_levelManagerPtr->FindLevelByObject(levelObjectPtr);
    if (!levelPtr)
    {
        sfDictionaryProperty::SPtr propertiesPtr = levelObjectPtr->Property()->AsDict();
        KS::Log::Warning("Could not find level " + propertiesPtr->Get(sfProp::Name)->ToString(), LOG_CHANNEL);
        levelPtr = GEditor->GetEditorWorldContext().World()->PersistentLevel;
    }
    AActor* actorPtr = InitializeActor(objPtr, levelPtr);
    if (actorPtr == nullptr)
    {
        return;
    }

    if (DetachIfParentIsLevel(objPtr, actorPtr))
    {
        return;
    }

    auto iter = m_objectToActorMap.find(objPtr->Parent());
    if (iter != m_objectToActorMap.end())
    {
        GEngine->OnLevelActorAdded().Remove(m_onActorAttachedHandle);
        actorPtr->AttachToActor(iter->second, FAttachmentTransformRules::KeepRelativeTransform);
        m_onActorAttachedHandle = GEngine->OnLevelActorAttached().AddRaw(this, &sfActorManager::OnAttachDetach);
    }
}

AActor* sfActorManager::InitializeActor(sfObject::SPtr objPtr, ULevel* levelPtr)
{
    sfDictionaryProperty::SPtr propertiesPtr = objPtr->Property()->AsDict();
    FString name = sfPropertyUtil::ToString(propertiesPtr->Get(sfProp::Name));
    AActor* actorPtr = sfActorUtil::FindActorWithNameInLevel(levelPtr, name);
    if (actorPtr != nullptr)
    {
        if (actorPtr->IsPendingKill())
        {
            // Rename the deleted actor so we can reuse its name.
            sfActorUtil::Rename(actorPtr, name + " (deleted)");
            actorPtr = nullptr;
        }
        else if (m_actorToObjectMap.Contains(actorPtr))
        {
            actorPtr = nullptr;
        }
    }

    FVector location{ 0, 0, 0 };
    FRotator rotation{ 0, 0, 0 };
    FVector scale{ 1, 1, 1 };
    sfProperty::SPtr propPtr;
    if (propertiesPtr->TryGet(sfProp::Location, propPtr))
    {
        location = sfPropertyUtil::ToVector(propPtr);
        rotation = sfPropertyUtil::ToRotator(propertiesPtr->Get(sfProp::Rotation));
        scale = sfPropertyUtil::ToVector(propertiesPtr->Get(sfProp::Scale));
    }

    if (actorPtr == nullptr)
    {
        FString className = sfPropertyUtil::ToString(propertiesPtr->Get(sfProp::Class));
        UClass* classPtr = nullptr;
        if (className.Contains("/"))
        {
            // If it contains a '/' it's a blueprint path
            // Disable loading dialog that causes a crash if we are dragging objects
            GIsSlowTask = true;
            UBlueprint* blueprintPtr = LoadObject<UBlueprint>(nullptr, *className);
            GIsSlowTask = false;
            if (blueprintPtr == nullptr)
            {
                KS::Log::Warning("Unable to load blueprint " + std::string(TCHAR_TO_UTF8(*className)), LOG_CHANNEL);
                return nullptr;
            }
            classPtr = blueprintPtr->GeneratedClass;
        }
        else
        {
            classPtr = FindObject<UClass>(ANY_PACKAGE, *className);
        }
        if (classPtr == nullptr)
        {
            KS::Log::Warning("Unable to find class " + std::string(TCHAR_TO_UTF8(*className)), LOG_CHANNEL);
            return nullptr;
        }

        GEngine->OnLevelActorAdded().Remove(m_onActorAddedHandle);
        UWorld* worldPtr = GEditor->GetEditorWorldContext().World();
        FActorSpawnParameters spawnParameters;
        spawnParameters.OverrideLevel = levelPtr;
        actorPtr = worldPtr->SpawnActor<AActor>(classPtr, location, rotation, spawnParameters);
        m_onActorAddedHandle = GEngine->OnLevelActorAdded().AddRaw(this, &sfActorManager::OnActorAdded);
    }
    else
    {
        // Detach from parent to avoid possible loops when we try to attach its children
        GEngine->OnLevelActorDetached().Remove(m_onActorDetachedHandle);
        actorPtr->DetachFromActor(FDetachmentTransformRules::KeepRelativeTransform);
        m_onActorDetachedHandle = GEngine->OnLevelActorDetached().AddRaw(this, &sfActorManager::OnAttachDetach);
        if (actorPtr->IsSelected())
        {
            objPtr->RequestLock();
            m_selectedActors[actorPtr] = objPtr;
        }
        if (actorPtr->IsA<ABrush>())
        {
            ABrush::SetNeedRebuild(actorPtr->GetLevel());
            m_bspRebuildDelay = BSP_REBUILD_DELAY;
        }
    }
    // If we recreate a deleted actor, the location and rotation may be wrong so we need to set it again
    actorPtr->SetActorRelativeLocation(location);
    actorPtr->SetActorRelativeRotation(rotation);
    actorPtr->SetActorRelativeScale3D(scale);
    actorPtr->SetFolderPath(FName(*sfPropertyUtil::ToString(propertiesPtr->Get(sfProp::Folder))));

    FString label = sfPropertyUtil::ToString(propertiesPtr->Get(sfProp::Label));
    // Calling SetActorLabel will change the actor's name (id), even if the label doesn't change. So we check first if
    // the label is different
    if (label != actorPtr->GetActorLabel())
    {
        FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(m_onPropertyChangeHandle);
        actorPtr->SetActorLabel(sfPropertyUtil::ToString(propertiesPtr->Get(sfProp::Label)));
        m_onPropertyChangeHandle =
            FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(this, &sfActorManager::OnUPropertyChange);
    }
    // Set name after setting label because setting label changes the name
    sfActorUtil::TryRename(actorPtr, name);

    ApplyStaticMeshProperties(actorPtr, propertiesPtr) ||
        ApplySkeletalMeshProperties(actorPtr, propertiesPtr) ||
        ApplyEmitterProperties(actorPtr, propertiesPtr);

#if SYNC_ACTOR_PROPERTIES
    sfPropertyUtil::ApplyProperties(actorPtr, propertiesPtr);
#endif

    m_actorToObjectMap.Add(actorPtr, objPtr);
    m_objectToActorMap[objPtr] = actorPtr;
    SceneFusion::RedrawActiveViewport();

    if (objPtr->IsLocked())
    {
        OnLock(objPtr);
    }
    InvokeOnLockStateChange(objPtr, actorPtr);

    // Initialize children
    for (sfObject::SPtr childPtr : objPtr->Children())
    {
        AActor* childActorPtr;
        auto iter = m_objectToActorMap.find(childPtr);
        if (iter != m_objectToActorMap.end())
        {
            childActorPtr = iter->second;
            propertiesPtr = childPtr->Property()->AsDict();
            childActorPtr->SetActorRelativeLocation(
                sfPropertyUtil::ToVector(propertiesPtr->Get(sfProp::Location)));
            childActorPtr->SetActorRelativeRotation(
                sfPropertyUtil::ToRotator(propertiesPtr->Get(sfProp::Rotation)));
            childActorPtr->SetActorRelativeScale3D(sfPropertyUtil::ToVector(propertiesPtr->Get(sfProp::Scale)));
        }
        else
        {
            childActorPtr = InitializeActor(childPtr, levelPtr);
        }
        if (childActorPtr != nullptr)
        {
            GEngine->OnLevelActorAdded().Remove(m_onActorAttachedHandle);
            childActorPtr->AttachToActor(actorPtr, FAttachmentTransformRules::KeepRelativeTransform);
            m_onActorAttachedHandle = GEngine->OnLevelActorAttached().AddRaw(this, &sfActorManager::OnAttachDetach);
        }
    }

    return actorPtr;
}

bool sfActorManager::CreateStaticMeshProperties(AActor* actorPtr, sfDictionaryProperty::SPtr propertiesPtr)
{
    AStaticMeshActor* staticMeshActorPtr = Cast<AStaticMeshActor>(actorPtr);
    if (staticMeshActorPtr == nullptr)
    {
        return false;
    }
    UStaticMeshComponent* componentPtr = staticMeshActorPtr->GetStaticMeshComponent();
    if (componentPtr != nullptr)
    {
        FString path = componentPtr->GetStaticMesh() == nullptr ? "" : componentPtr->GetStaticMesh()->GetPathName();
        propertiesPtr->Set(sfProp::Mesh, sfPropertyUtil::FromString(path, m_sessionPtr));

        sfListProperty::SPtr materialsPropPtr = sfListProperty::Create();
        TArray<UMaterialInterface*> materials = componentPtr->GetMaterials();
        for (UMaterialInterface* materialPtr : materials)
        {
            path = materialPtr == nullptr ? "" : materialPtr->GetPathName();
            materialsPropPtr->Add(sfPropertyUtil::FromString(path, m_sessionPtr));
        }
        propertiesPtr->Set(sfProp::Materials, materialsPropPtr);
    }
    return true;
}

bool sfActorManager::ApplyStaticMeshProperties(AActor* actorPtr, sfDictionaryProperty::SPtr propertiesPtr)
{
    AStaticMeshActor* staticMeshActorPtr = Cast<AStaticMeshActor>(actorPtr);
    if (staticMeshActorPtr == nullptr)
    {
        return false;
    }
    sfProperty::SPtr propPtr;
    UStaticMeshComponent* componentPtr = staticMeshActorPtr->GetStaticMeshComponent();
    if (componentPtr != nullptr && propertiesPtr->TryGet(sfProp::Mesh, propPtr))
    {
        FString path = sfPropertyUtil::ToString(propPtr);
        if (path.IsEmpty())
        {
            componentPtr->SetStaticMesh(nullptr);
        }
        else
        {
            // Disable loading dialog that causes a crash if we are dragging objects
            GIsSlowTask = true;
            componentPtr->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, *path));
            GIsSlowTask = false;
        }
        sfListProperty::SPtr materialsPtr = propertiesPtr->Get(sfProp::Materials)->AsList();
        int numMaterials = FMath::Min(componentPtr->GetNumMaterials(), materialsPtr->Size());
        if (componentPtr->GetNumMaterials() != materialsPtr->Size())
        {
            KS::Log::Warning("Material count mismatch on static mesh '" + std::string(TCHAR_TO_UTF8(*path)) +
                "'. Server has " + std::to_string(materialsPtr->Size()) + " but we have " +
                std::to_string(componentPtr->GetNumMaterials()), LOG_CHANNEL);
        }
        for (int i = 0; i < numMaterials; i++)
        {
            path = sfPropertyUtil::ToString(materialsPtr->Get(i));
            if (path.IsEmpty())
            {
                componentPtr->SetMaterial(i, nullptr);
            }
            else
            {
                // Disable loading dialog that causes a crash if we are dragging objects
                GIsSlowTask = true;
                componentPtr->SetMaterial(i, LoadObject<UMaterialInterface>(nullptr, *path));
                GIsSlowTask = false;
            }
        }
    }
    return true;
}

bool sfActorManager::CreateSkeletalMeshProperties(AActor* actorPtr, sfDictionaryProperty::SPtr propertiesPtr)
{
    ASkeletalMeshActor* skeletalMeshPtr = Cast<ASkeletalMeshActor>(actorPtr);
    if (skeletalMeshPtr == nullptr)
    {
        return false;
    }
    USkeletalMeshComponent* componentPtr = skeletalMeshPtr->GetSkeletalMeshComponent();
    if (componentPtr != nullptr)
    {
        FString path = componentPtr->SkeletalMesh == nullptr ? "" : componentPtr->SkeletalMesh->GetPathName();
        propertiesPtr->Set(sfProp::Mesh, sfPropertyUtil::FromString(path, m_sessionPtr));

        sfListProperty::SPtr materialsPropPtr = sfListProperty::Create();
        TArray<UMaterialInterface*> materials = componentPtr->GetMaterials();
        for (UMaterialInterface* materialPtr : materials)
        {
            path = materialPtr == nullptr ? "" : materialPtr->GetPathName();
            materialsPropPtr->Add(sfPropertyUtil::FromString(path, m_sessionPtr));
        }
        propertiesPtr->Set(sfProp::Materials, materialsPropPtr);
    }
    return true;
}

bool sfActorManager::ApplySkeletalMeshProperties(AActor* actorPtr, sfDictionaryProperty::SPtr propertiesPtr)
{
    ASkeletalMeshActor* skeletalMeshPtr = Cast<ASkeletalMeshActor>(actorPtr);
    if (skeletalMeshPtr == nullptr)
    {
        return false;
    }
    sfProperty::SPtr propPtr;
    USkeletalMeshComponent* componentPtr = skeletalMeshPtr->GetSkeletalMeshComponent();
    if (componentPtr != nullptr && propertiesPtr->TryGet(sfProp::Mesh, propPtr))
    {
        FString path = sfPropertyUtil::ToString(propPtr);
        if (path.IsEmpty())
        {
            componentPtr->SetSkeletalMesh(nullptr);
        }
        else
        {
            // Disable loading dialog that causes a crash if we are dragging objects
            GIsSlowTask = true;
            componentPtr->SetSkeletalMesh(LoadObject<USkeletalMesh>(nullptr, *path));
            GIsSlowTask = false;
        }
        sfListProperty::SPtr materialsPtr = propertiesPtr->Get(sfProp::Materials)->AsList();
        int numMaterials = FMath::Min(componentPtr->GetNumMaterials(), materialsPtr->Size());
        if (componentPtr->GetNumMaterials() != materialsPtr->Size())
        {
            KS::Log::Warning("Material count mismatch on skeletal mesh '" + std::string(TCHAR_TO_UTF8(*path)) +
                "'. Server has " + std::to_string(materialsPtr->Size()) + " but we have " +
                std::to_string(componentPtr->GetNumMaterials()), LOG_CHANNEL);
        }
        for (int i = 0; i < numMaterials; i++)
        {
            path = sfPropertyUtil::ToString(materialsPtr->Get(i));
            if (path.IsEmpty())
            {
                componentPtr->SetMaterial(i, nullptr);
            }
            else
            {
                // Disable loading dialog that causes a crash if we are dragging objects
                GIsSlowTask = true;
                componentPtr->SetMaterial(i, LoadObject<UMaterialInterface>(nullptr, *path));
                GIsSlowTask = false;
            }
        }
    }
    return true;
}

bool sfActorManager::CreateEmitterProperties(AActor* actorPtr, sfDictionaryProperty::SPtr propertiesPtr)
{
    AEmitter* emitterPtr = Cast<AEmitter>(actorPtr);
    if (emitterPtr == nullptr)
    {
        return false;
    }
    UParticleSystemComponent* componentPtr = emitterPtr->GetParticleSystemComponent();
    if (componentPtr != nullptr && componentPtr->Template != nullptr)
    {
        propertiesPtr->Set(sfProp::Template,
            sfPropertyUtil::FromString(componentPtr->Template->GetPathName(), m_sessionPtr));
    }
    return true;
}

bool sfActorManager::ApplyEmitterProperties(AActor* actorPtr, sfDictionaryProperty::SPtr propertiesPtr)
{
    AEmitter* emitterPtr = Cast<AEmitter>(actorPtr);
    if (emitterPtr == nullptr)
    {
        return false;
    }
    sfProperty::SPtr propPtr;
    UParticleSystemComponent* componentPtr = emitterPtr->GetParticleSystemComponent();
    if (componentPtr != nullptr && propertiesPtr->TryGet(sfProp::Template, propPtr))
    {
        // Disable loading dialog that causes a crash if we are dragging objects
        GIsSlowTask = true;
        componentPtr->SetTemplate(LoadObject<UParticleSystem>(nullptr, *sfPropertyUtil::ToString(propPtr)));
        GIsSlowTask = false;
    }
    return true;
}

void sfActorManager::OnActorDeleted(AActor* actorPtr)
{
    // Ignore actors in the buffer level.
    // The buffer level is a temporary level used when moving actors to a different level.
    if (actorPtr->GetOutermost() == GetTransientPackage())
    {
        return;
    }

    sfObject::SPtr objPtr;
    if (m_actorToObjectMap.RemoveAndCopyValue(actorPtr, objPtr))
    {
        objPtr->ReleaseLock();
        m_objectToActorMap.erase(objPtr);
        if (objPtr->IsLocked())
        {
            m_recreateQueue.Enqueue(objPtr);
        }
        else
        {
            // Attach children to level object before deleting the object
            sfObject::SPtr levelObjPtr = m_levelManagerPtr->GetOrCreateLevelObject(actorPtr->GetLevel());
            while (objPtr->Children().size() > 0)
            {
                sfObject::SPtr childPtr = objPtr->Child(0);
                levelObjPtr->AddChild(childPtr);
                auto iter = m_objectToActorMap.find(childPtr);
                if (iter != m_objectToActorMap.end())
                {
                    SendTransformUpdate(iter->second, childPtr);
                }
            }
            m_sessionPtr->Delete(objPtr);
        }
    }
    m_selectedActors.erase(actorPtr);
    m_propertyChangeMap.Remove(actorPtr);
    m_uploadList.Remove(actorPtr);
}

void sfActorManager::OnDelete(sfObject::SPtr objPtr)
{
    auto iter = m_objectToActorMap.find(objPtr);
    if (iter == m_objectToActorMap.end())
    {
        return;
    }
    AActor* actorPtr = iter->second;
    m_objectToActorMap.erase(iter);
    if (actorPtr->IsA<ABrush>())
    {
        m_bspRebuildDelay = BSP_REBUILD_DELAY;
    }
    UWorld* worldPtr = GEditor->GetEditorWorldContext().World();
    GEngine->OnLevelActorDeleted().Remove(m_onActorDeletedHandle);
    worldPtr->EditorDestroyActor(actorPtr, true);
    m_onActorDeletedHandle = GEngine->OnLevelActorDeleted().AddRaw(this, &sfActorManager::OnActorDeleted);
    m_actorToObjectMap.Remove(actorPtr);
    SceneFusion::RedrawActiveViewport();
}

void sfActorManager::OnLock(sfObject::SPtr objPtr)
{
    auto iter = m_objectToActorMap.find(objPtr);
    if (iter == m_objectToActorMap.end())
    {
        OnCreate(objPtr, 0);
        return;
    }
    AActor* actorPtr = iter->second;
    InvokeOnLockStateChange(objPtr, actorPtr);
    if (actorPtr->GetRootComponent() == nullptr)
    {
        return;
    }
    Lock(actorPtr, objPtr->LockOwner());
}

void sfActorManager::Lock(AActor* actorPtr, sfUser::SPtr lockOwnerPtr)
{
    if (m_lockMaterialPtr != nullptr)
    {
        UMaterialInterface* lockMaterialPtr = GetLockMaterial(lockOwnerPtr);
        TArray<UMeshComponent*> meshes;
        actorPtr->GetComponents(meshes);
        if (meshes.Num() > 0)
        {
            for (int i = 0; i < meshes.Num(); i++)
            {
                UsfLockComponent* lockPtr = NewObject<UsfLockComponent>(actorPtr, *FString("SFLock" + FString::FromInt(i)));
                lockPtr->CreationMethod = EComponentCreationMethod::Instance;
                lockPtr->SetMobility(meshes[i]->Mobility);
                lockPtr->AttachToComponent(meshes[i], FAttachmentTransformRules::KeepRelativeTransform);
                lockPtr->RegisterComponent();
                lockPtr->InitializeComponent();
                lockPtr->DuplicateParentMesh(lockMaterialPtr);
                SceneFusion::RedrawActiveViewport();
            }
            return;
        }
    }
    UsfLockComponent* lockPtr = NewObject<UsfLockComponent>(actorPtr, *FString("SFLock"));
    lockPtr->CreationMethod = EComponentCreationMethod::Instance;
    lockPtr->AttachToComponent(actorPtr->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
    lockPtr->RegisterComponent();
    lockPtr->InitializeComponent();
}

sfObject::SPtr sfActorManager::GetSFObjectByActor(AActor* actorPtr)
{
    return m_actorToObjectMap.FindRef(actorPtr);
}

UMaterialInterface* sfActorManager::GetLockMaterial(sfUser::SPtr userPtr)
{
    if (userPtr == nullptr)
    {
        return m_lockMaterialPtr;
    }
    UMaterialInstanceDynamic** materialPtrPtr = m_lockMaterials.Find(userPtr->Id());
    if (materialPtrPtr != nullptr)
    {
        return Cast<UMaterialInterface>(*materialPtrPtr);
    }
    UMaterialInstanceDynamic* materialPtr = UMaterialInstanceDynamic::Create(m_lockMaterialPtr, nullptr);
    materialPtr->SetFlags(EObjectFlags::RF_Standalone);//prevent material from being destroyed
    ksColor color = userPtr->Color();
    FLinearColor ucolor(color.R(), color.G(), color.B());
    materialPtr->SetVectorParameterValue("Color", ucolor);
    m_lockMaterials.Add(userPtr->Id(), materialPtr);
    return Cast<UMaterialInterface>(materialPtr);
}

void sfActorManager::OnUnlock(sfObject::SPtr objPtr)
{
    auto iter = m_objectToActorMap.find(objPtr);
    if (iter != m_objectToActorMap.end())
    {
        Unlock(iter->second);
        InvokeOnLockStateChange(objPtr, iter->second);
    }
}

void sfActorManager::Unlock(AActor* actorPtr)
{
    // If you undo the deletion of an actor with lock components, the lock components will not be part of the
    // OwnedComponents set so we have to use our own function to find them instead of AActor->GetComponents.
    // Not sure why this happens. It seems like an Unreal bug.
    TArray<UsfLockComponent*> locks;
    sfActorUtil::GetSceneComponents<UsfLockComponent>(actorPtr, locks);
    for (UsfLockComponent* lockPtr : locks)
    {
        lockPtr->DestroyComponent();
        SceneFusion::RedrawActiveViewport();
    }
    // When a selected actor becomes unlocked you have to unselect and reselect it to unlock the handles
    if (actorPtr->IsSelected())
    {
        GEditor->SelectActor(actorPtr, false, true);
        GEditor->SelectActor(actorPtr, true, true);
    }
}

void sfActorManager::OnLockOwnerChange(sfObject::SPtr objPtr)
{
    auto iter = m_objectToActorMap.find(objPtr);
    if (iter == m_objectToActorMap.end())
    {
        return;
    }

    InvokeOnLockStateChange(objPtr, iter->second);

    UMaterialInterface* lockMaterialPtr = GetLockMaterial(objPtr->LockOwner());
    if (lockMaterialPtr == nullptr)
    {
        return;
    }
    TArray<UsfLockComponent*> locks;
    sfActorUtil::GetSceneComponents<UsfLockComponent>(iter->second, locks);
    for (UsfLockComponent* lockPtr : locks)
    {
        lockPtr->SetMaterial(lockMaterialPtr);
    }
}

void sfActorManager::OnAttachDetach(AActor* actorPtr, const AActor* parentPtr)
{
    // Unreal fires the detach event before updating the relative transform, and if we need to change the parent back
    // because of locks Unreal won't let us here, so we queue the actor to be processed later.
    m_syncParentList.AddUnique(actorPtr);
}

void sfActorManager::OnParentChange(sfObject::SPtr objPtr, int childIndex)
{
    auto iter = m_objectToActorMap.find(objPtr);
    if (iter == m_objectToActorMap.end())
    {
        return;
    }
    AActor* actorPtr = iter->second;
    if (objPtr->Parent() == nullptr)
    {
        LogNoParentErrorAndDisconnect(objPtr);
        return;
    }
    else
    {
        if (DetachIfParentIsLevel(objPtr, actorPtr))
        {
            return;
        }

        iter = m_objectToActorMap.find(objPtr->Parent());
        if (iter != m_objectToActorMap.end())
        {
            GEngine->OnLevelActorAdded().Remove(m_onActorAttachedHandle);
            actorPtr->AttachToActor(iter->second, FAttachmentTransformRules::KeepRelativeTransform);
            m_onActorAttachedHandle = GEngine->OnLevelActorAttached().AddRaw(this, &sfActorManager::OnAttachDetach);
        }
    }
}

void sfActorManager::OnFolderChange(const AActor* actorPtr, FName oldFolder)
{
    sfObject::SPtr objPtr = m_actorToObjectMap.FindRef(actorPtr);
    if (objPtr == nullptr)
    {
        return;
    }
    sfDictionaryProperty::SPtr propertiesPtr = objPtr->Property()->AsDict();
    if (objPtr->IsLocked())
    {
        // Reverting the folder now can break the world outliner, so we queue it to be done on the next tick
        m_revertFolderQueue.Enqueue(const_cast<AActor*>(actorPtr));
    }
    else
    {
        propertiesPtr->Set(sfProp::Folder,
            sfPropertyUtil::FromString(actorPtr->GetFolderPath().ToString(), m_sessionPtr));
    }
}

void sfActorManager::OnMoveStart(UObject& obj)
{
    m_movingActors = GCurrentLevelEditingViewportClient &&
        GCurrentLevelEditingViewportClient->bWidgetAxisControlledByDrag;
}

void sfActorManager::OnMoveEnd(UObject& obj)
{
    m_movingActors = false;
    for (auto iter : m_selectedActors)
    {
        SendTransformUpdate(iter.first, iter.second);
    }
}

void sfActorManager::SyncTransform(AActor* actorPtr)
{
    sfObject::SPtr objPtr = m_actorToObjectMap.FindRef(actorPtr);
    if (objPtr == nullptr)
    {
        return;
    }
    if (!objPtr->IsLocked())
    {
        SendTransformUpdate(actorPtr, objPtr);
    }
    else
    {
        ApplyServerTransform(actorPtr, objPtr);
    }
}

void sfActorManager::SendTransformUpdate(AActor* actorPtr, sfObject::SPtr objPtr)
{
    sfDictionaryProperty::SPtr propertiesPtr = objPtr->Property()->AsDict();

    USceneComponent* rootComponentPtr = actorPtr->GetRootComponent();
    if (rootComponentPtr == nullptr)
    {
        return;
    }
    sfProperty::SPtr oldPropPtr;
    if (!propertiesPtr->TryGet(sfProp::Location, oldPropPtr) ||
        rootComponentPtr->RelativeLocation != sfPropertyUtil::ToVector(oldPropPtr))
    {
        propertiesPtr->Set(sfProp::Location, sfPropertyUtil::FromVector(rootComponentPtr->RelativeLocation));
    }

    if (!propertiesPtr->TryGet(sfProp::Rotation, oldPropPtr) ||
        rootComponentPtr->RelativeRotation != sfPropertyUtil::ToRotator(oldPropPtr))
    {
        propertiesPtr->Set(sfProp::Rotation, sfPropertyUtil::FromRotator(rootComponentPtr->RelativeRotation));
    }

    FVector scale = actorPtr->GetActorRelativeScale3D();
    if (!propertiesPtr->TryGet(sfProp::Scale, oldPropPtr) || scale != sfPropertyUtil::ToVector(oldPropPtr))
    {
        propertiesPtr->Set(sfProp::Scale, sfPropertyUtil::FromVector(scale));
    }
}

void sfActorManager::ApplyServerTransform(AActor* actorPtr, sfObject::SPtr objPtr)
{
    sfDictionaryProperty::SPtr propertiesPtr = objPtr->Property()->AsDict();
    sfProperty::SPtr locationPtr;
    if (propertiesPtr->TryGet(sfProp::Location, locationPtr))
    {
        actorPtr->SetActorRelativeLocation(sfPropertyUtil::ToVector(locationPtr));
        actorPtr->SetActorRelativeRotation(sfPropertyUtil::ToRotator(propertiesPtr->Get(sfProp::Rotation)));
        actorPtr->SetActorRelativeScale3D(sfPropertyUtil::ToVector(propertiesPtr->Get(sfProp::Scale)));
    }
}

void sfActorManager::RegisterUndoTypes()
{
    m_undoTypes.Add("Move Actors", UndoType::Move);
    m_undoTypes.Add("Move Components", UndoType::Move);
    m_undoTypes.Add("Set Location", UndoType::Move);
    m_undoTypes.Add("Paste Location", UndoType::Move);
    m_undoTypes.Add("Rotate Actors", UndoType::Rotate);
    m_undoTypes.Add("Rotate Components", UndoType::Rotate);
    m_undoTypes.Add("Translate/RotateZ Actors", UndoType::Rotate);
    m_undoTypes.Add("Translate/RotateZ Components", UndoType::Rotate);
    m_undoTypes.Add("Translate/Rotate2D Actors", UndoType::Rotate);
    m_undoTypes.Add("Translate/Rotate2D Components", UndoType::Rotate);
    m_undoTypes.Add("Set Rotation", UndoType::Rotate);
    m_undoTypes.Add("Paste Rotation", UndoType::Rotate);
    m_undoTypes.Add("Scale Actors", UndoType::Scale);
    m_undoTypes.Add("Scale Components", UndoType::Scale);
    m_undoTypes.Add("Set Scale", UndoType::Scale);
    m_undoTypes.Add("Paste Scale", UndoType::Scale);
    m_undoTypes.Add("Create Actors", UndoType::Create);
    m_undoTypes.Add("Paste", UndoType::Create);
    m_undoTypes.Add("Delete Actors", UndoType::Delete);
    m_undoTypes.Add("Cut", UndoType::Delete);
    m_undoTypes.Add("Delete Selection", UndoType::DeleteOutliner);
    m_undoTypes.Add("Rename Actor", UndoType::Rename);
    m_undoTypes.Add("Rename Multiple Actors", UndoType::Rename);
    m_undoTypes.Add("Rename Folder", UndoType::Folder);
    m_undoTypes.Add("Create Folder", UndoType::Folder);
    m_undoTypes.Add("Move World Outliner Items", UndoType::Folder);
    m_undoTypes.Add("Attach actors", UndoType::Attach);
    m_undoTypes.Add("Detach actors", UndoType::Detach);
    m_undoTypes.Add("Add Child", UndoType::Edit);
    m_undoTypes.Add("Insert Child", UndoType::Edit);
    m_undoTypes.Add("Delete Child", UndoType::Edit);
    m_undoTypes.Add("Duplicate Child", UndoType::Edit);
    m_undoTypes.Add("Clear Children", UndoType::Edit);
    m_undoTypes.Add("Move Row", UndoType::Edit);
    m_undoTypes.Add("Move Actors To Level", UndoType::MoveToLevel);
    m_undoTypes.Add("Move Selected Actors To Level", UndoType::MoveToLevel);
}

void sfActorManager::OnUndo(FUndoSessionContext context, bool success)
{
    if (success)
    {
        FixTransactedComponentChildren();
        OnUndoRedo(context.Title.ToString(), true);
        DestroyUnwantedActors();
    }
}

void sfActorManager::OnRedo(FUndoSessionContext context, bool success)
{
    if (success)
    {
        FixTransactedComponentChildren();
        OnUndoRedo(context.Title.ToString(), false);
        DestroyUnwantedActors();
    }
}

void sfActorManager::BeforeUndoRedo(FUndoSessionContext context)
{
    // Because componet child lists can be incorrect if another user changed the child list after the transaction was
    // recorded, we need to store the child components before the undoing or redoing the transaction so we can correct
    // bad state after.
    FString action = context.Title.ToString();
    int index = m_undoBufferPtr->UndoBuffer.Num() - m_undoBufferPtr->GetUndoCount();
    const FTransaction* transactionPtr = m_undoBufferPtr->GetTransaction(index);
    // We don't know which transaction is being undone or redone because we don't know if this is an undo or redo, so
    // we check if the title matches the context title.
    if (transactionPtr != nullptr && action == transactionPtr->GetContext().Title.ToString())
    {
        RecordPreTransactionState(transactionPtr);
    }
    transactionPtr = m_undoBufferPtr->GetTransaction(index - 1);
    if (transactionPtr != nullptr && action == transactionPtr->GetContext().Title.ToString())
    {
        RecordPreTransactionState(transactionPtr);
    }
}

void sfActorManager::RecordPreTransactionState(const FTransaction* transactionPtr)
{
    TArray<UObject*> objs;
    transactionPtr->GetTransactionObjects(objs);
    for (UObject* uobjPtr : objs)
    {
        AActor* actorPtr = Cast<AActor>(uobjPtr);
        if (actorPtr != nullptr)
        {
            if (actorPtr->IsPendingKill())
            {
                m_destroyedActorsToCheck.AddUnique(actorPtr);
            }
            continue;
        }

        USceneComponent* componentPtr = Cast<USceneComponent>(uobjPtr);
        if (componentPtr == nullptr)
        {
            continue;
        }
        m_parentsToCheck.AddUnique(componentPtr);
        for (USceneComponent* childPtr : componentPtr->GetAttachChildren())
        {
            if (childPtr != nullptr)
            {
                m_childrenToCheck.AddUnique(childPtr);
            }
        }
    }
}

void sfActorManager::FixTransactedComponentChildren()
{
    // Iterate components in the transaction and remove components in their child lists that should not be there
    for (USceneComponent* componentPtr : m_parentsToCheck)
    {
        TArray<USceneComponent*>& children = const_cast<TArray<USceneComponent*>&>(componentPtr->GetAttachChildren());
        for (int i = children.Num() - 1; i >= 0; i--)
        {
            USceneComponent* childPtr = children[i];
            if (childPtr == nullptr || childPtr->GetAttachParent() != componentPtr)
            {
                children.RemoveAt(i);
                if (childPtr != nullptr && childPtr->GetOwner() == componentPtr->GetOwner() &&
                    childPtr->GetAttachParent() == nullptr &&
                    (childPtr->GetOwner() == nullptr || childPtr->GetOwner()->GetRootComponent() != childPtr))
                {
                    childPtr->DestroyComponent();
                }
            }
        }
    }
    // Iterate the children of components we stored before the transaction and add them to their parent's child list if
    // they are missing.
    for (USceneComponent* componentPtr : m_childrenToCheck)
    {
        USceneComponent* parentPtr = componentPtr->GetAttachParent();
        if (parentPtr != nullptr && !parentPtr->GetAttachChildren().Contains(componentPtr))
        {
            TArray<USceneComponent*>& children = const_cast<TArray<USceneComponent*>&>(parentPtr->GetAttachChildren());
            children.Add(componentPtr);
        }
    }
    m_parentsToCheck.Empty();
    m_childrenToCheck.Empty();
}

void sfActorManager::DestroyUnwantedActors()
{
    UWorld* worldPtr = GEditor->GetEditorWorldContext().World();
    for (AActor* actorPtr : m_destroyedActorsToCheck)
    {
        if (!actorPtr->IsPendingKill() && !m_uploadList.Contains(actorPtr))
        {
            GEngine->OnLevelActorDeleted().Remove(m_onActorDeletedHandle);
            worldPtr->EditorDestroyActor(actorPtr, true);
            m_onActorDeletedHandle = GEngine->OnLevelActorDeleted().AddRaw(this,
                &sfActorManager::OnActorDeleted);
        }
    }
    m_destroyedActorsToCheck.Empty();
}

void sfActorManager::OnUndoRedo(FString action, bool isUndo)
{
    int index = m_undoBufferPtr->UndoBuffer.Num() - m_undoBufferPtr->GetUndoCount();
    if (!isUndo)
    {
        index--;
    }
    const FTransaction* transactionPtr = m_undoBufferPtr->GetTransaction(index);
    if (transactionPtr == nullptr)
    {
        return;
    }
    UndoType* undoTypePtr = m_undoTypes.Find(action);
    UndoType undoType = undoTypePtr == nullptr ? UndoType::None : *undoTypePtr;
    if (undoType == UndoType::None && action.StartsWith("Edit "))
    {
        undoType = UndoType::Edit;
    }

    if (undoType == UndoType::Delete || undoType == UndoType::Create || UndoType::MoveToLevel)
    {
        // If BSP was rebuilt since the undo or create transaction was registered, we need to rebuild BSP again or
        // Unreal may crash. Unreal ignores calls to rebuild BSP during a transaction, but we can hack our way around
        // this by setting GIsTransacting to false.
        GIsTransacting = false;
        ABrush::SetNeedRebuild(GEditor->GetEditorWorldContext().World()->GetCurrentLevel());
        m_bspRebuildDelay = 0.0f;
        SceneFusion::RedrawActiveViewport();
        GEditor->RebuildAlteredBSP();
        GIsTransacting = true;
    }

    TArray<UObject*> objs;
    transactionPtr->GetTransactionObjects(objs);
    for (UObject* uobjPtr : objs)
    {
        AActor* actorPtr = Cast<AActor>(uobjPtr);
        if (actorPtr == nullptr)
        {
            continue;
        }
        sfObject::SPtr objPtr = m_actorToObjectMap.FindRef(actorPtr);
        sfDictionaryProperty::SPtr propertiesPtr = objPtr == nullptr ?
            nullptr : objPtr->Property()->AsDict();
        if (objPtr != nullptr)
        {
            actorPtr->bLockLocation = objPtr->IsLocked();
        }
        switch (undoType)
        {
            case UndoType::Move:
            case UndoType::Rotate:
            {
                OnUndoRedoMove(actorPtr, objPtr, propertiesPtr, undoType == UndoType::Rotate);
                break;
            }
            case UndoType::Scale:
            {
                SyncScale(actorPtr, objPtr, propertiesPtr);
                break;
            }
            case UndoType::DeleteOutliner:
            {
                if (!actorPtr->IsPendingKill())
                {
                    SyncFolder(actorPtr, objPtr, propertiesPtr);
                }
                // Do not break--we want to also run the Delete case
            }
            case UndoType::Create:
            case UndoType::Delete:
            case UndoType::MoveToLevel:
            {
                if (actorPtr->IsPendingKill())
                {
                    OnActorDeleted(actorPtr);
                }
                else if (objPtr == nullptr)
                {
                    OnUndoDelete(actorPtr);
                }
                break;
            }
            case UndoType::Rename:
            {
                SyncLabelAndName(actorPtr, objPtr, propertiesPtr);
                break;
            }
            case UndoType::Folder:
            {
                SyncFolder(actorPtr, objPtr, propertiesPtr);
                SyncParent(actorPtr, objPtr);
                break;
            }
            case UndoType::Attach:
            {
                SyncParent(actorPtr, objPtr);
                break;
            }
            case UndoType::Detach:
            {
                if (isUndo)
                {
                    OnUndoDetach(actorPtr);
                }
                else
                {
                    OnRedoDetach(actorPtr, objPtr);
                }
                break;
            }
#if SYNC_ACTOR_PROPERTIES
            case UndoType::Edit:
            {
                if (propertiesPtr != nullptr)
                {
                    // Unreal doesn't tell us which property changed, so we iterate them looking for changes
                    if (objPtr->IsLocked())
                    {
                        sfPropertyUtil::ApplyProperties(actorPtr, propertiesPtr);
                    }
                    else
                    {
                        sfPropertyUtil::SendPropertyChanges(actorPtr, propertiesPtr);
                    }
                }
                break;
            }
#endif
        }
    }
}

void sfActorManager::OnUndoRedoMove(
    AActor* actorPtr,
    sfObject::SPtr objPtr,
    sfDictionaryProperty::SPtr propertiesPtr,
    bool isRotation)
{
    if (actorPtr->IsSelected())
    {
        if (objPtr == nullptr)
        {
            // We're probably redoing an alt-drag (copy drag).
            OnUndoDelete(actorPtr);
        }
        else
        {
            // Rotating multiple actors may also change their location, so we check location in both cases
            FVector oldLocation = sfPropertyUtil::ToVector(propertiesPtr->Get(sfProp::Location));
            USceneComponent* rootComponentPtr = actorPtr->GetRootComponent();
            if (rootComponentPtr == nullptr)
            {
                return;
            }
            if (rootComponentPtr->RelativeLocation != oldLocation)
            {
                if (objPtr->IsLocked())
                {
                    actorPtr->SetActorRelativeLocation(oldLocation);
                }
                else
                {
                    propertiesPtr->Set(sfProp::Location, sfPropertyUtil::FromVector(rootComponentPtr->RelativeLocation));
                }
            }
            if (isRotation)
            {
                FRotator oldRotation = sfPropertyUtil::ToRotator(propertiesPtr->Get(sfProp::Rotation));
                // If we're undoing an alt-drag, the rotation of the original actor won't have changed
                if (rootComponentPtr->RelativeRotation != oldRotation)
                {
                    if (objPtr->IsLocked())
                    {
                        actorPtr->SetActorRelativeRotation(oldRotation);
                    }
                    else
                    {
                        propertiesPtr->Set(sfProp::Rotation, sfPropertyUtil::FromRotator(rootComponentPtr->RelativeRotation));
                    }
                }
            }
        }
    }
    else if (actorPtr->IsPendingKill())
    {
        // We're undoing an alt-drag (copy drag).
        OnActorDeleted(actorPtr);
    }
}

void sfActorManager::OnUndoDelete(AActor* actorPtr)
{
    if (!IsSyncable(actorPtr))
    {
        return;
    }
    bool inLevel = false;
    UWorld* worldPtr = GEditor->GetEditorWorldContext().World();
    for (AActor* existActorPtr : actorPtr->GetLevel()->Actors)
    {
        if (existActorPtr == nullptr)
        {
            continue;
        }

        if (existActorPtr == actorPtr)
        {
            inLevel = true;
        }
        else if (existActorPtr->GetFName() == actorPtr->GetFName())
        {
            // An actor with the same name already exists. Rename and delete the one that was just created. Although we
            // will delete it, we still need to rename it because names of deleted actors are still in use.
            sfActorUtil::Rename(actorPtr, actorPtr->GetName() + " (deleted)");
            GEngine->OnLevelActorDeleted().Remove(m_onActorDeletedHandle);
            worldPtr->EditorDestroyActor(actorPtr, true);
            m_onActorDeletedHandle = GEngine->OnLevelActorDeleted().AddRaw(this,
                &sfActorManager::OnActorDeleted);
            return;
        }
    }
    if (!inLevel)
    {
        // The actor is not in the world. This means the actor was deleted by another user and should not be recreated,
        // so we delete it.
        GEngine->OnLevelActorDeleted().Remove(m_onActorDeletedHandle);
        worldPtr->EditorDestroyActor(actorPtr, true);
        m_onActorDeletedHandle = GEngine->OnLevelActorDeleted().AddRaw(this,
            &sfActorManager::OnActorDeleted);
        return;
    }
    // If the actor was locked when it was deleted, it will still have a lock component, so we need to unlock it.
    Unlock(actorPtr);
    m_uploadList.AddUnique(actorPtr);
}

void sfActorManager::OnUndoDetach(AActor* actorPtr)
{
    // The actor in the transaction is the parent, so we need to iterate the children to find which child or children
    // were re-attached.
    TArray<AActor*> children;
    actorPtr->GetAttachedActors(children);
    for (AActor* childPtr : children)
    {
        sfObject::SPtr childObjPtr = m_actorToObjectMap.FindRef(childPtr);
        if (childObjPtr != nullptr)
        {
            SyncParent(childPtr, childObjPtr);
            // Detaching may change the folder, so we sync it.
            sfDictionaryProperty::SPtr propertiesPtr = childObjPtr->Property()->AsDict();
            SyncFolder(childPtr, childObjPtr, propertiesPtr);
        }
    }
}

void sfActorManager::OnRedoDetach(AActor* actorPtr, sfObject::SPtr objPtr)
{
    if (objPtr == nullptr)
    {
        return;
    }
    // The actor in the transaction is the parent, so we need to iterate the children to find which child or children
    // were re-detached.
    std::vector<TPair<sfObject::SPtr, AActor*>> toDetach;
    for (sfObject::SPtr childObjPtr : objPtr->Children())
    {
        auto iter = m_objectToActorMap.find(childObjPtr);
        if (iter != m_objectToActorMap.end() && iter->second->GetAttachParentActor() == nullptr)
        {
            if (childObjPtr->IsLocked())
            {
                GEngine->OnLevelActorAdded().Remove(m_onActorAttachedHandle);
                iter->second->AttachToActor(actorPtr, FAttachmentTransformRules::KeepWorldTransform);
                m_onActorAttachedHandle = GEngine->OnLevelActorAttached().AddRaw(this, &sfActorManager::OnAttachDetach);
            }
            else
            {
                toDetach.emplace_back(childObjPtr, iter->second);
            }
            // Detaching may change the folder, so we sync it.
            sfDictionaryProperty::SPtr propertiesPtr = childObjPtr->Property()->AsDict();
            SyncFolder(iter->second, childObjPtr, propertiesPtr);
        }
    }
    for (TPair<sfObject::SPtr, AActor*> pair : toDetach)
    {
        pair.Key->Detach();
        SendTransformUpdate(pair.Value, pair.Key);
    }
}

void sfActorManager::SyncScale(AActor* actorPtr, sfObject::SPtr objPtr, sfDictionaryProperty::SPtr propertiesPtr)
{
    if (propertiesPtr != nullptr)
    {
        if (objPtr->IsLocked())
        {
            actorPtr->SetActorScale3D(sfPropertyUtil::ToVector(propertiesPtr->Get(sfProp::Scale)));
        }
        else
        {
            propertiesPtr->Set(sfProp::Scale, sfPropertyUtil::FromVector(actorPtr->GetActorScale()));
        }
    }
}

void sfActorManager::SyncLabelAndName(
    AActor* actorPtr,
    sfObject::SPtr objPtr,
    sfDictionaryProperty::SPtr propertiesPtr)
{
    if (propertiesPtr != nullptr)
    {
        if (objPtr->IsLocked())
        {
            FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(m_onPropertyChangeHandle);
            actorPtr->SetActorLabel(sfPropertyUtil::ToString(propertiesPtr->Get(sfProp::Label)));
            m_onPropertyChangeHandle =
                FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(this, &sfActorManager::OnUPropertyChange);
            sfActorUtil::TryRename(actorPtr, sfPropertyUtil::ToString(propertiesPtr->Get(sfProp::Name)));
        }
        else
        {
            propertiesPtr->Set(sfProp::Label, sfPropertyUtil::FromString(actorPtr->GetActorLabel(),
                m_sessionPtr));
            FString name = actorPtr->GetName();
            if (sfPropertyUtil::ToString(propertiesPtr->Get(sfProp::Name)) != name)
            {
                propertiesPtr->Set(sfProp::Name, sfPropertyUtil::FromString(name, m_sessionPtr));
            }
        }
    }
}

void sfActorManager::SyncFolder(AActor* actorPtr, sfObject::SPtr objPtr, sfDictionaryProperty::SPtr propertiesPtr)
{
    if (propertiesPtr != nullptr)
    {
        FString newFolder = actorPtr->GetFolderPath().ToString();
        if (newFolder != sfPropertyUtil::ToString(propertiesPtr->Get(sfProp::Folder)))
        {
            if (objPtr->IsLocked())
            {
                // Setting folder during a transaction causes a crash, so we queue it to be done on the next tick
                m_revertFolderQueue.Enqueue(actorPtr);
            }
            else
            {
                propertiesPtr->Set(sfProp::Folder, sfPropertyUtil::FromString(newFolder, m_sessionPtr));
            }
        }
    }
}

void sfActorManager::SyncParent(AActor* actorPtr, sfObject::SPtr objPtr)
{
    if (objPtr == nullptr)
    {
        return;
    }

    sfObject::SPtr parentPtr = nullptr;
    if (actorPtr->GetAttachParentActor() != nullptr)
    {
        parentPtr = m_actorToObjectMap.FindRef(actorPtr->GetAttachParentActor());
    }
    else
    {
        parentPtr = m_levelManagerPtr->GetOrCreateLevelObject(actorPtr->GetLevel());
    }
    if (parentPtr == objPtr->Parent())
    {
        return;
    }
    if (objPtr->IsLocked() || (parentPtr != nullptr && parentPtr->IsFullyLocked()))
    {
        if (objPtr->Parent() == nullptr)
        {
            if (objPtr->IsSyncing())
            {
                LogNoParentErrorAndDisconnect(objPtr);
            }
            return;
        }

        if (DetachIfParentIsLevel(objPtr, actorPtr))
        {
            ApplyServerTransform(actorPtr, objPtr);
            return;
        }

        auto iter = m_objectToActorMap.find(objPtr->Parent());
        if (iter == m_objectToActorMap.end())
        {
            return;
        }
        GEngine->OnLevelActorAdded().Remove(m_onActorAttachedHandle);
        actorPtr->AttachToActor(iter->second, FAttachmentTransformRules::KeepRelativeTransform);
        m_onActorAttachedHandle = GEngine->OnLevelActorAttached().AddRaw(this, &sfActorManager::OnAttachDetach);
        ApplyServerTransform(actorPtr, objPtr);
    }
    else if (parentPtr == nullptr)
    {
        objPtr->Detach();
        SendTransformUpdate(actorPtr, objPtr);
    }
    else
    {
        parentPtr->AddChild(objPtr);
        SendTransformUpdate(actorPtr, objPtr);
    }
}

void sfActorManager::OnUPropertyChange(UObject* uobjPtr, FPropertyChangedEvent& ev)
{
    if (ev.MemberProperty == nullptr)
    {
        return;
    }
    AActor* actorPtr = Cast<AActor>(uobjPtr);
    if (actorPtr == nullptr || actorPtr->GetOutermost() == GetTransientPackage())
    {
        return;
    }
    // Sliding values in the details panel can generate nearly 1000 change events per second, so to throttle the update
    // rate we queue the property to be processed at most once per tick.
    std::unordered_set<UProperty*>& changedProperties = m_propertyChangeMap.FindOrAdd(actorPtr);
    changedProperties.emplace(ev.MemberProperty);
}

void sfActorManager::RehashProperties()
{
    for (TPair<FScriptMap*, TSharedPtr<FScriptMapHelper>>& pair : m_staleMaps)
    {
        pair.Value->Rehash();
    }
    m_staleMaps.Empty();
    for (TPair<FScriptSet*, TSharedPtr<FScriptSetHelper>>& pair : m_staleSets)
    {
        pair.Value->Rehash();
    }
    m_staleSets.Empty();
}

void sfActorManager::SendPropertyChanges()
{
    for (auto& pair : m_propertyChangeMap)
    {
        AActor* actorPtr = pair.Key;
        if (actorPtr->IsPendingKill())
        {
            continue;
        }

        for (UProperty* upropPtr : pair.Value)
        {
            sfObject::SPtr objPtr = m_actorToObjectMap.FindRef(actorPtr);
            if (objPtr == nullptr)
            {
                continue;
            }
            FString path = upropPtr->GetName();
            sfDictionaryProperty::SPtr propertiesPtr = objPtr->Property()->AsDict();

            USceneComponent* rootComponentPtr = actorPtr->GetRootComponent();
            if (rootComponentPtr != nullptr)
            {
                if (path == "RelativeLocation")
                {
                    propertiesPtr->Set(sfProp::Location, sfPropertyUtil::FromVector(rootComponentPtr->RelativeLocation));
                    continue;
                }
                if (path == "RelativeRotation")
                {
                    propertiesPtr->Set(sfProp::Rotation, sfPropertyUtil::FromRotator(rootComponentPtr->RelativeRotation));
                    continue;
                }
                if (path == "RelativeScale3D")
                {
                    propertiesPtr->Set(sfProp::Scale, sfPropertyUtil::FromVector(actorPtr->GetActorRelativeScale3D()));
                    continue;
                }
            }
            if (path == "ActorLabel")
            {
                // If the object is locked, renaming it now will cause a crash so we queue it to be done later
                m_syncLabelQueue.Enqueue(actorPtr);
                continue;
            }

#if SYNC_ACTOR_PROPERTIES
            std::string name = std::string(TCHAR_TO_UTF8(*path));
            if (sfPropertyUtil::IsDefaultValue(actorPtr, upropPtr))
            {
                propertiesPtr->Remove(name);
            }
            else
            {
                sfProperty::SPtr propPtr = sfPropertyUtil::GetValue(actorPtr, upropPtr);
                sfProperty::SPtr oldPropPtr;
                if (propPtr == nullptr)
                {
                    FString str = upropPtr->GetClass()->GetName() + " is not supported by Scene Fusion. Changes to " +
                        upropPtr->GetName() + " will not sync.";
                    KS::Log::Warning(TCHAR_TO_UTF8(*str));
                }
                else if (!propertiesPtr->TryGet(name, oldPropPtr) || !sfPropertyUtil::Copy(oldPropPtr, propPtr))
                {
                    propertiesPtr->Set(name, propPtr);
                }
            }
#endif
        }
    }
    m_propertyChangeMap.Empty();
}

void sfActorManager::OnPropertyChange(sfProperty::SPtr propertyPtr)
{
    auto actorIter = m_objectToActorMap.find(propertyPtr->GetContainerObject());
    if (actorIter == m_objectToActorMap.end())
    {
        return;
    }
    AActor* actorPtr = actorIter->second;

    if (propertyPtr->GetDepth() == 1)
    {
        auto handlerIter = m_propertyChangeHandlers.find(propertyPtr->Key());
        if (handlerIter != m_propertyChangeHandlers.end())
        {
            sfUtils::PreserveUndoStack([handlerIter, actorPtr, propertyPtr]()
            {
                handlerIter->second(actorPtr, propertyPtr);
            });
            return;
        }
    }

#if SYNC_ACTOR_PROPERTIES
    sfUPropertyInstance prop = sfPropertyUtil::FindUProperty(actorPtr, propertyPtr);
    if (prop.IsValid())
    {
        sfPropertyUtil::SetValue(prop, propertyPtr);
        if (prop.ContainerMap().IsValid())
        {
            m_staleMaps.Add(prop.ContainerMap()->Map, prop.ContainerMap());
        }
        if (prop.ContainerSet().IsValid())
        {
            m_staleSets.Add(prop.ContainerSet()->Set, prop.ContainerSet());
        }
    }
    else
    {
        KS::Log::Warning("Could not find property " + propertyPtr->GetPath() + " on " +
            std::string(TCHAR_TO_UTF8(*actorPtr->GetClass()->GetName())), LOG_CHANNEL);
    }
#endif
}

void sfActorManager::OnRemoveField(sfDictionaryProperty::SPtr dictPtr, const sfName& name)
{
    auto iter = m_objectToActorMap.find(dictPtr->GetContainerObject());
    if (iter == m_objectToActorMap.end())
    {
        return;
    }
    AActor* actorPtr = iter->second;

    UProperty* upropPtr = actorPtr->GetClass()->FindPropertyByName(FName(UTF8_TO_TCHAR(name->c_str())));
    if (upropPtr != nullptr)
    {
        sfPropertyUtil::SetToDefaultValue(actorPtr, upropPtr);
    }
}

void sfActorManager::OnListAdd(sfListProperty::SPtr listPtr, int index, int count)
{
    auto iter = m_objectToActorMap.find(listPtr->GetContainerObject());
    if (iter == m_objectToActorMap.end())
    {
        return;
    }
    AActor* actorPtr = iter->second;
    sfUPropertyInstance uprop = sfPropertyUtil::FindUProperty(actorPtr, listPtr);
    if (!uprop.IsValid())
    {
        return;
    }
    ArrayInsert(uprop, listPtr, index, count) || SetInsert(uprop, listPtr, index, count) ||
        MapInsert(uprop, listPtr, index, count);
}

bool sfActorManager::ArrayInsert(const sfUPropertyInstance& uprop, sfListProperty::SPtr listPtr, int index, int count)
{
    UArrayProperty* arrayPropPtr = Cast<UArrayProperty>(uprop.Property());
    if (arrayPropPtr == nullptr)
    {
        return false;
    }
    FScriptArrayHelper array(arrayPropPtr, uprop.Data());
    array.InsertValues(index, count);
    for (int i = index; i < index + count; i++)
    {
        sfPropertyUtil::SetValue(sfUPropertyInstance(arrayPropPtr->Inner, array.GetRawPtr(i)), listPtr->Get(i));
    }
    return true;
}

bool sfActorManager::SetInsert(const sfUPropertyInstance& uprop, sfListProperty::SPtr listPtr, int index, int count)
{
    USetProperty* setPropPtr = Cast<USetProperty>(uprop.Property());
    if (setPropPtr == nullptr)
    {
        return false;
    }
    TSharedPtr<FScriptSetHelper> setPtr = MakeShareable(new FScriptSetHelper(setPropPtr, uprop.Data()));
    int firstInsertIndex = setPtr->GetMaxIndex();
    int lastInsertIndex = 0;
    for (int i = 0; i < count; i++)
    {
        int insertIndex = setPtr->AddDefaultValue_Invalid_NeedsRehash();
        firstInsertIndex = FMath::Min(firstInsertIndex, insertIndex);
        lastInsertIndex = FMath::Max(lastInsertIndex, insertIndex);
    }
    int listIndex = -1;
    for (int i = 0; i < setPtr->GetMaxIndex(); i++)
    {
        if (!setPtr->IsValidIndex(i))
        {
            continue;
        }
        listIndex++;
        if (listIndex < index && i < firstInsertIndex)
        {
            continue;
        }
        sfPropertyUtil::SetValue(sfUPropertyInstance(setPropPtr->ElementProp, setPtr->GetElementPtr(i)),
            listPtr->Get(listIndex));
        if (listIndex >= index + count - 1 && i >= lastInsertIndex)
        {
            break;
        }
    }
    m_staleSets.Add(setPtr->Set, setPtr);
    return true;
}

bool sfActorManager::MapInsert(const sfUPropertyInstance& uprop, sfListProperty::SPtr listPtr, int index, int count)
{
    UMapProperty* mapPropPtr = Cast<UMapProperty>(uprop.Property());
    if (mapPropPtr == nullptr)
    {
        return false;
    }
    TSharedPtr<FScriptMapHelper> mapPtr = MakeShareable(new FScriptMapHelper(mapPropPtr, uprop.Data()));
    int firstInsertIndex = mapPtr->GetMaxIndex();
    int lastInsertIndex = 0;
    for (int i = 0; i < count; i++)
    {
        int insertIndex = mapPtr->AddDefaultValue_Invalid_NeedsRehash();
        firstInsertIndex = FMath::Min(firstInsertIndex, insertIndex);
        lastInsertIndex = FMath::Max(lastInsertIndex, insertIndex);
    }
    int listIndex = -1;
    for (int i = 0; i < mapPtr->GetMaxIndex(); i++)
    {
        if (!mapPtr->IsValidIndex(i))
        {
            continue;
        }
        listIndex++;
        if (listIndex < index && i < firstInsertIndex)
        {
            continue;
        }
        sfListProperty::SPtr pairPtr = listPtr->Get(listIndex)->AsList();
        sfPropertyUtil::SetValue(sfUPropertyInstance(mapPropPtr->KeyProp, mapPtr->GetKeyPtr(i)), pairPtr->Get(0));
        sfPropertyUtil::SetValue(sfUPropertyInstance(mapPropPtr->ValueProp, mapPtr->GetValuePtr(i)),
            pairPtr->Get(1));
        if (listIndex >= index + count - 1 && i >= lastInsertIndex)
        {
            break;
        }
    }
    m_staleMaps.Add(mapPtr->Map, mapPtr);
    return true;
}

void sfActorManager::OnListRemove(sfListProperty::SPtr listPtr, int index, int count)
{
    auto iter = m_objectToActorMap.find(listPtr->GetContainerObject());
    if (iter == m_objectToActorMap.end())
    {
        return;
    }
    AActor* actorPtr = iter->second;
    sfUPropertyInstance uprop = sfPropertyUtil::FindUProperty(actorPtr, listPtr);
    if (!uprop.IsValid())
    {
        return;
    }
    ArrayRemove(uprop, index, count) || SetRemove(uprop, index, count) || MapRemove(uprop, index, count);
}

bool sfActorManager::ArrayRemove(const sfUPropertyInstance& uprop, int index, int count)
{
    UArrayProperty* arrayPropPtr = Cast<UArrayProperty>(uprop.Property());
    if (arrayPropPtr == nullptr)
    {
        return false;
    }
    FScriptArrayHelper array(arrayPropPtr, uprop.Data());
    array.RemoveValues(index, count);
    return true;
}

bool sfActorManager::SetRemove(const sfUPropertyInstance& uprop, int index, int count)
{
    USetProperty* setPropPtr = Cast<USetProperty>(uprop.Property());
    if (setPropPtr == nullptr)
    {
        return false;
    }
    FScriptSetHelper set(setPropPtr, uprop.Data());
    int i = 0;
    for (; i < set.GetMaxIndex(); i++)
    {
        if (set.IsValidIndex(i))
        {
            index--;
            if (index < 0)
            {
                break;
            }
        }
    }
    set.RemoveAt(i, count);
    return true;
}

bool sfActorManager::MapRemove(const sfUPropertyInstance& uprop, int index, int count)
{
    UMapProperty* mapPropPtr = Cast<UMapProperty>(uprop.Property());
    if (mapPropPtr == nullptr)
    {
        return false;
    }
    FScriptMapHelper map(mapPropPtr, uprop.Data());
    int i = 0;
    for (; i < map.GetMaxIndex(); i++)
    {
        if (map.IsValidIndex(i))
        {
            index--;
            if (index < 0)
            {
                break;
            }
        }
    }
    map.RemoveAt(i, count);
    return true;
}

void sfActorManager::RegisterPropertyChangeHandlers()
{
    m_propertyChangeHandlers[sfProp::Location] =
        [this](AActor* actorPtr, sfProperty::SPtr propertyPtr)
    {
        actorPtr->SetActorRelativeLocation(sfPropertyUtil::ToVector(propertyPtr));
        actorPtr->InvalidateLightingCache();
        SceneFusion::RedrawActiveViewport();
        if (actorPtr->IsA<ABrush>())
        {
            ABrush::SetNeedRebuild(actorPtr->GetLevel());
            m_bspRebuildDelay = BSP_REBUILD_DELAY;
        }
    };
    m_propertyChangeHandlers[sfProp::Rotation] =
        [this](AActor* actorPtr, sfProperty::SPtr propertyPtr)
    {
        actorPtr->SetActorRelativeRotation(sfPropertyUtil::ToRotator(propertyPtr));
        actorPtr->InvalidateLightingCache();
        SceneFusion::RedrawActiveViewport();
        if (actorPtr->IsA<ABrush>())
        {
            ABrush::SetNeedRebuild(actorPtr->GetLevel());
            m_bspRebuildDelay = BSP_REBUILD_DELAY;
        }
    };
    m_propertyChangeHandlers[sfProp::Scale] =
        [this](AActor* actorPtr, sfProperty::SPtr propertyPtr)
    {
        actorPtr->SetActorRelativeScale3D(sfPropertyUtil::ToVector(propertyPtr));
        actorPtr->InvalidateLightingCache();
        SceneFusion::RedrawActiveViewport();
        if (actorPtr->IsA<ABrush>())
        {
            ABrush::SetNeedRebuild(actorPtr->GetLevel());
            m_bspRebuildDelay = BSP_REBUILD_DELAY;
        }
    };
    m_propertyChangeHandlers[sfProp::Name] = 
        [this](AActor* actorPtr, sfProperty::SPtr propertyPtr)
    {
        sfActorUtil::TryRename(actorPtr, sfPropertyUtil::ToString(propertyPtr));
    };
    m_propertyChangeHandlers[sfProp::Label] =
        [this](AActor* actorPtr, sfProperty::SPtr propertyPtr)
    {
        FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(m_onPropertyChangeHandle);
        actorPtr->SetActorLabel(sfPropertyUtil::ToString(propertyPtr));
        m_onPropertyChangeHandle =
            FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(this, &sfActorManager::OnUPropertyChange);
    };
    m_propertyChangeHandlers[sfProp::Folder] = 
        [this](AActor* actorPtr, sfProperty::SPtr propertyPtr)
    {
        m_foldersToCheck.AddUnique(actorPtr->GetFolderPath().ToString());
        GEngine->OnLevelActorFolderChanged().Remove(m_onFolderChangeHandle);
        actorPtr->SetFolderPath(FName(*sfPropertyUtil::ToString(propertyPtr)));
        m_onFolderChangeHandle = GEngine->OnLevelActorFolderChanged().AddRaw(this, &sfActorManager::OnFolderChange);
    };
}

void sfActorManager::OnUserColorChange(sfUser::SPtr userPtr)
{
    UMaterialInstanceDynamic** materialPtrPtr = m_lockMaterials.Find(userPtr->Id());
    if (materialPtrPtr == nullptr)
    {
        return;
    }
    UMaterialInstanceDynamic* materialPtr = *materialPtrPtr;
    ksColor color = userPtr->Color();
    FLinearColor ucolor(color.R(), color.G(), color.B());
    materialPtr->SetVectorParameterValue("Color", ucolor);
}

void sfActorManager::OnUserLeave(sfUser::SPtr userPtr)
{
    UMaterialInstanceDynamic* materialPtr;
    if (m_lockMaterials.RemoveAndCopyValue(userPtr->Id(), materialPtr))
    {
        materialPtr->ClearFlags(EObjectFlags::RF_Standalone);// Allow unreal to destroy the material instance
    }
}

bool sfActorManager::CanEdit(const TArray<TWeakObjectPtr<UObject>>& objects)
{
    if (m_selectedActors.size() == 0) {
        return true;
    }

    AActor* actorPtr;
    for (const TWeakObjectPtr<UObject>& objPtr : objects) {
        if (objPtr.IsValid()) {
            actorPtr = Cast<AActor>(objPtr.Get());

            // If we did not get an actor pointer, then try to get the actor owning this UObject.
            if (!actorPtr) {
                UActorComponent* actorComponent = Cast<UActorComponent>(objPtr.Get());
                if (actorComponent != nullptr) {
                    actorPtr = Cast<AActor>(actorComponent->GetOuter());
                }
            }

            // Check the locked state of the sfObject that maps to the actor.
            if (actorPtr) {
                auto iter = m_selectedActors.find(actorPtr);
                if (iter != m_selectedActors.end()) {
                    if (iter->second != nullptr && iter->second->IsLocked()) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

void sfActorManager::InvokeOnLockStateChange(sfObject::SPtr objPtr, AActor* actorPtr)
{
    LockType lockType = Unlocked;
    if (objPtr->IsFullyLocked())
    {
        lockType = FullyLocked;
    }
    else if (objPtr->IsPartiallyLocked())
    {
        lockType = PartiallyLocked;
    }
    OnLockStateChange.ExecuteIfBound(actorPtr, lockType, objPtr->LockOwner());
}

void sfActorManager::OnRemoveLevel(ULevel* levelPtr)
{
    for (auto actorIter = levelPtr->Actors.CreateConstIterator(); actorIter; actorIter++)
    {
        sfObject::SPtr objPtr;
        if (m_actorToObjectMap.RemoveAndCopyValue(*actorIter, objPtr))
        {
            objPtr->ReleaseLock();
            m_objectToActorMap.erase(objPtr);
            m_selectedActors.erase(*actorIter);
            m_uploadList.Remove(*actorIter);
        }
    }
}

void sfActorManager::OnSFLevelObjectCreate(sfObject::SPtr sfLevelObjPtr, ULevel* levelPtr)
{
    for (sfObject::SPtr childPtr : sfLevelObjPtr->Children())
    {
        OnCreate(childPtr, 0); // Child index does not matter
    }
    DestroyUnsyncedActorsInLevel(levelPtr);
}

int sfActorManager::NumSyncedActors()
{
    return m_actorToObjectMap.Num();
}

bool sfActorManager::DetachIfParentIsLevel(sfObject::SPtr objPtr, AActor* actorPtr)
{
    if (objPtr->Parent()->Type() == sfType::Level)
    {
        GEngine->OnLevelActorDetached().Remove(m_onActorDetachedHandle);
        actorPtr->DetachFromActor(FDetachmentTransformRules::KeepRelativeTransform);
        m_onActorDetachedHandle = GEngine->OnLevelActorDetached().AddRaw(this, &sfActorManager::OnAttachDetach);
        return true;
    }
    return false;
}

void sfActorManager::LogNoParentErrorAndDisconnect(sfObject::SPtr objPtr)
{
    sfDictionaryProperty::SPtr propertiesPtr = objPtr->Property()->AsDict();
    KS::Log::Error("Disconnecting because no parent object was found for actor " +
        propertiesPtr->Get(sfProp::Name)->ToString() +
        ". Root actor's parent object should be the level object.");
    SceneFusion::Service->LeaveSession();
}

#undef BSP_REBUILD_DELAY
#undef LOG_CHANNEL