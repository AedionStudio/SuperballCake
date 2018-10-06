#pragma once

#include <CoreMinimal.h>
#include <GameFramework/Actor.h>
#include <sfObject.h>
#include <sfSession.h>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <Editor/UnrealEd/Classes/Editor/TransBuffer.h>

#include "IObjectManager.h"
#include "../sfUPropertyInstance.h"
#include "sfLevelManager.h"

using namespace KS::SceneFusion2;
using namespace KS;

/**
 * Manages actor syncing.
 */
class sfActorManager : public IObjectManager
{
public:
    friend class sfLevelManager;

    /**
     * Types of lock.
     */
    enum LockType
    {
        NotSynced,
        Unlocked,
        PartiallyLocked,
        FullyLocked
    };

    /**
     * Delegate for lock state change.
     *
     * @param   AActor* - pointer to the actor whose lock state changed
     * @param   LockType - lock type
     * @param   sfUser::SPtr - lock owner
     */
    DECLARE_DELEGATE_ThreeParams(OnLockStateChangeDelegate, AActor*, LockType, sfUser::SPtr);

    /**
     * Lock state change event handler.
     */
    OnLockStateChangeDelegate OnLockStateChange;

    /**
     * Constructor
     *
     * @param   TSharedPtr<sfLevelManager> levelManagerPtr
     */
    sfActorManager(TSharedPtr<sfLevelManager> levelManagerPtr);

    /**
     * Destructor
     */
    virtual ~sfActorManager();

    /**
     * Initialization. Called after connecting to a session.
     */
    virtual void Initialize();

    /**
     * Deinitialization. Called after disconnecting from a session.
     */
    virtual void CleanUp();

    /**
     * Updates the actor manager.
     *
     * @param   float deltaTime in seconds since last tick.
     */
    void Tick(float deltaTime);

    /**
     * Checks if an actor can be synced.
     *
     * @param   AActor* actorPtr
     * @return  bool true if the actor can be synced.
     */
    bool IsSyncable(AActor* actorPtr);

    /**
     * Sends a new transform value to the server, or reverts to the server value if the actor is locked.
     *
     * @param   AActor* actorPtr to sync transform for.
     */
    void SyncTransform(AActor* actorPtr);

    /**
     * Check a list of objects against the selected list of actors. If any of the
     * selected objects is in the actor list and is locked then return false.
     *
     * @param   const TArray<TWeakObjectPtr<UObject>>& - object list
     * @param   bool
     */
    bool CanEdit(const TArray<TWeakObjectPtr<UObject>>& objects);

    /**
     * @return   int - number of synced actors.
     */
    int NumSyncedActors();
    
    /**
     * Get the sfObject for the given actor. If the actor is not synced, return nullptr.
     *
     * @param   AActor* actorPtr
     * @return  sfObject::SPtr
     */
    sfObject::SPtr GetSFObjectByActor(AActor* actorPtr);

private:
    typedef std::function<void(AActor*, sfProperty::SPtr)> PropertyChangeHandler;

    /**
     * Types of undo transactions we sync.
     */
    enum UndoType
    {
        None,
        Move,
        Rotate,
        Scale,
        Create,
        Delete,
        DeleteOutliner,
        Rename,
        Folder,
        Attach,
        Detach,
        Edit,
        MoveToLevel // Move actor to another level
    };

    FDelegateHandle m_onActorAddedHandle;
    FDelegateHandle m_onActorDeletedHandle;
    FDelegateHandle m_onActorAttachedHandle;
    FDelegateHandle m_onActorDetachedHandle;
    FDelegateHandle m_onFolderChangeHandle;
    FDelegateHandle m_onMoveStartHandle;
    FDelegateHandle m_onMoveEndHandle;
    FDelegateHandle m_onUndoHandle;
    FDelegateHandle m_onRedoHandle;
    FDelegateHandle m_beforeUndoRedoHandle;
    FDelegateHandle m_onPropertyChangeHandle;
    ksEvent<sfUser::SPtr&>::SPtr m_onUserColorChangeEventPtr;
    ksEvent<sfUser::SPtr&>::SPtr m_onUserLeaveEventPtr;

