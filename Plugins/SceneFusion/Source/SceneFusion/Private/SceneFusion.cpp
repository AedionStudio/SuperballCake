#include "SceneFusion.h"
#include "Web/sfWebService.h"
#include "Web/sfMockWebService.h"
#include "ObjectManagers/sfAvatarManager.h"
#include "Testing/sfTestUtil.h"
#include "Consts.h"
#include "sfConfig.h"

#include <Runtime/Projects/Public/Interfaces/IPluginManager.h>
#include <Editor.h>
#include <Editor/PropertyEditor/Public/PropertyEditorModule.h>
#include <Runtime/Slate/Public/Widgets/Docking/SDockTab.h>

// Log setup
DEFINE_LOG_CATEGORY(LogSceneFusion)

#define LOG_CHANNEL "SceneFusion"

TSharedPtr<sfBaseWebService> SceneFusion::WebService = MakeShareable(new sfWebService());
sfService::SPtr SceneFusion::Service = nullptr;
IConsoleCommand* SceneFusion::m_mockWebServiceCommand = nullptr;
sfObjectEventDispatcher::SPtr SceneFusion::ObjectEventDispatcher = nullptr;
TSharedPtr<sfActorManager> SceneFusion::ActorManager = nullptr;
TSharedPtr<sfAvatarManager> SceneFusion::AvatarManager = nullptr;
TSharedPtr<sfUI> SceneFusion::m_sfUIPtr = nullptr;
bool SceneFusion::IsSessionCreator = false;
bool SceneFusion::m_redrawActiveViewport = false;

void SceneFusion::StartupModule()
{
    KS::Log::RegisterHandler("Root", HandleLog, KS::LogLevel::LOG_ALL, true);
    sfConfig::Get().Load();
    InitializeWebService();

    Service = sfService::Create();
    ObjectEventDispatcher = sfObjectEventDispatcher::CreateSPtr();
    m_levelManagerPtr = MakeShareable(new sfLevelManager);
    ObjectEventDispatcher->Register(sfType::Level, m_levelManagerPtr);
    ObjectEventDispatcher->Register(sfType::LevelLock, m_levelManagerPtr);
    ActorManager = MakeShareable(new sfActorManager(m_levelManagerPtr));
    ObjectEventDispatcher->Register(sfType::Actor, ActorManager);

    AvatarManager = MakeShareable(new sfAvatarManager);
    ObjectEventDispatcher->Register(sfType::Avatar, AvatarManager);

    if (FSlateApplication::IsInitialized())
    {
        m_sfUIPtr = MakeShareable(new sfUI);
        m_sfUIPtr->Initialize();
        m_sfUIPtr->OnGoToUser().BindRaw(AvatarManager.Get(), &sfAvatarManager::MoveViewportToUser);
        m_sfUIPtr->OnFollowUser().BindRaw(AvatarManager.Get(), &sfAvatarManager::Follow);
        AvatarManager->OnUnfollow.BindRaw(m_sfUIPtr.Get(), &sfUI::UnfollowCamera);

        RegisterEditableObjectPredicates();
    }

    sfTestUtil::RegisterCommands();

    // Register an FTickerDelegate to be called 60 times per second.
    m_updateHandle = FTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateRaw(this, &SceneFusion::Tick), 1.0f / 60.0f);
}

void SceneFusion::ShutdownModule()
{
    KS::Log::Info("Scene Fusion shut down module.", LOG_CHANNEL);

    m_sfUIPtr.Reset();
    sfTestUtil::CleanUp();
    m_sfUIPtr.Reset();
    if (FSlateApplication::IsInitialized())
    {
        UnregisterEditableObjectPredicates();
    }
    IConsoleManager::Get().UnregisterConsoleObject(m_mockWebServiceCommand);
    FTicker::GetCoreTicker().RemoveTicker(m_updateHandle);
}

void SceneFusion::OnConnect()
{
    ObjectEventDispatcher->Initialize();
}

void SceneFusion::OnDisconnect()
{
    ObjectEventDispatcher->CleanUp();
    SetDetailPanelEnabled(true);
}

bool SceneFusion::Tick(float deltaTime)
{
    Service->Update(deltaTime);
    if (Service->Session() != nullptr && Service->Session()->IsConnected())
    {
        if (m_levelManagerPtr.IsValid())
        {
            m_levelManagerPtr->Tick();
        }
        if (ActorManager.IsValid())
        {
            ActorManager->Tick(deltaTime);
        }
        if (AvatarManager.IsValid())
        {
            AvatarManager->Tick();
        }
    }

    // Redraw the active viewport
    if (m_redrawActiveViewport)
    {
        m_redrawActiveViewport = false;
        FViewport* viewport = GEditor->GetActiveViewport();
        if (viewport != nullptr)
        {
            viewport->Draw();
        }
    }
    return true;
}

