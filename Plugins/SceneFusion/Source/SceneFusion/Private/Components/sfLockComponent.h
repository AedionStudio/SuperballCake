#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Classes/Components/MeshComponent.h"
#include "sfLockComponent.generated.h"


/**
 * Lock component for indicating an actor cannot be edited. This is added to each mesh component of the actor, and
 * adds a copy of the mesh as a child with a lock shader. It also deletes itself and unlocks the actor when copied.
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class UsfLockComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    bool bCopied;

    /**
     * Constructor
     */
    UsfLockComponent();

    /**
     * Destructor
     */
    virtual ~UsfLockComponent();

    /**
     * Initialization
     */
    virtual void InitializeComponent() override;

    /**
     * Called after being duplicated. Destroys this component, its children if any, and unlocks the actor.
     */
    virtual void PostEditImport() override;

    /**
     * Called when the component is destroyed. Destroys children components.
     *
     * @param   bool bDestroyingHierarchy
     */
    virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

    /**
     * Duplicates the parent mesh component and adds the duplicate as a child.
     *
     * @param   UMaterialInterface* materialPtr to use on the duplicate mesh.
     */
    void DuplicateParentMesh(UMaterialInterface* materialPtr);

    /**
     * Sets the material of all child meshes.
     *
     * @param   UMaterialInterface* materialPtr
     */
    void SetMaterial(UMaterialInterface* materialPtr);

private:
    FDelegateHandle m_tickerHandle;
};