    TMap<AActor*, sfObject::SPtr> m_actorToObjectMap;
    std::map<sfObject::SPtr, AActor*> m_objectToActorMap;
    TMap<uint32_t, UMaterialInstanceDynamic*> m_lockMaterials;
    TMap<FScriptMap*, TSharedPtr<FScriptMapHelper>> m_staleMaps;
    TMap<FScriptSet*, TSharedPtr<FScriptSetHelper>> m_staleSets;
    TArray<AActor*> m_uploadList;
    TMap<AActor*, std::unordered_set<UProperty*>> m_propertyChangeMap;
    TQueue<sfObject::SPtr> m_recreateQueue;
    TQueue<AActor*> m_syncLabelQueue;
    TQueue<AActor*> m_revertFolderQueue;
    TArray<AActor*> m_syncParentList;
    TArray<FString> m_foldersToCheck;
    TArray<USceneComponent*> m_childrenToCheck;
    TArray<USceneComponent*> m_parentsToCheck;
    TArray<AActor*> m_destroyedActorsToCheck;
    TMap<FString, UndoType> m_undoTypes;
    // Use std map because TSortedMap causes compile errors in Unreal's code
    std::map<AActor*, sfObject::SPtr> m_selectedActors;
    std::unordered_map<sfName, PropertyChangeHandler> m_propertyChangeHandlers;
    sfSession::SPtr m_sessionPtr;
    UMaterialInterface* m_lockMaterialPtr;
    UTransBuffer* m_undoBufferPtr;
    bool m_movingActors;
    float m_bspRebuildDelay;

    TSharedPtr<sfLevelManager> m_levelManagerPtr;

    /**
     * Checks for selection changes and requests locks on newly selected objects and unlocks unselected objects.
     */
    void UpdateSelection();

    /**
     * Destroys actors that don't exist on the server in the given level.
     *
     * @param   ULevel* levelPtr - level to check
     */
    void DestroyUnsyncedActorsInLevel(ULevel* levelPtr);

    /**
     * Reverts folders to server values for actors whose folder changed while locked.
     */
    void RevertLockedFolders();

    /**
     * Recreates actors that were deleted while locked.
     */
    void RecreateLockedActors();

    /**
     * Deletes folders that were emptied by other users.
     */
    void DeleteEmptyFolders();

    /**
     * Decreases the rebuild bsp timer and rebuilds bsp if it reaches 0.
     *
     * @param   deltaTime in seconds since the last cick.
     */
    void RebuildBSPIfNeeded(float deltaTime);

    /**
     * Called when an actor is added to the level.
     *
     * @param   AActor* actorPtr
     */
    void OnActorAdded(AActor* actorPtr);

    /**
     * Called when an actor is deleted from the level.
     *
     * @param   AActor* actorPtr
     */
    void OnActorDeleted(AActor* actorPtr);

    /**
     * Called when an actor is attached to or detached from another actor.
     *
     * @param   AActor* actorPtr
     * @param   const AActor* parentPtr
     */
    void OnAttachDetach(AActor* actorPtr, const AActor* parentPtr);

    /**
     * Called when an actor's folder changes.
     *
     * @param   const AActor* actorPtr
     * @param   FName oldFolder
     */
    void OnFolderChange(const AActor* actorPtr, FName oldFolder);

    /**
     * Called when an object starts being dragged in the viewport.
     *
     * @param   UObject& obj
     */
    void OnMoveStart(UObject& obj);

    /**
     * Called when an object stops being dragged in the viewport.
     *
     * @param   UObject& object
     */
    void OnMoveEnd(UObject& obj);

    /**
     * Called when a property is changed through the details panel.
     *
     * @param   UObject* uobjPtr whose property changed.
     * @param   FPropertyChangedEvent& ev with information on what property changed.
     */
    void OnUPropertyChange(UObject* uobjPtr, FPropertyChangedEvent& ev);

    /**
     * Rehashes property containers whose keys were changed by other users.
     */
    void RehashProperties();

    /**
     * Sends queued property changes to the server.
     */
    void SendPropertyChanges();

    /**
     * Tries to insert elements from an sfListProperty into an array using reflection. Returns false if the UProperty
     * is not an array property.
     *
     * @param   const sfUPropertyInstance& uprop to try insert array elements for. Returns false if this is not an
     *          array property.
     * @param   sfListProperty::SPtr listPtr with elements to insert.
     * @param   int index of first element to insert, and the index to insert at.
     * @param   int count - number of elements to insert.
     * @return  bool true if the UProperty was an array property.
     */
    bool ArrayInsert(const sfUPropertyInstance& uprop, sfListProperty::SPtr listPtr, int index, int count);