void SceneFusion::HandleLog(KS::LogLevel level, const char* channel, const char* message)
{
    std::string str = "[" + KS::Log::GetLevelString(level) + ";" + channel + "] " + message;
    FString fstr(UTF8_TO_TCHAR(str.c_str()));
    switch (level)
    {
        case KS::LogLevel::LOG_DEBUG:
        {
            UE_LOG(LogSceneFusion, Log, TEXT("%s"), *fstr);
            break;
        }
        case KS::LogLevel::LOG_INFO:
        {
            UE_LOG(LogSceneFusion, Log, TEXT("%s"), *fstr);
            break;
        }
        case KS::LogLevel::LOG_WARNING:
        {
            UE_LOG(LogSceneFusion, Warning, TEXT("%s"), *fstr);
            break;
        }
        case KS::LogLevel::LOG_ERROR:
        {
            UE_LOG(LogSceneFusion, Error, TEXT("%s"), *fstr);
            break;
        }
        case KS::LogLevel::LOG_FATAL:
        {
            UE_LOG(LogSceneFusion, Fatal, TEXT("%s"), *fstr);
            break;
        }
    }
}

void SceneFusion::InitializeWebService()
{
    // Load Mock Web Service configs
    sfConfig& config = sfConfig::Get();
    if (!config.MockWebServerAddress.IsEmpty() && !config.MockWebServerPort.IsEmpty()) {
        KS::Log::Info("Mock Web Service enabled: " +
            std::string(TCHAR_TO_UTF8(*config.MockWebServerAddress)) + " " +
            std::string(TCHAR_TO_UTF8(*config.MockWebServerPort)), LOG_CHANNEL);
        WebService = MakeShareable(new sfMockWebService(config.MockWebServerAddress, config.MockWebServerPort));
    }

    // Register Mock Web Service Command
    m_mockWebServiceCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("SFMockWebService"),
        TEXT("Usage: SFMockWebService [host port]. If a host or port are ommitted then the mock web service will be disabled."),
        FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& args) {
        sfConfig& config = sfConfig::Get();
        if (args.Num() == 2) {
            KS::Log::Info("Mock Web Service enabled: " +
                std::string(TCHAR_TO_UTF8(*args[0])) + " " +
                std::string(TCHAR_TO_UTF8(*args[1])), LOG_CHANNEL);
            WebService = MakeShareable(new sfMockWebService(args[0], args[1]));
            config.MockWebServerAddress = args[0];
            config.MockWebServerPort = args[1];
        }
        else {
            KS::Log::Info("Mock Web Service disabled", LOG_CHANNEL);
            WebService = MakeShareable(new sfWebService());
            config.MockWebServerAddress.Empty();
            config.MockWebServerPort.Empty();
        }
        config.Save();
    })
    );
}

void SceneFusion::RedrawActiveViewport()
{
    m_redrawActiveViewport = true;
}

void SceneFusion::JoinSession(TSharedPtr<sfSessionInfo> sessionInfoPtr)
{
    m_sfUIPtr->JoinSession(sessionInfoPtr);
}

void SceneFusion::RegisterEditableObjectPredicates()
{
    FLevelEditorModule& module = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
    m_editableObjectPredicate.BindRaw(this, &SceneFusion::AreObjectsEditable);
    module.AddEditableObjectPredicate(m_editableObjectPredicate);
}

void SceneFusion::UnregisterEditableObjectPredicates()
{
    FLevelEditorModule& module = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
    module.RemoveEditableObjectPredicate(m_editableObjectPredicate.GetHandle());
}

bool SceneFusion::AreObjectsEditable(const TArray<TWeakObjectPtr<UObject>>& objects)
{
    bool editable = true;
    if (Service != nullptr && Service->Session() != nullptr && Service->Session()->IsConnected() &&
        ActorManager.IsValid() && objects.Num() > 0) {
        editable = ActorManager->CanEdit(objects);
        SetDetailPanelEnabled(editable);
    }
    return editable;
}

void SceneFusion::SetDetailPanelEnabled(bool enabled)
{
    //Disable name area text box and AddComponent button.
    FPropertyEditorModule& PropPlugin = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
    static const TArray<FName> DetailsTabIdentifiers =
    {
        "LevelEditorSelectionDetails",
        "LevelEditorSelectionDetails2",
        "LevelEditorSelectionDetails3",
        "LevelEditorSelectionDetails4"
    };
    static const TArray<FName> DiabledWidgetTypes =
    {
        "SComponentClassCombo",
        "SObjectNameEditableTextBox"
    };
    for (const FName& DetailsTabIdentifier : DetailsTabIdentifiers)
    {
        TSharedPtr<IDetailsView> DetailsView = PropPlugin.FindDetailView(DetailsTabIdentifier);
        if (DetailsView.IsValid())
        {
            TSharedPtr<FTabManager> tabManagerPtr = DetailsView->GetHostTabManager();
            if (tabManagerPtr.IsValid())
            {
                TSharedPtr<SDockTab> Tab = tabManagerPtr->FindExistingLiveTab(DetailsView->GetIdentifier());
                if (Tab.IsValid())
                {
                    SetEnabledRecursive(Tab->GetContent(), DiabledWidgetTypes, enabled);
                }
            }
        }
    }
}

void SceneFusion::SetEnabledRecursive(TSharedRef<SWidget> widget, TArray<FName> disabledWidgetTypes, bool enabled)
{
    if (disabledWidgetTypes.Contains(widget->GetType()))
    {
        widget->SetEnabled(enabled);
        return;
    }

    FChildren* children = widget->GetChildren();
    if (children != nullptr)
    {
        for (int32 i = 0; i < children->Num(); ++i)
        {
            SetEnabledRecursive(children->GetChildAt(i), disabledWidgetTypes, enabled);
        }
    }

}

// Module loading
IMPLEMENT_MODULE(SceneFusion, SceneFusion)

#undef LOG_CHANNEL