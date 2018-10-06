#pragma once

#include <CoreMinimal.h>
#include <Runtime/Engine/Classes/Engine/Level.h>

#include <map>
#include <unordered_set>

#include "IObjectManager.h"

using namespace KS::SceneFusion2;
using namespace KS;

/**
 * Manages level syncing. Level relationship is not maintained.
 */
class sfLevelManager : public IObjectManager
{
public:
    /**
     * Constructor
     */
    sfLevelManager();

    /**
     * Destructor
     */
    virtual ~sfLevelManager();

    /**
     * Initialization. Called after connecting to a session.
     */
    virtual void Initialize();

    /**
     * Deinitialization. Called after disconnecting from a session.
     */
    virtual void CleanUp();

    /**
     * Updates the level manager.
     */
    void Tick();

    /**
     * Called when a level sfObject is created by another user. If the level is a temp level, create a temp level.
     * If the level is loaded from saved file, try to load the level. If the level file could not be found, disconnect.
     *
     * @param   sfObject::SPtr objPtr that was created.
     * @param   int childIndex of new object. -1 if object is a root
     */
    virtual void OnCreate(sfObject::SPtr objPtr, int childIndex) override;

    /**
     * Called when a level is deleted by another user. Unloads the level.
     *
     * @param   sfObject::SPtr objPtr that was deleted.
     */
    virtual void OnDelete(sfObject::SPtr objPtr) override;

    /**
     * Called when a level property changes. Sets the property on the level.
     *
     * @param   sfProperty::SPtr propertyPtr that changed.
     */
    virtual void OnPropertyChange(sfProperty::SPtr propertyPtr) override;

    /**
     * When we acquire a lock on the level lock object, upload new levels.
     *
     * @param   sfObject::SPtr objPtr
     */
    virtual void OnDirectLockChange(sfObject::SPtr objPtr);

    /**
     * Gets sfObject by the given ULevel. If could not find the sfObject, creates one.
     *
     * @param   ULevel* levelPtr
     * @return  sfObject::SPtr
     */
    sfObject::SPtr GetOrCreateLevelObject(ULevel* levelPtr);

    /**
     * Gets ULevel by the given sfObject. If could not find the ULevel, returns nullptr.
     *
     * @param   sfObject::SPtr levelObjectPtr
     * @return  ULevel*
     */
    ULevel* FindLevelByObject(sfObject::SPtr levelObjectPtr);

private:
    typedef std::function<void()> Callback;

    bool m_initialized;

    /**
     * Sets the property on the given streaming level.
     *
     * @param   ULevelStreaming* streaming level to set property on.
     * @param   sfProperty::SPtr new property
     */
    typedef std::function<void(ULevelStreaming*, sfProperty::SPtr)> PropertyChangeHandler;

    sfSession::SPtr m_sessionPtr;
    bool m_destroyUnsyncedLevels;
    UWorld* m_worldPtr;

    TMap<ULevel*, sfObject::SPtr> m_levelToObjectMap;
    std::map<sfObject::SPtr, ULevel*> m_objectToLevelMap;

    std::unordered_set<ULevel*> m_movedLevels;
    TSet<ULevelStreaming*> m_dirtyStreamingLevels;
    std::unordered_set<sfObject::SPtr> m_levelsNeedToBeLoaded;
    std::unordered_set<ULevel*> m_levelsToUpload;

    sfObject::SPtr m_lockObject;

    FDelegateHandle m_onAddLevelToWorldHandle;
    FDelegateHandle m_onPrepareToCleanseEditorObjectHandle;
    FDelegateHandle m_onObjectModifiedHandle;
    FDelegateHandle m_onUndoHandle;
    FDelegateHandle m_onRedoHandle;
    TMap<ULevel*, FDelegateHandle> m_onLevelTransformChangeHandles;

    std::unordered_map<sfName, PropertyChangeHandler> m_propertyChangeHandlers;

    /**
     * Tries to find level in all loaded levels. If found, returns level pointer. Otherwise, returns nullptr.
     *
     * @param   FString levelPath
     * @param   bool isPersistentLevel
     * @return  ULevel*
     */
    ULevel* FindLevelInLoadedLevels(FString levelPath, bool isPersistentLevel);

    /**
     * Tries to load level from file and return level pointer.
     *
     * @param   FString levelPath
     * @param   bool isPersistentLevel
     * @return  ULevel*
     */
    ULevel* TryLoadLevelFromFile(FString levelPath, bool isPersistentLevel);

    /**
     * Creates map file for level and returns level pointer.
     *
     * @param   FString levelPath
     * @param   bool isPersistentLevel
     * @return  ULevel*
     */
    ULevel* CreateMap(FString levelPath, bool isPersistentLevel);

    /**
     * Called when a level is added to the world. Uploads the new level.
     *
     * @param   ULevel* newLevelPtr
     */
    void OnAddLevelToWorld(ULevel* newLevelPtr);

    /**
     * Called when the editor is about to cleanse an object that must be purged,
     * such as when changing the active map or level. If the object is a world object, disconnect.
     * If it is a level object, delete the sfObject on the server. Clears raw pointers of actors in
     * the level from our containers.
     *
     * @param   UObject* uobjPtr - object to be purged
     */
    void OnPrepareToCleanseEditorObject(UObject* uobjPtr);

    /**
     * Called when an object is modified. Sends streaming level changes to server.
     *
     * @param   UObject* uobjPtr - modified object
     */
    void OnObjectModified(UObject* uobjPtr);

    /**
     * Destroys levels that don't exist on the server.
     */
    void DestroyUnsyncedLevels();

    /**
     * Registers property change handlers for server events.
     */
    void RegisterPropertyChangeHandlers();

    /**
     * Checks for and sends transform changes for a level to the server.
     *
     * @param   ULevel* levelPtr to send transform update for.
     */
    void SendTransformUpdate(ULevel* levelPtr);

    /**
     * Sends a new folder value to the server.
     *
     * @param   ULevelStreaming* streamingLevelPtr to send folder change for.
     */
    void SendFolderChange(ULevelStreaming* streamingLevelPtr);

    /**
     * Called when a transaction in undone or redone. Sends changes made by the transaction.
     *
     * @param   FUndoSessionContext context
     * @param   bool success
     */
    void OnUndoRedo(FUndoSessionContext context, bool success);

    /**
     * Modifies a ULevel. Removes event handlers before and adds event handlers back after.
     * Prevents any changes to the undo stack during the call.
     *
     * @param   ULevel* levelPtr to modify
     * @param   Callback callback to modify the given level
     */
    void ModifyLevelWithoutTriggerEvent(ULevel* levelPtr, Callback callback);

    /**
     * Uploads the given level.
     *
     * @param   ULevel* levelPtr
     */
    void UploadLevel(ULevel* levelPtr);
    
    /**
     * Reuqests lock for uploading levels.
     */
    void RequestLock();
};