    /**
     * Tries to remove elements from an array using reflection. Returns false if the UProperty is not an array
     * property.
     *
     * @param   const sfUPropertyInstance& uprop to try remove array elements from. Returns false if this is not an
     *          array property.
     * @param   int index of first element to remove.
     * @param   int count - number of elements to remove.
     * @return  bool true if the UProperty was an array property.
     */
    bool ArrayRemove(const sfUPropertyInstance& uprop, int index, int count);

    /**
     * Tries to insert elements from an sfListProperty into a set using reflection. Returns false if the UProperty is
     * not a set property.
     *
     * @param   const sfUPropertyInstance& uprop to try insert set elements for. Returns false if this is not a set
     *          property.
     * @param   sfListProperty::SPtr listPtr with elements to insert.
     * @param   int index of first element to insert, and the index to insert at.
     * @param   int count - number of elements to insert.
     * @return  bool true if the UProperty was a set property.
     */
    bool SetInsert(const sfUPropertyInstance& uprop, sfListProperty::SPtr listPtr, int index, int count);

    /**
     * Tries to remove elements from a set using reflection. Returns false if the UProperty is not a set property.
     *
     * @param   const sfUPropertyInstance& uprop to try remove set elements from. Returns false if this is not a set
     *          property.
     * @param   int index of first element to remove.
     * @param   int count - number of elements to remove.
     * @return  bool true if the UProperty was a set property.
     */
    bool SetRemove(const sfUPropertyInstance& uprop, int index, int count);

    /**
     * Tries to insert elements from an sfListProperty into a map using reflection. Returns false if the UProperty is
     * not a map property.
     *
     * @param   const sfUPropertyInstance& uprop to try insert map elements for. Returns false if this is not a map
     *          property.
     * @param   sfListProperty::SPtr listPtr with elements to insert. Each element is another sfListProperty with two
                elements: a key followed by a value.
     * @param   int index of first element to insert, and the index to insert at.
     * @param   int count - number of elements to insert.
     * @return  bool true if the UProperty was a map property.
     */
    bool MapInsert(const sfUPropertyInstance& uprop, sfListProperty::SPtr listPtr, int index, int count);

    /**
     * Tries to remove elements from a map using reflection. Returns false if the UProperty is not a map property.
     *
     * @param   const sfUPropertyInstance& uprop to try remove map elements from. Returns false if this is not a map
     *          property.
     * @param   int index of first element to remove.
     * @param   int count - number of elements to remove.
     * @return  bool true if the UProperty was a map property.
     */
    bool MapRemove(const sfUPropertyInstance& uprop, int index, int count);

    /**
     * Creates map of unreal undo strings to undo types.
     */
    void RegisterUndoTypes();

    /**
     * Called when a transaction is undone.
     *
     * @param   FUndoSessionContext context
     * @param   bool success
     */
    void OnUndo(FUndoSessionContext context, bool success);

    /**
     * Called when a transaction is redone.
     *
     * @param   FUndoSessionContext context
     * @param   bool success
     */
    void OnRedo(FUndoSessionContext context, bool success);

    /**
     * Called when a transaction is undone or redone.
     *
     * @param   FUndoSessionContext context
     */
    void BeforeUndoRedo(FUndoSessionContext context);

    /**
     * Unreal can get in a bad state if another user changed the children of a component after the transaction was
     * recorded and we undo or redo the transaction, causing a component's child list to be incorrect. We call this
     * before a transaction to store the components of a transaction and their children in private member arrays we
     * can use to correct the bad state after the transaction. Unreal can also partially recreate actors in a
     * transaction that were deleted by another user, so this records the actors in the transaction that are deleted
     * so we can redelete them after the transaction.
     *
     * @param   const FTransaction* transactionPtr
     */
    void RecordPreTransactionState(const FTransaction* transactionPtr);

    /**
     * Checks for and corrects bad state in the child lists of components affected by a transaction.
     */
    void FixTransactedComponentChildren();

