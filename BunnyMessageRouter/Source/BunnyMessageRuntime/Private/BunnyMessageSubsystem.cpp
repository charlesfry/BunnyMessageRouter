// BunnyMessageRouter. Messages delivered at the speed of a startled rabbit.

#include "BunnyMessageSubsystem.h"

#include "Components/ActorComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "UObject/Stack.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(BunnyMessageSubsystem)

IMPLEMENT_MODULE(FDefaultModuleImpl, BunnyMessageRuntime)

DEFINE_LOG_CATEGORY(LogBunnyMessages);

namespace BunnyMessages
{
	constexpr int32 MaxDepth = 16;

	// Game thread only (ensured in BroadcastInternal).
	int32 Depth = 0;
	int32 LastBroadcastNumber = 0;

	FString Indent(int32 Spaces) { return FString::ChrN(Spaces, TEXT(' ')); }

	const TCHAR* MatchName(EBunnyMessageMatch Match) { return Match == EBunnyMessageMatch::PartialMatch ? TEXT("partial") : TEXT("exact"); }

	FString TypeName(const UScriptStruct* Type) { return Type ? Type->GetName() : TEXT("any payload"); }

	FAutoConsoleCommandWithWorldAndArgs DumpCommand(
		TEXT("BunnyMessages.Dump"),
		TEXT("Logs every live message listener. Optional argument: a tag; only channels matching it are listed."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (const UBunnyMessageSubsystem* Subsystem = UBunnyMessageSubsystem::Find(World))
			{
				Subsystem->Dump(Args.IsEmpty() ? FGameplayTag() : FGameplayTag::RequestGameplayTag(FName(*Args[0]), false));
			}
			else
			{
				UE_LOG(LogBunnyMessages, Warning, TEXT("BunnyMessages.Dump: no game instance in this world (start PIE first)."));
			}
		}));
}

//////////////////////////////////////////////////////////////////////
// FBunnyMessageListenerHandle

void FBunnyMessageListenerHandle::Unregister()
{
	if (UBunnyMessageSubsystem* StrongSubsystem = Subsystem.Get())
	{
		StrongSubsystem->Remove(Channel, ID);
	}
	*this = FBunnyMessageListenerHandle();
}

//////////////////////////////////////////////////////////////////////
// UBunnyMessageSubsystem

UBunnyMessageSubsystem* UBunnyMessageSubsystem::Find(const UObject* WorldContext)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	return World ? UGameInstance::GetSubsystem<UBunnyMessageSubsystem>(World->GetGameInstance()) : nullptr;
}

FString UBunnyMessageSubsystem::Describe(const UObject* Object)
{
	if (!Object)
	{
		return TEXT("none");
	}
	if (const AActor* Actor = Cast<AActor>(Object))
	{
		return Actor->GetActorNameOrLabel();
	}
	if (const UActorComponent* Component = Cast<UActorComponent>(Object))
	{
		return Describe(Component->GetOwner()) + TEXT(".") + Component->GetName();
	}
	const UObject* Outer = Object->GetOuter();
	if (Cast<AActor>(Outer) || Cast<UActorComponent>(Outer))
	{
		return Describe(Outer) + TEXT(".") + Object->GetName();
	}
	return Object->GetName();
}

void UBunnyMessageSubsystem::Deinitialize()
{
	for (TPair<FGameplayTag, TArray<TSharedRef<FListener>>>& Pair : Listeners)
	{
		for (const TSharedRef<FListener>& Listener : Pair.Value)
		{
			Listener->bRemoved = true;
		}
	}
	Listeners.Reset();
	Super::Deinitialize();
}

