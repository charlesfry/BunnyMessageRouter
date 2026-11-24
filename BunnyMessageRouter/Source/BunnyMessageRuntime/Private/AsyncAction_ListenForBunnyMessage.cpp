// BunnyMessageRouter. Messages delivered at the speed of a startled rabbit.

#include "AsyncAction_ListenForBunnyMessage.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/Stack.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AsyncAction_ListenForBunnyMessage)

UAsyncAction_ListenForBunnyMessage* UAsyncAction_ListenForBunnyMessage::ListenForBunnyMessages(UObject* WorldContextObject, FGameplayTag Channel, UScriptStruct* PayloadType, EBunnyMessageMatch MatchType)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World)
	{
		return nullptr;
	}

	UAsyncAction_ListenForBunnyMessage* Action = NewObject<UAsyncAction_ListenForBunnyMessage>();
	Action->Owner = WorldContextObject;
	Action->Channel = Channel;
	Action->PayloadType = PayloadType;
	Action->Match = MatchType;
	Action->RegisterWithGameInstance(World);
	return Action;
}

void UAsyncAction_ListenForBunnyMessage::Activate()
{
	// The Blueprint that called the node owns the listener: the log names it, and the listener dies with it.
	TWeakObjectPtr<UAsyncAction_ListenForBunnyMessage> WeakThis(this);
	Handle = UBunnyMessageSubsystem::ListenInternal(Owner.Get(), Channel, PayloadType.Get(), Match,
		[WeakThis](FGameplayTag ActualChannel, const void* Payload)
		{
			if (UAsyncAction_ListenForBunnyMessage* StrongThis = WeakThis.Get())
			{
				StrongThis->HandleMessageReceived(ActualChannel, Payload);
			}
		});

	if (!Handle.IsValid())
	{
		SetReadyToDestroy();
	}
}

void UAsyncAction_ListenForBunnyMessage::SetReadyToDestroy()
{
	Handle.Unregister();
	Super::SetReadyToDestroy();
}

bool UAsyncAction_ListenForBunnyMessage::GetPayload(int32& OutPayload)
{
	checkNoEntry();
	return false;
}

DEFINE_FUNCTION(UAsyncAction_ListenForBunnyMessage::execGetPayload)
{
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	void* MessagePtr = Stack.MostRecentPropertyAddress;
	FStructProperty* StructProp = CastField<FStructProperty>(Stack.MostRecentProperty);
	P_FINISH;

	bool bSuccess = false;
	if (StructProp && StructProp->Struct && MessagePtr && StructProp->Struct == P_THIS->PayloadType.Get() && P_THIS->ReceivedPayload)
	{
		StructProp->Struct->CopyScriptStruct(MessagePtr, P_THIS->ReceivedPayload);
		bSuccess = true;
	}
	*(bool*)RESULT_PARAM = bSuccess;
}

void UAsyncAction_ListenForBunnyMessage::HandleMessageReceived(FGameplayTag ActualChannel, const void* Payload)
{
	ReceivedPayload = Payload;
	OnMessageReceived.Broadcast(this, ActualChannel);
	ReceivedPayload = nullptr;

	if (!OnMessageReceived.IsBound())
	{
		// The Blueprint that bound the node is gone.
		SetReadyToDestroy();
	}
}
