#pragma once

#include "CoreMinimal.h"
#include "sfAvatarActor.h"
#include "sfBodyActor.generated.h"

/**
 * Actor class used for XR body avatar. It manages the body's transform to follow the head naturally.
 */
UCLASS()
class AsfBodyActor : public AsfAvatarActor
{
    GENERATED_BODY()

public:
    /**
     * Creates an AsfBodyActor and returns it as a AsfAvatarActor pointer.
     *
     * @param   const FVector& location
     * @param   const FRotator& rotation
     * @param   UStaticMesh* headMeshPtr
     * @param   UStaticMesh* hmdMeshPtr
     * @param   UStaticMesh* bodyMeshPtr
     * @param   UMaterialInstanceDynamic* materialPtr
     */
    static AsfAvatarActor* Create(
        const FVector& location,
        const FRotator& rotation,
        UStaticMesh* headMeshPtr,
        UStaticMesh* hmdMeshPtr,
        UStaticMesh* bodyMeshPtr,
        UMaterialInstanceDynamic* materialPtr);

    /**
     * Initializes AsfBodyActor. Sets meshes and materials.
     *
     * @param   UStaticMesh* headMeshPtr
     * @param   UStaticMesh* hmdMeshPtr
     * @param   UStaticMesh* bodyMeshPtr
     * @param   UMaterialInstanceDynamic* materialPtr
     */
    void Initialize(
        UStaticMesh* headMeshPtr,
        UStaticMesh* hmdMeshPtr,
        UStaticMesh* bodyMeshPtr,
        UMaterialInstanceDynamic* materialPtr);

    /**
     * Sets the head's rotation instantly to the specified rotation. Ajusts body's rotation to follow the head.
     *
     * @param   const FQuat& newRotation - the new rotation for the actor.
     */
    virtual void SetRotation(const FQuat& newRotation) override;

    /**
     * Sets the actor's scale instantly to the specified scale. Adjusts distance between head and body.
     *
     * @param   const FVector newScale - the new scale for the actor.
     */
    virtual void SetScale(const FVector& newScale) override;

private:
    UStaticMeshComponent* m_bodyComponentPtr;

    /**
     * Creates and adds a static mesh component to the actor.
     *
     * @param   const FName& name of static mesh component
     */
    UStaticMeshComponent* AddStaticMeshComponent(const FName& name);

    /**
     * Updates body's rotation.
     */
    void UpdateBodyRotation();

    /**
     * Updates body's location.
     */
    void UpdateBodyLocation();
};