void UBunnyMessageSubsystem::BroadcastInternal(const UObject* Sender, FGameplayTag Channel, const UScriptStruct* Type, const void* Payload)
{
	using namespace BunnyMessages;

	if (!ensureMsgf(IsInGameThread(), TEXT("BunnyMessages: broadcast of %s from %s off the game thread; dropped."), *Channel.ToString(), *Describe(Sender)))
	{
		return;
	}
	if (!Channel.IsValid())
	{
		UE_LOG(LogBunnyMessages, Warning, TEXT("BROADCAST with an invalid channel from %s (%s); dropped. Is the tag declared?"), *Describe(Sender), *TypeName(Type));
		return;
	}
	UBunnyMessageSubsystem* Self = Find(Sender);
	if (!Self)
	{
		UE_LOG(LogBunnyMessages, Warning, TEXT("BROADCAST %s from %s: the sender has no world or game instance; dropped."), *Channel.ToString(), *Describe(Sender));
		return;
	}
	if (Depth >= MaxDepth)
	{
		UE_LOG(LogBunnyMessages, Error, TEXT("BROADCAST %s from %s: %d broadcasts deep, a message loop? Dropped."), *Channel.ToString(), *Describe(Sender), Depth);
		return;
	}

	const int32 Number = ++LastBroadcastNumber;
	const FString Pad = Indent(Depth * 4);
	FString PayloadText;
	if (UE_LOG_ACTIVE(LogBunnyMessages, Log))
	{
		Type->ExportText(PayloadText, Payload, /*Defaults=*/ nullptr, /*OwnerObject=*/ nullptr, PPF_None, /*ExportRootScope=*/ nullptr);
	}
	UE_LOG(LogBunnyMessages, Log, TEXT("[%d] %sBROADCAST %s from %s %s"), Number, *Pad, *Channel.ToString(), *Describe(Sender), *PayloadText);

	++Depth;
	int32 Reactions = 0;
	TArray<FString> NearMisses; // ExactMatch listeners on a parent channel: the usual "why didn't it fire"
	bool bExact = true;
	for (FGameplayTag Tag = Channel; Tag.IsValid(); Tag = Tag.RequestDirectParent(), bExact = false)
	{
		const TArray<TSharedRef<FListener>>* Found = Self->Listeners.Find(Tag);
		if (!Found)
		{
			continue;
		}
		// Snapshot: a listener added during this broadcast waits for the next one; one removed is skipped via bRemoved.
		const TArray<TSharedRef<FListener>> Snapshot = *Found;
		for (const TSharedRef<FListener>& Listener : Snapshot)
		{
			if (Listener->bRemoved)
			{
				continue;
			}
			if (!bExact && Listener->Match != EBunnyMessageMatch::PartialMatch)
			{
				NearMisses.Add(FString::Printf(TEXT("%s -> %s"), *Tag.ToString(), *Listener->OwnerName));
				continue;
			}
			const UObject* Owner = Listener->Owner.Get();
			if (!Owner)
			{
				UE_LOG(LogBunnyMessages, Verbose, TEXT("[%d] %s  PRUNE %s -> %s (owner destroyed without UnregisterAll)"), Number, *Pad, *Tag.ToString(), *Listener->OwnerName);
				Self->Remove(Tag, Listener->ID);
				continue;
			}
			if (Listener->bTyped && !Listener->Type.IsValid())
			{
				UE_LOG(LogBunnyMessages, Warning, TEXT("[%d] %s  PRUNE %s -> %s (its payload type was unloaded)"), Number, *Pad, *Tag.ToString(), *Listener->OwnerName);
				Self->Remove(Tag, Listener->ID);
				continue;
			}
			if (Listener->bTyped && !Type->IsChildOf(Listener->Type.Get()))
			{
				UE_LOG(LogBunnyMessages, Error, TEXT("[%d] %s  TYPE MISMATCH %s -> %s expects %s, got %s; not called."), Number, *Pad, *Tag.ToString(),
					*Listener->OwnerName, *TypeName(Listener->Type.Get()), *TypeName(Type));
				continue;
			}
			++Reactions;
			UE_LOG(LogBunnyMessages, Log, TEXT("[%d] %s  REACT %s%s -> %s"), Number, *Pad, *Tag.ToString(), bExact ? TEXT("") : TEXT(" (partial)"), *Describe(Owner));
			Listener->Callback(Channel, Payload);
		}
	}
	--Depth;

	if (Reactions == 0)
	{
		UE_LOG(LogBunnyMessages, Log, TEXT("[%d] %s  NOBODY LISTENING%s"), Number, *Pad, NearMisses.IsEmpty() ? TEXT("") :
			*FString::Printf(TEXT(" (%d ExactMatch listener(s) on a parent channel: %s)"), NearMisses.Num(), *FString::Join(NearMisses, TEXT(", "))));
	}
}

FBunnyMessageListenerHandle UBunnyMessageSubsystem::ListenInternal(const UObject* Owner, FGameplayTag Channel, const UScriptStruct* Type,
	EBunnyMessageMatch Match, FCallback&& Callback)
{
	using namespace BunnyMessages;

	FBunnyMessageListenerHandle Handle;
	if (!Channel.IsValid())
	{
		UE_LOG(LogBunnyMessages, Warning, TEXT("LISTEN on an invalid channel by %s (%s); ignored. Is the tag declared?"), *Describe(Owner), *TypeName(Type));
		return Handle;
	}
	UBunnyMessageSubsystem* Self = Find(Owner);
	if (!Self)
	{
		UE_LOG(LogBunnyMessages, Warning, TEXT("LISTEN %s by %s: the owner has no world or game instance; ignored."), *Channel.ToString(), *Describe(Owner));
		return Handle;
	}

	TSharedRef<FListener> Listener = MakeShared<FListener>();
	Listener->Callback = MoveTemp(Callback);
	Listener->Owner = Owner;
	Listener->OwnerName = Describe(Owner);
	Listener->Type = Type;
	Listener->bTyped = Type != nullptr;
	Listener->Match = Match;
	Listener->ID = ++Self->LastID;
	Self->Listeners.FindOrAdd(Channel).Add(Listener);
	UE_LOG(LogBunnyMessages, Log, TEXT("LISTEN %s (%s, %s) <- %s"), *Channel.ToString(), MatchName(Match), *TypeName(Type), *Listener->OwnerName);

	Handle.Subsystem = Self;
	Handle.Channel = Channel;
	Handle.ID = Listener->ID;
	return Handle;
}