    /**
     * Destroys actors that were partially recreated by a transaction that should not have been recreated.
     */
    void DestroyUnwantedActors();

    /**
     * Called when a transaction in undone or redone. Sends changes made by the transaction to the server, or reverts
     * changed values to server values for locked objects.
     *
     * @param   FString action that was undone or redone.
     * @param   bool isUndo
     */
    void OnUndoRedo(FString action, bool isUndo);

    /**
     * Called for each actor affected by an undone or redone move or rotate transaction. Sends changes to the server,
     * or reverts to server values if the actor is locked.
     *
     * @param   AActor* actorPtr effected by the transaction.
     * @param   sfObject::SPtr objPtr for the actor. nullptr if the actor is not synced.
     * @param   sfDictionaryProperty::SPtr propertiesPtr for the actor. nullptr if the actor is not synced.
     * @param   bool isRotation - true if the transaction was for rotation. False for location.
     */
    void OnUndoRedoMove(
        AActor* actorPtr,
        sfObject::SPtr objPtr,
        sfDictionaryProperty::SPtr propertiesPtr,
        bool isRotation);

    /**
     * Called for each actor in an undo delete transaction, or redo create transaction. Recreates the actor on the
     * server, or deletes the actor if another actor with the same name already exists.
     *
     * @param   AActor* actorPtr
     */
    void OnUndoDelete(AActor* actorPtr);

    /**
     * Called on the parent actor in an undo detach transaction. Finds which child was reattached and reattaches it on
     * the server, or redetaches it if the child or parent are locked.
     *
     * @param   AActor* actorPtr - parent whose child was reattached during an undo detach transaction.
     */
    void OnUndoDetach(AActor* actorPtr);

    /**
     * Called on the parent actor in an redo detach transaction. Finds which child was redetached and redetaches it on
     * the server, or reattaches it if the child is locked.
     *
     * @param   AActor* actorPtr - parent whose child was redetached during a redo detach transaction.
     * @param   sfObject::SPtr objPtr for the actor.
     */
    void OnRedoDetach(AActor* actorPtr, sfObject::SPtr objPtr);

    /**
     * Sends a new scale value to the server, or reverts to the server value if the actor is locked.
     *
     * @param   AActor* actorPtr to sync scale for.
     * @param   sfObject::SPtr objPtr for the actor.
     * @param   sfDictionaryProperty::SPtr propertiesPtr for the actor.
     */
    void SyncScale(AActor* actorPtr, sfObject::SPtr objPtr, sfDictionaryProperty::SPtr propertiesPtr);

    /**
     * Sends new label and name values to the server, or reverts to the server values if the actor is locked.
     *
     * @param   AActor* actorPtr to sync label and name for.
     * @param   sfObject::SPtr objPtr for the actor.
     * @param   sfDictionaryProperty::SPtr propertiesPtr for the actor.
     */
    void SyncLabelAndName(AActor* actorPtr, sfObject::SPtr objPtr, sfDictionaryProperty::SPtr propertiesPtr);

    /**
     * Sends a new folder value to the server, or reverts to the server value if the actor is locked.
     *
     * @param   AActor* actorPtr to sync folder for.
     * @param   sfObject::SPtr objPtr for the actor.
     * @param   sfDictionaryProperty::SPtr propertiesPtr for the actor.
     */
    void SyncFolder(AActor* actorPtr, sfObject::SPtr objPtr, sfDictionaryProperty::SPtr propertiesPtr);

    /**
     * Sends a new parent value to the server, or reverts to the server value if the actor or new parent are locked.
     *
     * @param   AActor* actorPtr to sync parent for.
     * @param   sfObject::SPtr objPtr for the actor.
     */
    void SyncParent(AActor* actorPtr, sfObject::SPtr objPtr);

    /**
     * Creates actor objects on the server.
     *
     * @param   const TArray<AActor*>& actors to upload.
     */
    void UploadActors(const TArray<AActor*>& actors);

    /**
     * Recursively creates actor objects for an actor and its children.
     *
     * @param   AActor* actorPtr to create object for.
     * @return  sfObject::SPtr object for the actor.
     */
    sfObject::SPtr CreateObject(AActor* actorPtr);

