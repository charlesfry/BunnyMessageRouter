// BunnyMessageRouter. Messages delivered at the speed of a startled rabbit.

#pragma once

#include "BunnyMessageSubsystem.h"
#include "Engine/CancellableAsyncAction.h"

#include "AsyncAction_ListenForBunnyMessage.generated.h"

#define UE_API BUNNYMESSAGERUNTIME_API

/**
 * ProxyObject is hidden by K2Node_AsyncAction_ListenForBunnyMessages and used for its follow-up GetPayload call.
 * ActualChannel is the channel broadcast on: Channel itself, or a child of it under PartialMatch.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAsyncBunnyMessageDelegate, UAsyncAction_ListenForBunnyMessage*, ProxyObject, FGameplayTag, ActualChannel);

UCLASS(MinimalAPI, BlueprintType, meta = (HasDedicatedAsyncNode))
class UAsyncAction_ListenForBunnyMessage : public UCancellableAsyncAction
{
	GENERATED_BODY()

public:
	/**
	 * Fires On Message Received for each broadcast on Channel until cancelled or the calling object is destroyed.
	 * The calling object is the listener's owner, so the log names it.
	 *
	 * @param PayloadType	The broadcast struct or a parent of it; leave empty to hear any payload.
	 */
	UFUNCTION(BlueprintCallable, Category = Messaging, meta = (WorldContext = "WorldContextObject", BlueprintInternalUseOnly = "true"))
	static UE_API UAsyncAction_ListenForBunnyMessage* ListenForBunnyMessages(UObject* WorldContextObject, FGameplayTag Channel, UScriptStruct* PayloadType, EBunnyMessageMatch MatchType = EBunnyMessageMatch::ExactMatch);

	/** Copies the payload being received into OutPayload, whose type must be PayloadType. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = Messaging, meta = (CustomStructureParam = "OutPayload"))
	UE_API bool GetPayload(UPARAM(ref) int32& OutPayload);
	DECLARE_FUNCTION(execGetPayload);

	UE_API virtual void Activate() override;
	UE_API virtual void SetReadyToDestroy() override;

	/** Called for each message received. The payload pin is filled from GetPayload. */
	UPROPERTY(BlueprintAssignable)
	FAsyncBunnyMessageDelegate OnMessageReceived;

private:
	void HandleMessageReceived(FGameplayTag Channel, const void* Payload);

	const void* ReceivedPayload = nullptr;
	TWeakObjectPtr<UObject> Owner;
	FGameplayTag Channel;
	TWeakObjectPtr<UScriptStruct> PayloadType;
	EBunnyMessageMatch Match = EBunnyMessageMatch::ExactMatch;
	FBunnyMessageListenerHandle Handle;
};

#undef UE_API