FBunnyMessageListenerHandle UBunnyMessageSubsystem::ListenForTag(const UObject* Owner, FGameplayTag Channel, TFunction<void(FGameplayTag)>&& Callback, EBunnyMessageMatch Match)
{
	return ListenInternal(Owner, Channel, nullptr, Match, [Callback = MoveTemp(Callback)](FGameplayTag Ch, const void*) { Callback(Ch); });
}

void UBunnyMessageSubsystem::Remove(FGameplayTag Channel, int32 ID)
{
	TArray<TSharedRef<FListener>>* List = Listeners.Find(Channel);
	if (!List)
	{
		return;
	}
	const int32 Index = List->IndexOfByPredicate([ID](const TSharedRef<FListener>& Listener) { return Listener->ID == ID; });
	if (Index != INDEX_NONE)
	{
		UE_LOG(LogBunnyMessages, Log, TEXT("UNLISTEN %s <- %s"), *Channel.ToString(), *(*List)[Index]->OwnerName);
		(*List)[Index]->bRemoved = true;
		List->RemoveAt(Index); // not RemoveAtSwap: registration order is the call order
	}
	if (List->IsEmpty())
	{
		Listeners.Remove(Channel);
	}
}

void UBunnyMessageSubsystem::UnregisterAll(const UObject* Owner)
{
	UBunnyMessageSubsystem* Self = Find(Owner);
	if (!Self || !Owner)
	{
		return;
	}
	for (auto It = Self->Listeners.CreateIterator(); It; ++It)
	{
		It->Value.RemoveAll([Owner, &It](const TSharedRef<FListener>& Listener)
		{
			if (Listener->Owner.Get(/*bEvenIfGarbage=*/ true) != Owner)
			{
				return false;
			}
			UE_LOG(LogBunnyMessages, Log, TEXT("UNLISTEN %s <- %s"), *It->Key.ToString(), *Listener->OwnerName);
			Listener->bRemoved = true;
			return true;
		});
		if (It->Value.IsEmpty())
		{
			It.RemoveCurrent();
		}
	}
}

void UBunnyMessageSubsystem::Dump(FGameplayTag Filter) const
{
	using namespace BunnyMessages;

	TArray<FGameplayTag> Channels;
	Listeners.GetKeys(Channels);
	Channels.Sort([](const FGameplayTag& A, const FGameplayTag& B) { return A.ToString() < B.ToString(); });

	int32 Count = 0;
	for (const FGameplayTag& Channel : Channels)
	{
		if (Filter.IsValid() && !Channel.MatchesTag(Filter) && !Filter.MatchesTag(Channel))
		{
			continue;
		}
		for (const TSharedRef<FListener>& Listener : Listeners[Channel])
		{
			++Count;
			UE_LOG(LogBunnyMessages, Display, TEXT("  %s (%s, %s) <- %s%s"), *Channel.ToString(), MatchName(Listener->Match), *TypeName(Listener->Type.Get()),
				*Listener->OwnerName, Listener->Owner.IsValid() ? TEXT("") : TEXT(" [owner destroyed]"));
		}
	}
	UE_LOG(LogBunnyMessages, Display, TEXT("BunnyMessages.Dump%s: %d listener(s)"), Filter.IsValid() ? *(TEXT(" ") + Filter.ToString()) : TEXT(""), Count);
}

void UBunnyMessageSubsystem::K2_BroadcastMessage(const UObject* Sender, FGameplayTag Channel, const int32& Message)
{
	checkNoEntry(); // execK2_BroadcastMessage runs instead
}

DEFINE_FUNCTION(UBunnyMessageSubsystem::execK2_BroadcastMessage)
{
	P_GET_OBJECT(UObject, Sender);
	P_GET_STRUCT(FGameplayTag, Channel);

	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	void* MessagePtr = Stack.MostRecentPropertyAddress;
	FStructProperty* StructProp = CastField<FStructProperty>(Stack.MostRecentProperty);

	P_FINISH;

	if (ensure(StructProp && StructProp->Struct && MessagePtr))
	{
		BroadcastInternal(Sender, Channel, StructProp->Struct, MessagePtr);
	}
}