    /**
     * Creates or finds an actor for an object and initializes it with server values. Recursively initializes child
     * actors for child objects.
     *
     * @param   sfObject::SPtr objPtr to initialize actor for.
     * @param   ULevel* levelPtr - the level that the actor for the given object belongs to
     * @return  AActor* actor for the object.
     */
    AActor* InitializeActor(sfObject::SPtr objPtr, ULevel* levelPtr);

    /**
     * Iterates a list of objects and their descendants, looking for child actors whose objects are not attached and
     * attaches those objects.
     *
     * @param   const std::list<sfObject::SPtr>& objects
     */
    void FindAndAttachChildren(const std::list<sfObject::SPtr>& objects);

    /**
     * Creates properties for syncing a static mesh actor's mesh and materials.
     *
     * @param   AActor* actorPtr
     * @param   sfDictionaryProperty::SPtr propertiesPtr
     * @return  bool false if the actor is not a static mesh actor.
     */
    bool CreateStaticMeshProperties(AActor* actorPtr, sfDictionaryProperty::SPtr propertiesPtr);

    /**
     * Applies mesh and material properties to a static mesh actor.
     *
     * @param   AActor* actorPtr
     * @param   sfDictionaryProperty::SPtr propertiesPtr
     * @return  bool false if the actor is not a static mesh actor.
     */
    bool ApplyStaticMeshProperties(AActor* actorPtr, sfDictionaryProperty::SPtr propertiesPtr);

    /**
     * Creates properties for syncing a skeletal mesh actor's mesh and materials.
     *
     * @param   AActor* actorPtr
     * @param   sfDictionaryProperty::SPtr propertiesPtr
     * @return  bool false if the actor is not a skeletal mesh actor.
     */
    bool CreateSkeletalMeshProperties(AActor* actorPtr, sfDictionaryProperty::SPtr propertiesPtr);

    /**
     * Applies mesh and material properties to a skeletal mesh actor.
     *
     * @param   AActor* actorPtr
     * @param   sfDictionaryProperty::SPtr propertiesPtr
     * @return  bool false if the actor is not a skeletal mesh actor.
     */
    bool ApplySkeletalMeshProperties(AActor* actorPtr, sfDictionaryProperty::SPtr propertiesPtr);

    /**
     * Creates a property for syncing an emitter's template.
     *
     * @param   AActor* actorPtr
     * @param   sfDictionaryProperty::SPtr propertiesPtr
     * @return  bool false if the actor is not an emitter.
     */
    bool CreateEmitterProperties(AActor* actorPtr, sfDictionaryProperty::SPtr propertiesPtr);

    /**
     * Applies the template property to an emitter.
     *
     * @param   AActor* actorPtr
     * @param   sfDictionaryProperty::SPtr propertiesPtr
     * @return  bool false if the actor is not an emitter.
     */
    bool ApplyEmitterProperties(AActor* actorPtr, sfDictionaryProperty::SPtr propertiesPtr);

    /**
     * Checks for and sends transform changes for an actor to the server.
     *
     * @param   AActor* actorPtr to send transform update for.
     * @param   sfObject::SPtr objPtr for the actor.
     */
    void SendTransformUpdate(AActor* actorPtr, sfObject::SPtr objPtr);

    /**
     * Applies server transform values to an actor.
     *
     * @param   AActor* actorPtr to apply transform to.
     * @param   sfObject::SPtr objPtr for the actor.
     */
    void ApplyServerTransform(AActor* actorPtr, sfObject::SPtr objPtr);

    /**
     * Registers property change handlers for server events.
     */
    void RegisterPropertyChangeHandlers();

    /**
     * Locks an actor.
     * 
     * @param   AActor* actorPtr
     * @param   sfUser::SPtr lockOwnerPtr. nullptr if the actor is indirectly locked by a descendant.
     */
    void Lock(AActor* actorPtr, sfUser::SPtr lockOwnerPtr);

    /**
     * Unlocks an actor.
     *
     * @param   AActor* actorPtr
     */
    void Unlock(AActor* actorPtr);

    /**
     * Gets the lock material for a user. Creates the material if it does not already exist.
     *
     * @param   sfUser::SPtr userPtr to get lock material for. May be nullptr.
     * @return  UMaterialInterface* lock material for the user.
     */
    UMaterialInterface* GetLockMaterial(sfUser::SPtr userPtr);

