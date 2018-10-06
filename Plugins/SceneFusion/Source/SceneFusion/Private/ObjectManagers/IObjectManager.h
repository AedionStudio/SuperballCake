#pragma once
#include <sfObject.h>

using namespace KS::SceneFusion2;

/**
 * Interface for handling object events.
 */
class IObjectManager
{
public:
    friend class sfObjectEventDispatcher;

    /**
     * Constructor
     */
    IObjectManager() {}

    /**
     * Destructor
     */
    virtual ~IObjectManager() {}

private:
    /**
     * Initialization. Called after connecting to a session.
     */
    virtual void Initialize() {}

    /**
     * Deinitialization. Called after disconnecting from a session.
     */
    virtual void CleanUp() {}

    /**
     * Called when an object is created by another user.
     *
     * @param   sfObject::SPtr objPtr that was created.
     * @param   int childIndex of new object. -1 if object is a root.
     */
    virtual void OnCreate(sfObject::SPtr objPtr, int childIndex) {}

    /**
     * Called when an object is deleted by another user.
     *
     * @param   sfObject::SPtr objPtr that was deleted.
     */
    virtual void OnDelete(sfObject::SPtr objPtr) {}

    /**
     * Called when an object is locked by another user.
     *
     * @param   sfObject::SPtr objPtr that was locked.
     */
    virtual void OnLock(sfObject::SPtr objPtr) {}

    /**
     * Called when an object is unlocked by another user.
     *
     * @param   sfObject::SPtr objPtr that was unlocked.
     */
    virtual void OnUnlock(sfObject::SPtr objPtr) {}

    /**
     * Called when an object's lock owner changes.
     *
     * @param   sfObject::SPtr objPtr whose lock owner changed.
     */
    virtual void OnLockOwnerChange(sfObject::SPtr objPtr) {}

    /**
     * Called when an object's direct lock owner changes.
     *
     * @param   sfObject::SPtr objPtr whose direct lock owner changed.
     */
    virtual void OnDirectLockChange(sfObject::SPtr objPtr) {}

    /**
     * Called when an object's parent is changed by another user.
     *
     * @param   sfObject::SPtr objPtr whose parent changed.
     * @param   int childIndex of the object. -1 if the object is a root.
     */
    virtual void OnParentChange(sfObject::SPtr objPtr, int childIndex) {}

    /**
     * Called when an object property changes.
     *
     * @param   sfProperty::SPtr propertyPtr that changed.
     */
    virtual void OnPropertyChange(sfProperty::SPtr propertyPtr) {}

    /**
     * Called when a field is removed from a dictionary property.
     *
     * @param   sfDictionaryProperty::SPtr dictPtr the field was removed from.
     * @param   const sfName& name of removed field.
     */
    virtual void OnRemoveField(sfDictionaryProperty::SPtr dictPtr, const sfName& name) {}

    /**
     * Called when one or more elements are added to a list property.
     *
     * @param   sfListProperty::SPtr listPtr that elements were added to.
     * @param   int index elements were inserted at.
     * @param   int count - number of elements added.
     */
    virtual void OnListAdd(sfListProperty::SPtr listPtr, int index, int count) {}

    /**
     * Called when one or more elements are removed from a list property.
     *
     * @param   sfListProperty::SPtr listPtr that elements were removed from.
     * @param   int index elements were removed from.
     * @param   int count - number of elements removed.
     */
    virtual void OnListRemove(sfListProperty::SPtr listPtr, int index, int count) {}
};