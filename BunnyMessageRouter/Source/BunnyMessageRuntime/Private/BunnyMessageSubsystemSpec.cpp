// BunnyMessageRouter. Messages delivered at the speed of a startled rabbit.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "BunnyMessageSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/OutputDevice.h"
#include "NativeGameplayTags.h"

namespace BunnyMessageSpec
{
	UE_DEFINE_GAMEPLAY_TAG_STATIC(TagA, "BunnyMessagesTest.A");
	UE_DEFINE_GAMEPLAY_TAG_STATIC(TagAB, "BunnyMessagesTest.A.B");
	UE_DEFINE_GAMEPLAY_TAG_STATIC(TagC, "BunnyMessagesTest.C");

	// Collects LogBunnyMessages lines (the automation framework's expected messages ignore Log verbosity).
	struct FCapture : FOutputDevice
	{
		TArray<FString> Lines;
		FCapture() { GLog->Flush(); GLog->AddOutputDevice(this); } // flush first: earlier lines are still in flight
		virtual ~FCapture() override { GLog->RemoveOutputDevice(this); }
		virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type, const FName& Category) override
		{
			if (Category == LogBunnyMessages.GetCategoryName())
			{
				Lines.Add(Text);
			}
		}
		bool Has(const FString& Needle) const
		{
			GLog->Flush(); // devices are fed from the logging thread
			return Lines.ContainsByPredicate([&Needle](const FString& Line) { return Line.Contains(Needle); });
		}
	};
}

