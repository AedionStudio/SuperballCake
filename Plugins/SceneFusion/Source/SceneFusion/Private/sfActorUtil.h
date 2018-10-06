#pragma once

#include <Log.h>
#include <CoreMinimal.h>
#include <GameFramework/Actor.h>
#include <Runtime/Engine/Classes/Components/SceneComponent.h>

#define LOG_CHANNEL "sfActorUtil"

/**
 * Actor utility functions
 */
class sfActorUtil
{
public:
    /**
     * Finds an actor with the given name in the current level.
     *
     * @param   const FString& name of actor to find.
     * @return  AActor* with the given name, or nullptr if none was found. The actor may be deleted.
     */
    static AActor* FindActorWithNameInCurrentLevel(const FString& name)
    {
        UWorld* worldPtr = GEditor->GetEditorWorldContext().World();
        if (worldPtr == nullptr)
        {
            return nullptr;
        }
        return Cast<AActor>(StaticFindObjectFast(AActor::StaticClass(), worldPtr->GetCurrentLevel(), FName(*name)));
    }

    /**
     * Finds an actor with the given name in the given level.
     *
     * @param   ULevel* levelPtr - level to find in
     * @param   const FString& name of actor to find.
     * @return  AActor* with the given name, or nullptr if none was found. The actor may be deleted.
     */
    static AActor* FindActorWithNameInLevel(ULevel* levelPtr, const FString& name)
    {
        if (levelPtr == nullptr)
        {
            return nullptr;
        }
        return Cast<AActor>(StaticFindObjectFast(AActor::StaticClass(), levelPtr, FName(*name)));
    }

    /**
     * Renames an actor. If the name is not available, appends random digits to the name until it finds a name that is
     * available.
     *
     * @param   AActor* actorPtr to rename.
     * @param   FString name to set. If this name is already used, random digits will be appended to the name.
     */
    static void Rename(AActor* actorPtr, FString name)
    {
        while (!actorPtr->Rename(*name, nullptr, REN_Test))
        {
            name += FString::FromInt(rand() % 10);
        }
        actorPtr->Rename(*name);
    }

    /**
     * Tries to rename an actor. Logs a warning if the actor could not be renamed because the name is already in use.
     * If a deleted actor is using the name, renames the deleted actor to make the name available.
     *
     * @param   AActor* actorPtr to rename.
     * @param   const FString& name
     */
    static void TryRename(AActor* actorPtr, const FString& name)
    {
        AActor* currentPtr = FindActorWithNameInLevel(actorPtr->GetLevel(), name);
        if (currentPtr == actorPtr)
        {
            return;
        }
        if (currentPtr != nullptr && currentPtr->IsPendingKill())
        {
            // Rename the deleted actor so we can reuse its name
            Rename(currentPtr, name + " (deleted)");
            currentPtr = nullptr;
        }

        if (currentPtr == nullptr && actorPtr->Rename(*name, nullptr, REN_Test))
        {
            actorPtr->Rename(*name);
        }
        else
        {
            KS::Log::Warning("Cannot rename actor to " + std::string(TCHAR_TO_UTF8(*name)) +
                " because another object with that name already exists.", LOG_CHANNEL);
        }
    }

    /**
     * Gets all scene components of type T belonging to an actor. This will find components that aren't in the actor's
     * OwnedComponents set, which will be missed by AActor->GetComponents<T>.
     *
     * @param   AActor* actorPtr to get components from.
     * @param   TArray<T*>& components array to add components to.
     */
    template<typename T>
    static void GetSceneComponents(AActor* actorPtr, TArray<T*>& components)
    {
        GetSceneComponents(actorPtr, actorPtr->GetRootComponent(), components);
    }

    /**
     * Finds scene components of type T belonging to an actor by searching a component and its descendants.
     *
     * @param   AActor* actorPtr to get components from.
     * @param   USceneComponent* componentPtr to start depth-first search at.
     * @param   TArray<T*>& components array to add components to.
     */
    template<typename T>
    static void GetSceneComponents(AActor* actorPtr, USceneComponent* componentPtr, TArray<T*>& components)
    {
        if (componentPtr == nullptr || componentPtr->GetOwner() != actorPtr)
        {
            return;
        }
        T* tPtr = Cast<T>(componentPtr);
        if (tPtr != nullptr)
        {
            components.Add(tPtr);
        }
        for (USceneComponent* childPtr : componentPtr->GetAttachChildren())
        {
            GetSceneComponents(actorPtr, childPtr, components);
        }
    }
};

#undef LOG_CHANNEL