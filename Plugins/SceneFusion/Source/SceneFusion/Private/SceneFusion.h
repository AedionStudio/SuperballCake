#pragma once

#include "ISceneFusion.h"
#include "UI/sfUI.h"
#include "Web/sfBaseWebService.h"
#include "Log.h"
#include "sfService.h"
#include "sfObjectEventDispatcher.h"
#include "ObjectManagers/sfActorManager.h"
#include "ObjectManagers/sfAvatarManager.h"
#include "ObjectManagers/sfLevelManager.h"

#include <LevelEditor.h>
#include <CoreMinimal.h>

// Log setup
DECLARE_LOG_CATEGORY_EXTERN(LogSceneFusion, Log, All)

/**
 * Scene Fusion Plugin Module
 */
class SceneFusion : public ISceneFusion
{
public:
    static TSharedPtr<sfBaseWebService> WebService;
    static sfService::SPtr Service;
    static sfObjectEventDispatcher::SPtr ObjectEventDispatcher;
    static TSharedPtr<sfActorManager> ActorManager;
    static TSharedPtr<sfAvatarManager> AvatarManager;
    static bool IsSessionCreator;

    /**
     * Module entry point
     */
    void StartupModule();
    
    /**
     * Module cleanup
     */
    void ShutdownModule();

    /**
     * Updates the service
     *
     * @param   float deltaTime since the last tick
     * @return  bool true to keep the Tick function registered
     */
    bool Tick(float deltaTime);

    /**
     * Initialize the webservice and associated console commands
     */
    void InitializeWebService();

    /**
     * Writes a log message to Unreal's log system.
     *
     * @param   KS::LogLevel level
     * @param   const char* channel that was logged to.
     * @param   const char* message
     */
    static void HandleLog(KS::LogLevel level, const char* channel, const char* message);

    /**
     * Flags the active viewport to be redrawn during the next update SceneFusion tick.
     */
    static void RedrawActiveViewport();
    
    /**
     * Connects to a session.
     *
     * @param   TSharedPtr<sfSessionInfo> sessionInfoPtr - determines where to connect.
     */
    static void JoinSession(TSharedPtr<sfSessionInfo> sessionInfoPtr);

    /**
     * Called after connecting to a session.
     */
    static void OnConnect();

    /**
     * Called after disconnecting from a session.
     */
    static void OnDisconnect();

private:
    static IConsoleCommand* m_mockWebServiceCommand;
    static bool m_redrawActiveViewport;
    static TSharedPtr<sfUI> m_sfUIPtr;

    FDelegateHandle m_updateHandle;
    FAreObjectsEditable m_editableObjectPredicate;
    TSharedPtr<sfLevelManager> m_levelManagerPtr;
    
    /**
     * Register selection predicate for detail panel.
     */
    void RegisterEditableObjectPredicates();

    /**
     * Unregister selection predicate for detail panel.
     */
    void UnregisterEditableObjectPredicates();

    /**
     * Check if a selection of UObjects is editable.
     */
    bool AreObjectsEditable(const TArray<TWeakObjectPtr<UObject>>& objects);

    /**
     * Set detail panel's name area and AddComponent button enabled flag.
     *
     * @param   bool enable
     */
    static void SetDetailPanelEnabled(bool enabled);

    /**
     * Recursively iterate through given widget and its descendants.
     * If the type is in the given types, set its enabled flag as given.
     *
     * @param   TSharedRef<SWidget> widget - widget to iterate
     * @param   TArray<FName> disabledWidgetTypes - array of widget types to set enable flag on
     * @param   bool enabled
     */
    static void SetEnabledRecursive(TSharedRef<SWidget> widget, TArray<FName> disabledWidgetTypes, bool enabled);
};