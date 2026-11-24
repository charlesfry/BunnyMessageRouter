# Bunny Message Router

An Unreal Engine plugin for messaging between objects that don't know about each other: a sender broadcasts a USTRUCT
on a gameplay-tag channel, and anything listening on that channel reacts. Built so that you can debug event-driven
code by reading the log.

## Install

Clone this repo beside your project and point the project at it:

```
Unreal Projects/
  BunnyMessageRouter/          <- this repo
    BunnyMessageRouter/        <- the plugin
  MyGame/
    MyGame.uproject
```

```json
"Plugins": [ { "Name": "BunnyMessageRouter", "Enabled": true } ],
"AdditionalPluginDirectories": [ "../BunnyMessageRouter" ]
```

Add `"BunnyMessageRuntime"` to your module's dependencies, then include `BunnyMessageSubsystem.h`.
Alternatively, copy the inner `BunnyMessageRouter` folder into your project's `Plugins/`.

## Use

```cpp
// Send. The sender is the world context and the name the log shows.
UBunnyMessageSubsystem::Broadcast(this, TAG_Door_Open, FDoorMessage{ Room });

// Listen: member function, lambda, or tag only (any payload).
UBunnyMessageSubsystem::Listen(this, TAG_Door_Open, &UAlarm::HandleDoorOpen);
UBunnyMessageSubsystem::Listen<FDoorMessage>(this, TAG_Door, [this](FGameplayTag Channel, const FDoorMessage& Msg) { ... },
	EBunnyMessageMatch::PartialMatch);   // also hears Door.Open, Door.Shut, ...
UBunnyMessageSubsystem::ListenForTag(this, TAG_Night_Complete, [this](FGameplayTag) { ... });

// Stop. In EndPlay:
UBunnyMessageSubsystem::UnregisterAll(this);
```

- **Owners.** The first argument of every Listen is the listener's owner: its world, its name in the log, and its
  lifetime. A listener whose owner has been destroyed is never called. Unregister in EndPlay anyway (with
  `UnregisterAll`, or the handle that Listen returns), because an actor can end play without being destroyed.
- **Payload types.** A listener may take the broadcast struct or any parent of it. A mismatch is logged as an error
  and the listener is not called.
- **Order is guaranteed.** Listeners on the broadcast channel are called first, then those on each parent channel in
  turn (PartialMatch listeners only). Within a channel, listeners are called in the order they registered.
- **Re-entrancy.** A listener registered during a broadcast first hears the next broadcast. A listener removed
  during a broadcast is not called by it. A broadcast made inside a reaction is delivered immediately, before the
  outer broadcast moves on.
- **Game thread only.** A broadcast from another thread is dropped and raises an ensure.
- **Blueprints.** Use *Broadcast Message* and *Listen for Bunny Messages*. The Blueprint that calls the listen node
  owns the listener.

## Debugging

Everything is logged to `LogBunnyMessages`. Each broadcast gets a number, its reactions share that number, and a
broadcast made inside a reaction is indented under it, so a chain of messages reads as a tree:

```
LISTEN Trigger.Door.Open (exact, DoorMessage) <- BP_Alarm_C_0
[12] BROADCAST Trigger.Door.Open from BP_Door_C_1 (Room=(TagName="Area.Hall"))
[12]   REACT Trigger.Door.Open -> BP_Alarm_C_0
[13]     BROADCAST Event.Alarm from BP_Alarm_C_0 ()
[13]       REACT Event (partial) -> NightDirector_0
[14] BROADCAST Trigger.Door.Shut from BP_Door_C_1 ()
[14]   NOBODY LISTENING (1 ExactMatch listener(s) on a parent channel: Trigger.Door -> BP_Hud_C_0)
UNLISTEN Trigger.Door.Open <- BP_Alarm_C_0
```

| Question | Where the answer is |
|---|---|
| Who sent it, and with what payload? | `BROADCAST <channel> from <sender> (<payload>)` |
| Who reacted, in what order, and why did they match? | `REACT <listener's channel> [(partial)] -> <owner>`, in call order |
| What did this reaction cause? | The lines indented under it |
| Why didn't my listener fire? | `NOBODY LISTENING` names any listener on a parent channel that is missing PartialMatch. `TYPE MISMATCH` is logged as an error. `LISTEN`/`UNLISTEN` show whether it was registered at the time. |
| Who is listening right now? | Console: `BunnyMessages.Dump` or `BunnyMessages.Dump Trigger.Door` (also lists parent channels that would hear it) |
| Is this a loop? | Broadcasts more than 16 deep are dropped with an error |
| Did a tag fail to resolve? | A broadcast or listen on an invalid channel logs a warning and is dropped |

To keep the log quieter, run `log LogBunnyMessages Warning` (errors and warnings only). For more detail, run
`log LogBunnyMessages Verbose`, which adds the removal of listeners whose owners died.

`UBunnyMessageSubsystem::Describe(Object)` returns the name the log uses for an object: an actor's label,
`Actor.Component` for a component, or `Actor.Object` for an object inside an actor. Use it in your own log lines so
they match.

## Tests

`BunnyMessageRouter.Subsystem` (Session Frontend, or `Automation RunTests BunnyMessageRouter`).
