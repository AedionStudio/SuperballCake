#include "sfOutlinerManager.h"

#include <Editor.h>
#include <Editor/SceneOutliner/Public/SceneOutlinerModule.h>
#include <Editor/LevelEditor/Public/LevelEditor.h>
#include <Widgets/Docking/SDockTab.h>

#include "sfUIStyles.h"
#include "sfLockColumn.h"

#define SCENE_OUTLINER_MODULE "SceneOutliner"
#define LEVEL_EDITOR "LevelEditor"
#define WORLD_OUTLINER "LevelEditorSceneOutliner"

sfOutlinerManager::sfOutlinerManager() :
    m_tabManager { nullptr }
{

}

sfOutlinerManager::~sfOutlinerManager()
{

}

void sfOutlinerManager::Initialize()
{
    m_onActorDeletedHandle = GEngine->OnLevelActorDeleted().AddRaw(this, &sfOutlinerManager::OnActorDeleted);
    if (!m_tabManager.IsValid())
    {
        FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(LEVEL_EDITOR);
        m_tabManager = LevelEditorModule.GetLevelEditorTabManager();
    }

    // Register scene fusion lock icon column
    FSceneOutlinerModule& SceneOutlinerModule
        = FModuleManager::LoadModuleChecked<FSceneOutlinerModule>(SCENE_OUTLINER_MODULE);
    SceneOutliner::FColumnInfo ColumnInfo(SceneOutliner::EColumnVisibility::Visible, 15,
        FCreateSceneOutlinerColumn::CreateRaw(this, &sfOutlinerManager::CreateLockColumn));
    SceneOutlinerModule.RegisterDefaultColumnType<FsfLockColumn>(SceneOutliner::FDefaultColumnInfo(ColumnInfo));
    //Reconstruct the world outliner tab to show our column.
    ReconstructWorldOutliner();
}

void sfOutlinerManager::CleanUp()
{
    // Unregister scene fusion lock icon column
    FSceneOutlinerModule& SceneOutlinerModule
        = FModuleManager::LoadModuleChecked<FSceneOutlinerModule>(SCENE_OUTLINER_MODULE);
    SceneOutlinerModule.UnRegisterColumnType<FsfLockColumn>();

    //Reconstruct the world outliner tab to remove our column.
    ReconstructWorldOutliner();

    GEngine->OnLevelActorDeleted().Remove(m_onActorDeletedHandle);
    m_actorLockInfos.Empty();
}

TSharedRef<ISceneOutlinerColumn> sfOutlinerManager::CreateLockColumn(ISceneOutliner& SceneOutliner)
{
    TSharedRef<sfOutlinerManager> outlinerManagerPtr = AsShared();
    return MakeShareable(new FsfLockColumn(outlinerManagerPtr));
}

void sfOutlinerManager::ReconstructWorldOutliner()
{
    if (m_tabManager.IsValid())
    {
        TSharedPtr<SDockTab> worldOutlinerTab = m_tabManager->FindExistingLiveTab(FName(WORLD_OUTLINER));
        if (worldOutlinerTab.IsValid())
        {
            worldOutlinerTab->RequestCloseTab();
            FTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float Delta) {
                if (m_tabManager.IsValid())
                {
                    m_tabManager->InvokeTab(FName(WORLD_OUTLINER));
                }
                return false;
            }));
        }
    }
}

void sfOutlinerManager::SetLockState(AActor* actorPtr, sfActorManager::LockType lockType, sfUser::SPtr lockOwner)
{
    TSharedPtr<sfLockInfo> lockInfoPtr = FindOrAddLockInfo(actorPtr);
    lockInfoPtr->LockType = lockType;
    lockInfoPtr->LockOwner = lockOwner;
}

void sfOutlinerManager::OnActorDeleted(AActor* actorPtr)
{
    m_actorLockInfos.Remove(actorPtr);
}

const TSharedRef<SWidget> sfOutlinerManager::ConstructRowWidget(AActor* actorPtr)
{
    return StaticCastSharedPtr<SWidget>(FindOrAddLockInfo(actorPtr)->Icon).ToSharedRef();
}

TSharedPtr<sfLockInfo> sfOutlinerManager::FindOrAddLockInfo(AActor* actorPtr)
{
    TSharedPtr<sfLockInfo> lockInfoPtr = m_actorLockInfos.FindRef(actorPtr);
    if (!lockInfoPtr.IsValid())
    {
        lockInfoPtr = MakeShareable(new sfLockInfo);
        m_actorLockInfos.Add(actorPtr, lockInfoPtr);
    }
    return lockInfoPtr;
}

#undef SCENE_OUTLINER_MODULE
#undef LEVEL_EDITOR
#undef WORLD_OUTLINER