    /**
     * Called when a user's color changes.
     *
     * @param   sfUser::SPtr userPtr
     */
    void OnUserColorChange(sfUser::SPtr userPtr);

    /**
     * Called when a user disconnects.
     *
     * @param   sfUser::SPtr userPtr
     */
    void OnUserLeave(sfUser::SPtr userPtr);

    /**
     * Called when an actor is created by another user.
     *
     * @param   sfObject::SPtr objPtr that was created.
     * @param   int childIndex of new object. -1 if object is a root
     */
    virtual void OnCreate(sfObject::SPtr objPtr, int childIndex) override;

    /**
     * Called when an actor is deleted by another user.
     *
     * @param   sfObject::SPtr objPtr that was deleted.
     */
    virtual void OnDelete(sfObject::SPtr objPtr) override;

    /**
     * Called when an actor is locked by another user.
     *
     * @param   sfObject::SPtr objPtr that was locked.
     */
    virtual void OnLock(sfObject::SPtr objPtr) override;

    /**
     * Called when an actor is unlocked by another user.
     *
     * @param   sfObject::SPtr objPtr that was unlocked.
     */
    virtual void OnUnlock(sfObject::SPtr objPtr) override;

    /**
     * Called when an actor's lock owner changes.
     *
     * @param   sfObject::SPtr objPtr whose lock owner changed.
     */
    virtual void OnLockOwnerChange(sfObject::SPtr objPtr) override;

    /**
     * Called when an actor's parent is changed by another user.
     *
     * @param   sfObject::SPtr objPtr whose parent changed.
     * @param   int childIndex of the object. -1 if the object is a root.
     */
    virtual void OnParentChange(sfObject::SPtr objPtr, int childIndex) override;

    /**
     * Called when an actor property changes.
     *
     * @param   sfProperty::SPtr propertyPtr that changed.
     */
    virtual void OnPropertyChange(sfProperty::SPtr propertyPtr) override;

    /**
     * Called when a field is removed from a dictionary property.
     *
     * @param   sfDictionaryProperty::SPtr dictPtr the field was removed from.
     * @param   const sfName& name of removed field.
     */
    virtual void OnRemoveField(sfDictionaryProperty::SPtr dictPtr, const sfName& name) override;

    /**
     * Called when one or more elements are added to a list property.
     *
     * @param   sfListProperty::SPtr listPtr that elements were added to.
     * @param   int index elements were inserted at.
     * @param   int count - number of elements added.
     */
    virtual void OnListAdd(sfListProperty::SPtr listPtr, int index, int count) override;

    /**
     * Called when one or more elements are removed from a list property.
     *
     * @param   sfListProperty::SPtr listPtr that elements were removed from.
     * @param   int index elements were removed from.
     * @param   int count - number of elements removed.
     */
    virtual void OnListRemove(sfListProperty::SPtr listPtr, int index, int count) override;

    /**
     * Calls OnLockStateChange event handlers.
     *
     * @param   sfObject::SPtr objPtr whose lock state changed
     * @param   AActor* actorPtr
     */
    void InvokeOnLockStateChange(sfObject::SPtr objPtr, AActor* actorPtr);

    /**
     * Deletes all actors in the given level.
     *
     * @param   ULevel* levelPtr
     */
    void OnRemoveLevel(ULevel* levelPtr);

    /**
     * Calls OnCreate on every child of the given level sfObject. Destroys all unsynced actors after.
     *
     * @param   sfObject::SPtr sfLevelObjPtr
     * @param   ULevel* levelPtr
     */
    void OnSFLevelObjectCreate(sfObject::SPtr sfLevelObjPtr, ULevel* levelPtr);

    /**
     * Detaches the given actor from its parent if the given sfObject's parent is a level object and returns true.
     * Otherwise, returns false.
     *
     * @param   sfObject::SPtr objPtr
     * @param   AActor* actorPtr
     * @return  bool
     */
    bool DetachIfParentIsLevel(sfObject::SPtr objPtr, AActor* actorPtr);

    /**
     * Logs out an error that the given sfObject has no parent and then leaves the session.
     *
     * @param   sfObject::SPtr objPtr
     */
    void LogNoParentErrorAndDisconnect(sfObject::SPtr objPtr);
};