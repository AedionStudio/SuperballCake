#include "sfLockComponent.h"
#include "../SceneFusion.h"

UsfLockComponent::UsfLockComponent()
{
    bIsEditorOnly = true;// Prevents the component from saving and showing in the details panel
    bCopied = false;
    ClearFlags(EObjectFlags::RF_Transactional);// Prevent component from being recorded in transactions
}

UsfLockComponent::~UsfLockComponent()
{
    FTicker::GetCoreTicker().RemoveTicker(m_tickerHandle);
}

void UsfLockComponent::InitializeComponent()
{
    Super::InitializeComponent();
    AActor* actorPtr = GetOwner();
    if (actorPtr != nullptr)
    {
        actorPtr->bLockLocation = true;
    }
}

void UsfLockComponent::DuplicateParentMesh(UMaterialInterface* materialPtr)
{
    UMeshComponent* parentPtr = Cast<UMeshComponent>(GetAttachParent());
    if (parentPtr == nullptr)
    {
        return;
    }
    FString name = GetName();
    name.Append("Mesh");
    UMeshComponent* copyPtr = DuplicateObject(parentPtr, this, *name);
    copyPtr->CreationMethod = EComponentCreationMethod::Instance;
    copyPtr->bIsEditorOnly = true;
    copyPtr->SetRelativeLocation(FVector::ZeroVector);
    copyPtr->SetRelativeRotation(FQuat::Identity);
    copyPtr->SetRelativeScale3D(FVector::OneVector);
    for (int i = 0; i < copyPtr->GetNumMaterials(); i++)
    {
        copyPtr->SetMaterial(i, materialPtr);
    }
    copyPtr->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
    copyPtr->RegisterComponent();
    copyPtr->InitializeComponent();
    copyPtr->ClearFlags(EObjectFlags::RF_Transactional);// Prevent mesh from being recorded in transactions
}

void UsfLockComponent::SetMaterial(UMaterialInterface* materialPtr)
{
    for (USceneComponent* childPtr : GetAttachChildren())
    {
        UMeshComponent* meshPtr = Cast<UMeshComponent>(childPtr);
        if (meshPtr != nullptr)
        {
            for (int i = 0; i < meshPtr->GetNumMaterials(); i++)
            {
                meshPtr->SetMaterial(i, materialPtr);
            }
        }
    }
}

void UsfLockComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
    AActor* actorPtr = GetOwner();
    if (actorPtr != nullptr)
    {
        actorPtr->bLockLocation = false;
    }
    if (GetNumChildrenComponents() > 0 && !bDestroyingHierarchy)
    {
        for (int i = GetNumChildrenComponents() - 1; i >= 0; i--)
        {
            USceneComponent* childPtr = GetChildComponent(i);
            if (childPtr != nullptr)
            {
                childPtr->DestroyComponent();
            }
        }
    }
    Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UsfLockComponent::PostEditImport()
{
    // This is called twice when the object is duplicated, so we check if it was already called
    if (bCopied)
    {
        return;
    }
    bCopied = true;
    // We want to destroy this component and its child, but we have to wait a tick for the child to be created
    m_tickerHandle = FTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([this](float deltaTime)
    {
        DestroyComponent();
        return false;
    }), 1.0f / 60.0f);
}