BEGIN_DEFINE_SPEC(FBunnyMessageSubsystemSpec, "BunnyMessageRouter.Subsystem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	UGameInstance* GameInstance = nullptr;
	AActor* Sender = nullptr;
	AActor* Listener = nullptr;
	AActor* Other = nullptr;

END_DEFINE_SPEC(FBunnyMessageSubsystemSpec)

void FBunnyMessageSubsystemSpec::Define()
{
	using namespace BunnyMessageSpec;
	using Bus = UBunnyMessageSubsystem;

	BeforeEach([this]()
	{
		GameInstance = NewObject<UGameInstance>(GEngine);
		GameInstance->InitializeStandalone();
		UWorld* World = GameInstance->GetWorld();
		Sender = World->SpawnActor<AActor>();
		Listener = World->SpawnActor<AActor>();
		Other = World->SpawnActor<AActor>();
	});

	AfterEach([this]()
	{
		GameInstance->Shutdown();
		GameInstance = nullptr;
	});

	Describe("delivery", [this]()
	{
		It("delivers the payload to an exact listener", [this]()
		{
			FVector Received = FVector::ZeroVector;
			Bus::Listen<FVector>(Listener, TagA, [&Received](FGameplayTag, const FVector& V) { Received = V; });
			Bus::Broadcast(Sender, TagA, FVector(1, 2, 3));
			TestEqual("payload", Received, FVector(1, 2, 3));
		});

		It("reaches a partial listener on a parent channel with the actual channel", [this]()
		{
			FGameplayTag Actual;
			Bus::Listen<FVector>(Listener, TagA, [&Actual](FGameplayTag Ch, const FVector&) { Actual = Ch; }, EBunnyMessageMatch::PartialMatch);
			Bus::Broadcast(Sender, TagAB, FVector());
			TestTrue("actual channel", Actual == TagAB.GetTag());
		});

		It("does not reach an exact listener on a parent channel", [this]()
		{
			bool bCalled = false;
			Bus::Listen<FVector>(Listener, TagA, [&bCalled](FGameplayTag, const FVector&) { bCalled = true; });
			Bus::Broadcast(Sender, TagAB, FVector());
			TestFalse("not called", bCalled);
		});

		It("ListenForTag hears any payload", [this]()
		{
			int32 Calls = 0;
			Bus::ListenForTag(Listener, TagA, [&Calls](FGameplayTag) { ++Calls; });
			Bus::Broadcast(Sender, TagA, FVector());
			Bus::Broadcast(Sender, TagA, FRotator());
			TestEqual("calls", Calls, 2);
		});

		It("does not call a listener expecting another payload type, and logs an error", [this]()
		{
			AddExpectedMessagePlain(TEXT("TYPE MISMATCH"), ELogVerbosity::Error);
			bool bCalled = false;
			Bus::Listen<FVector>(Listener, TagA, [&bCalled](FGameplayTag, const FVector&) { bCalled = true; });
			Bus::Broadcast(Sender, TagA, FRotator());
			TestFalse("not called", bCalled);
		});
	});

	Describe("order", [this]()
	{
		It("calls listeners in registration order, even after a removal", [this]()
		{
			FString Order;
			FBunnyMessageListenerHandle First = Bus::ListenForTag(Listener, TagA, [&Order](FGameplayTag) { Order += TEXT("1"); });
			Bus::ListenForTag(Listener, TagA, [&Order](FGameplayTag) { Order += TEXT("2"); });
			Bus::ListenForTag(Listener, TagA, [&Order](FGameplayTag) { Order += TEXT("3"); });
			First.Unregister();
			Bus::Broadcast(Sender, TagA, FVector());
			TestEqual("order", Order, FString(TEXT("23")));
		});

		It("calls the broadcast channel's listeners before a parent channel's", [this]()
		{
			FString Order;
			Bus::ListenForTag(Listener, TagA, [&Order](FGameplayTag) { Order += TEXT("parent"); }, EBunnyMessageMatch::PartialMatch);
			Bus::ListenForTag(Listener, TagAB, [&Order](FGameplayTag) { Order += TEXT("child,"); });
			Bus::Broadcast(Sender, TagAB, FVector());
			TestEqual("order", Order, FString(TEXT("child,parent")));
		});
	});

	Describe("lifetime", [this]()
	{
		It("does not call a listener removed earlier in the same broadcast", [this]()
		{
			bool bSecondCalled = false;
			FBunnyMessageListenerHandle Second;
			Bus::ListenForTag(Listener, TagA, [&Second](FGameplayTag) { Second.Unregister(); });
			Second = Bus::ListenForTag(Listener, TagA, [&bSecondCalled](FGameplayTag) { bSecondCalled = true; });
			Bus::Broadcast(Sender, TagA, FVector());
			TestFalse("removed listener not called", bSecondCalled);
		});

		It("does not call a listener added during the broadcast until the next one", [this]()
		{
			int32 LateCalls = 0;
			Bus::ListenForTag(Listener, TagA, [this, &LateCalls](FGameplayTag)
			{
				Bus::ListenForTag(Other, TagA, [&LateCalls](FGameplayTag) { ++LateCalls; });
			});
			Bus::Broadcast(Sender, TagA, FVector());
			TestEqual("not called by the broadcast it was added in", LateCalls, 0);
		});

		It("does not call a listener whose owner was destroyed", [this]()
		{
			bool bCalled = false;
			Bus::ListenForTag(Listener, TagA, [&bCalled](FGameplayTag) { bCalled = true; });
			Listener->Destroy();
			Bus::Broadcast(Sender, TagA, FVector());
			TestFalse("dead owner's listener not called", bCalled);
		});

		It("UnregisterAll removes only that owner's listeners", [this]()
		{
			int32 ListenerCalls = 0;
			int32 OtherCalls = 0;
			Bus::ListenForTag(Listener, TagA, [&ListenerCalls](FGameplayTag) { ++ListenerCalls; });
			Bus::ListenForTag(Listener, TagC, [&ListenerCalls](FGameplayTag) { ++ListenerCalls; });
			Bus::ListenForTag(Other, TagA, [&OtherCalls](FGameplayTag) { ++OtherCalls; });
			Bus::UnregisterAll(Listener);
			Bus::Broadcast(Sender, TagA, FVector());
			Bus::Broadcast(Sender, TagC, FVector());
			TestEqual("Listener's removed", ListenerCalls, 0);
			TestEqual("Other's kept", OtherCalls, 1);
		});

		It("Find returns null without a world instead of asserting", [this]()
		{
			TestNull("null context", Bus::Find(nullptr));
			TestNull("class default object", Bus::Find(GetDefault<AActor>()));
		});
	});

	Describe("log", [this]()
	{
		It("logs the broadcast with its sender and payload, and each reaction with its listener", [this]()
		{
			Bus::ListenForTag(Listener, TagA, [](FGameplayTag) {}, EBunnyMessageMatch::PartialMatch);
			FCapture Capture;
			Bus::Broadcast(Sender, TagAB, FVector(1, 2, 3));
			TestTrue("broadcast", Capture.Has(FString::Printf(TEXT("] BROADCAST BunnyMessagesTest.A.B from %s (X=1"), *Bus::Describe(Sender))));
			TestTrue("reaction names the listener's channel and match", Capture.Has(FString::Printf(TEXT("]   REACT BunnyMessagesTest.A (partial) -> %s"), *Bus::Describe(Listener))));
		});

		It("numbers each broadcast and indents one made inside a reaction under it", [this]()
		{
			Bus::ListenForTag(Listener, TagA, [this](FGameplayTag) { Bus::Broadcast(Listener, TagC, FVector()); });
			Bus::ListenForTag(Other, TagC, [](FGameplayTag) {});
			FCapture Capture;
			Bus::Broadcast(Sender, TagA, FVector());
			GLog->Flush();
			TestEqual("lines", Capture.Lines.Num(), 4);
			if (Capture.Lines.Num() == 4)
			{
				auto Number = [](const FString& Line) { return Line.Left(Line.Find(TEXT("]"))); };
				TestTrue("nested broadcast indented", Capture.Lines[2].Contains(TEXT("]     BROADCAST BunnyMessagesTest.C")));
				TestTrue("nested reaction indented", Capture.Lines[3].Contains(TEXT("]       REACT BunnyMessagesTest.C")));
				TestEqual("reaction shares its broadcast's number", Number(Capture.Lines[1]), Number(Capture.Lines[0]));
				TestNotEqual("nested broadcast has its own number", Number(Capture.Lines[2]), Number(Capture.Lines[0]));
			}
		});

		It("says when nobody is listening, and names exact listeners on a parent channel", [this]()
		{
			Bus::ListenForTag(Listener, TagA, [](FGameplayTag) {});
			FCapture Capture;
			Bus::Broadcast(Sender, TagAB, FVector());
			TestTrue("near miss", Capture.Has(FString::Printf(TEXT("NOBODY LISTENING (1 ExactMatch listener(s) on a parent channel: BunnyMessagesTest.A -> %s)"), *Bus::Describe(Listener))));
		});

		It("logs listeners arriving and leaving", [this]()
		{
			FCapture Capture;
			FBunnyMessageListenerHandle Handle = Bus::ListenForTag(Listener, TagA, [](FGameplayTag) {});
			Handle.Unregister();
			TestTrue("listen", Capture.Has(FString::Printf(TEXT("LISTEN BunnyMessagesTest.A (exact, any payload) <- %s"), *Bus::Describe(Listener))));
			TestTrue("unlisten", Capture.Has(FString::Printf(TEXT("UNLISTEN BunnyMessagesTest.A <- %s"), *Bus::Describe(Listener))));
		});

		It("Dump lists the live listeners matching a filter", [this]()
		{
			Bus::ListenForTag(Listener, TagA, [](FGameplayTag) {}, EBunnyMessageMatch::PartialMatch);
			Bus::ListenForTag(Other, TagC, [](FGameplayTag) {});
			FCapture Capture;
			Bus::Find(Sender)->Dump(TagAB);
			TestTrue("parent channel listener listed", Capture.Has(FString::Printf(TEXT("BunnyMessagesTest.A (partial, any payload) <- %s"), *Bus::Describe(Listener))));
			TestFalse("unrelated channel left out", Capture.Has(TEXT("BunnyMessagesTest.C")));
			TestTrue("count", Capture.Has(TEXT("1 listener(s)")));
		});

		It("stops a message loop and logs an error", [this]()
		{
			AddExpectedMessagePlain(TEXT("a message loop?"), ELogVerbosity::Error);
			int32 Calls = 0;
			Bus::ListenForTag(Listener, TagA, [this, &Calls](FGameplayTag) { ++Calls; Bus::Broadcast(Listener, TagA, FVector()); });
			Bus::Broadcast(Sender, TagA, FVector());
			TestEqual("calls stop at the depth limit", Calls, 16);
		});

		It("warns about an invalid channel", [this]()
		{
			AddExpectedMessagePlain(TEXT("BROADCAST with an invalid channel"), ELogVerbosity::Warning);
			Bus::Broadcast(Sender, FGameplayTag(), FVector());
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
