#pragma once

#include "ObjectManagers/IObjectManager.h"

#include <sfObject.h>
#include <unordered_map>

using namespace KS;

/**
 * The object event dispatcher listens for object events and calls the corresponding functions on the object manager
 * registered for the object's type.
 */
class sfObjectEventDispatcher
{
public:
    typedef std::shared_ptr<sfObjectEventDispatcher> SPtr;

    /**
     * Static shared pointer constructor.
     *
     * @return  SPtr
     */
    static SPtr CreateSPtr();

    /**
     * Constructor
     */
    sfObjectEventDispatcher();

    /**
     * Destructor
     */
    ~sfObjectEventDispatcher();

    /**
     * Registers an object manager to handle events for a given object type.
     *
     * @param   const sfName& objectType the manager should handle events for.
     * @param   TSharedPtr<IObjectManager> managerPtr to register.
     */
    void Register(const sfName& objectType, TSharedPtr<IObjectManager> managerPtr);

    /**
     * Starts listening for events and calls Initialize on all registered managers.
     */
    void Initialize();

    /**
     * Stops listening for events and calls CleanUp on all registered managers.
     */
    void CleanUp();

private:
    bool m_active;
    std::unordered_map<sfName, TSharedPtr<IObjectManager>> m_managers;
    ksEvent<sfObject::SPtr&, int&>::SPtr m_createEventPtr;
    ksEvent<sfObject::SPtr&>::SPtr m_deleteEventPtr;
    ksEvent<sfObject::SPtr&>::SPtr m_lockEventPtr;
    ksEvent<sfObject::SPtr&>::SPtr m_unlockEventPtr;
    ksEvent<sfObject::SPtr&>::SPtr m_lockOwnerChangeEventPtr;
    ksEvent<sfObject::SPtr&>::SPtr m_directLockChangeEventPtr;
    ksEvent<sfObject::SPtr&, int&>::SPtr m_parentChangeEventPtr;
    ksEvent<sfProperty::SPtr&>::SPtr m_propertyChangeEventPtr;
    ksEvent<sfDictionaryProperty::SPtr&, sfName&>::SPtr m_removeFieldEventPtr;
    ksEvent<sfListProperty::SPtr&, int&, int&>::SPtr m_listAddEventPtr;
    ksEvent<sfListProperty::SPtr&, int&, int&>::SPtr m_listRemoveEventPtr;

    /**
     * Gets the object manager for an object.
     *
     * @param   sfObject::SPtr objPtr to get manager for.
     * @return  sfObject::SPtr manager for the object, or nullptr if there is no manager for the object's type.
     */
    TSharedPtr<IObjectManager> GetManager(sfObject::SPtr objPtr);
};