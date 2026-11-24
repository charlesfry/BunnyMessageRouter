// BunnyMessageRouter. Messages delivered at the speed of a startled rabbit.

#pragma once

#include "GameplayTagContainer.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/SharedPointer.h"

#include "BunnyMessageSubsystem.generated.h"

#define UE_API BUNNYMESSAGERUNTIME_API

BUNNYMESSAGERUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogBunnyMessages, Log, All);

class UBunnyMessageSubsystem;

UENUM(BlueprintType)
enum class EBunnyMessageMatch : uint8
{
	// Listening on A.B receives A.B only.
	ExactMatch,
	// Listening on A.B receives A.B and A.B.C.
	PartialMatch
};

/** Removes one listener. Unregister() is safe to call twice, or after the subsystem is gone. */
USTRUCT(BlueprintType)
struct FBunnyMessageListenerHandle
{
	GENERATED_BODY()

	UE_API void Unregister();
	bool IsValid() const { return ID != 0; }

private:
	friend UBunnyMessageSubsystem;
	TWeakObjectPtr<UBunnyMessageSubsystem> Subsystem;
	FGameplayTag Channel;
	int32 ID = 0;
};

/**
 * Messages between objects that don't know about each other. Sender and listener agree only on a channel tag and a
 * USTRUCT payload; a listener may take the broadcast struct or any parent of it.
 *
 *   UBunnyMessageSubsystem::Broadcast(this, TAG_Door_Open, FDoorMessage{ ... });
 *   UBunnyMessageSubsystem::Listen(this, TAG_Door_Open, &UAlarm::HandleDoorOpen);
 *   UBunnyMessageSubsystem::UnregisterAll(this);   // in EndPlay
 *
 * Every listener has an owner. The owner is the world context, the name in the log, and the lifetime: a listener
 * whose owner has been destroyed is never called and is removed.
 *
 * Call order is guaranteed: listeners on the broadcast channel first, then on each parent channel in turn
 * (PartialMatch listeners only); within one channel, in the order they were registered. A listener registered during
 * a broadcast first hears the next one; a listener removed during a broadcast is not called by it.
 *
 * Debugging. Everything goes to LogBunnyMessages. Each broadcast gets a number, its reactions share it, and a
 * broadcast made inside a reaction is indented under it, so a chain of messages reads as a tree:
 *   [12] BROADCAST Trigger.Door.Open from BP_Door_C_1 (Room=(TagName="Area.Hall"))
 *   [12]   REACT Trigger.Door.Open -> BP_Alarm_C_0
 *   [13]     BROADCAST Event.Alarm from BP_Alarm_C_0 ()
 *   [13]       REACT Event (partial) -> RSNightDirector_0
 *   [14] BROADCAST Trigger.Door.Shut from BP_Door_C_1 ()
 *   [14]   NOBODY LISTENING (1 ExactMatch listener on a parent channel: Trigger.Door -> BP_Hud_C_0)
 * LISTEN and UNLISTEN lines show when each listener came and went. Mistakes are warnings or errors: invalid
 * channels, payload type mismatches, no subsystem, broadcasts off the game thread, loops deeper than 16.
 * The console command BunnyMessages.Dump [Tag] lists the live listeners.
 * Quieter: "log LogBunnyMessages Warning". Louder (removals of dead owners): "log LogBunnyMessages Verbose".
 */
UCLASS(MinimalAPI)
class UBunnyMessageSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** The subsystem of WorldContext's game instance, or null. Never asserts. */
	static UE_API UBunnyMessageSubsystem* Find(const UObject* WorldContext);

	/** Broadcasts Payload on Channel. Sender is the world context and the name in the log. */
	template <typename TPayload>
	static void Broadcast(const UObject* Sender, FGameplayTag Channel, const TPayload& Payload)
	{
		BroadcastInternal(Sender, Channel, TBaseStructure<TPayload>::Get(), &Payload);
	}

	/** Calls Callback(Channel, Payload) for each broadcast on Channel while Owner lives. */
	template <typename TPayload>
	static FBunnyMessageListenerHandle Listen(const UObject* Owner, FGameplayTag Channel,
		TFunction<void(FGameplayTag, const TPayload&)>&& Callback, EBunnyMessageMatch Match = EBunnyMessageMatch::ExactMatch)
	{
		return ListenInternal(Owner, Channel, TBaseStructure<TPayload>::Get(), Match,
			[Callback = MoveTemp(Callback)](FGameplayTag Ch, const void* Payload) { Callback(Ch, *static_cast<const TPayload*>(Payload)); });
	}

	/** Member-function form: Listen(this, Channel, &UMyThing::HandleX). */
	template <typename TPayload, typename TOwner>
	static FBunnyMessageListenerHandle Listen(TOwner* Owner, FGameplayTag Channel,
		void (TOwner::*Function)(FGameplayTag, const TPayload&), EBunnyMessageMatch Match = EBunnyMessageMatch::ExactMatch)
	{
		// Owner is only dereferenced while alive: the subsystem checks before every call.
		return Listen<TPayload>(Owner, Channel, [Owner, Function](FGameplayTag Ch, const TPayload& Payload) { (Owner->*Function)(Ch, Payload); }, Match);
	}

	/** Calls Callback(Channel) for each broadcast on Channel, whatever its payload. */
	static UE_API FBunnyMessageListenerHandle ListenForTag(const UObject* Owner, FGameplayTag Channel,
		TFunction<void(FGameplayTag)>&& Callback, EBunnyMessageMatch Match = EBunnyMessageMatch::ExactMatch);

	/** Removes every listener Owner registered. Call it in EndPlay. */
	static UE_API void UnregisterAll(const UObject* Owner);

	/** How the log names an object: actor label, Actor.Component, Actor.Object for objects inside one, else the name. */
	static UE_API FString Describe(const UObject* Object);

	/** Writes the live listeners to the log, optionally only channels matching Filter. Backs BunnyMessages.Dump. */
	UE_API void Dump(FGameplayTag Filter = FGameplayTag()) const;

	UE_API virtual void Deinitialize() override;

	/** Broadcasts Message on Channel. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = Messaging, meta = (WorldContext = "Sender", CustomStructureParam = "Message", AllowAbstract = "false", DisplayName = "Broadcast Message"))
	static UE_API void K2_BroadcastMessage(const UObject* Sender, FGameplayTag Channel, const int32& Message);
	DECLARE_FUNCTION(execK2_BroadcastMessage);

private:
	friend struct FBunnyMessageListenerHandle;
	friend class UAsyncAction_ListenForBunnyMessage;

	using FCallback = TFunction<void(FGameplayTag, const void*)>;

	struct FListener
	{
		FCallback Callback;
		TWeakObjectPtr<const UObject> Owner;
		FString OwnerName; // kept for the log after the owner is gone
		TWeakObjectPtr<const UScriptStruct> Type; // null: any payload
		bool bTyped = false;
		EBunnyMessageMatch Match = EBunnyMessageMatch::ExactMatch;
		int32 ID = 0;
		bool bRemoved = false;
	};

	static UE_API void BroadcastInternal(const UObject* Sender, FGameplayTag Channel, const UScriptStruct* Type, const void* Payload);
	static UE_API FBunnyMessageListenerHandle ListenInternal(const UObject* Owner, FGameplayTag Channel, const UScriptStruct* Type,
		EBunnyMessageMatch Match, FCallback&& Callback);
	void Remove(FGameplayTag Channel, int32 ID);

	TMap<FGameplayTag, TArray<TSharedRef<FListener>>> Listeners;
	int32 LastID = 0;
};

#undef UE_API
