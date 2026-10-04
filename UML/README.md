# Chess platform — UML diagrams

Fourteen focused diagrams of the C++ server and the browser frontend. There are 8 class diagrams for static structure and 6 sequence diagrams for runtime behaviour. Each class diagram covers one component, and each sequence diagram covers one use case. No single diagram tries to show the whole system.

All boxes, members and messages were checked against the headers in `src/` and `frontend/js/` as of **2026-10-04**.

## Where to start

1. **01 · Architecture overview.** One box per key class, with members hidden. Read it top to bottom to follow a request from the browser to the database.
2. Pick the component you care about (02–08) for its classes and members.
3. Read the matching sequence diagram (09–14) to see those classes working together at runtime.

## Class diagrams

| # | Diagram | Question it answers |
|---|---|---|
| 01 | [Architecture overview](01-architecture-overview.svg) | Which classes exist, and how does a request travel through them? |
| 02 | [Networking + request pipeline](02-networking-request-pipeline-classes.svg) | How do bytes on a socket become a validated, authenticated request? |
| 03 | [Authentication + sealed login](03-authentication-sealed-login-classes.svg) | How are passwords sealed (ML-KEM-768 + X25519), verified and turned into tokens? |
| 04 | [Chess rules + engine](04-chess-rules-engine-classes.svg) | How are moves generated and searched? |
| 05 | [Game room + gameplay + matchmaking](05-game-room-gameplay-classes.svg) | Who owns a live game, its clock and its players? |
| 06 | [Completion + persistence](06-completion-persistence-classes.svg) | How is a finished game saved without the game code knowing about Postgres? |
| 07 | [Tournaments](07-tournament-classes.svg) | How are tournaments paired, scheduled and played? |
| 08 | [Frontend](08-frontend-classes.svg) | How is the browser app structured (screens, socket, session)? |

## Sequence diagrams

| # | Diagram | Scenario |
|---|---|---|
| 09 | [Request lifecycle](09-request-lifecycle-sequence.svg) | A browser frame → epoll → worker → SealOpen → ParseJson → Auth → Dispatch → reply |
| 10 | [Sealed login](10-sealed-login-sequence.svg) | Fetch a signed one-time key, seal the password, verify, issue tokens |
| 11 | [Make move](11-make-move-sequence.svg) | Legal and illegal moves, broadcast to the opponent and spectators, the AI's reply |
| 12 | [Game completion](12-game-completion-sequence.svg) | A game ends and is saved exactly once, even if the save is retried |
| 13 | [Disconnect / reconnect](13-disconnect-reconnect-sequence.svg) | Reconnect within the 120 s grace period, or lose by `abandonment` |
| 14 | [Tournament round](14-tournament-round-sequence.svg) | Pair a round, reserve rooms, check players in, record results |

## How to read them

### Class diagrams

| Symbol | Meaning |
|---|---|
| Three-part box | Class name / attributes / operations |
| `- name : type` | Attribute: visibility, name, type |
| `+ method(params) : returnType` | Operation |
| `+` `#` `-` | public, protected, private |
| `<<interface>>` (italic name, dashed box) | Abstract class with only pure virtual methods, used as a port |
| `<<abstract>>` | Abstract class that has some implemented methods |
| `<<value>>`, `<<utility>>`, `<<facade>>` | A plain data struct; a class with only static helpers; a class that only wires others together |
| Hollow triangle ▷ | **Inheritance** (IS-A). Also used when a class implements an `<<interface>>`, because in C++ that is public inheritance |
| Open arrow → | **Simple association**: one class uses or calls the other. The italic label names the use |
| Hollow diamond ◇ (at the container) | **Aggregation**: the container holds a reference or pointer, and the part outlives it |
| Filled diamond ◆ (at the whole) | **Composition**: the whole owns the part by value or `unique_ptr`, so the part dies with it |
| `1`, `0..1`, `0..*`, `1..*` | Multiplicity at each end of an ownership line |

Box colours mark the layer (transport, protocol, application, domain, chess, storage, crypto, frontend). Each diagram has a legend in its bottom-left corner.

### Sequence diagrams

| Symbol | Meaning |
|---|---|
| Box + dashed vertical line | A participant and its lifeline |
| Narrow rectangle on a lifeline | Activation: that object is running |
| Solid line, filled head | Synchronous call: the caller waits |
| Solid line, open head | Asynchronous message (a WebSocket frame, a task queued to the thread pool) |
| Dashed line, open head | Return |
| `<<create>>` / ✕ `<<destroy>>` | An object is constructed / destroyed mid-scenario |
| `alt` / `opt` / `loop` frame + `[guard]` | If-else branches / optional block / repetition |

## Accuracy notes

- **The code is drawn, not the plan.** Source code is authoritative if a diagram becomes stale.
- **Members are selected, not exhaustive.** Each box shows the members that explain its role. Long C++ types are shortened, for example `shared_ptr<Connection>` stays but allocator and comparator arguments are dropped.
- **01 is a map, not a dependency graph.** It shows one line per important relationship. For example, `RequestPipeline → GameplayHandler "dispatch"` stands for route closures that `GameHandler` registers on the pipeline. Diagram 02 shows the real registration API.
- **The interface/implementation arrow is inheritance.** This follows the course notation. Strict UML would draw a dashed realization arrow. The meaning is the same.
- **Ports keep storage out of the game code.** `GameStore`, `PlayerQueries` and the other `<<interface>>` boxes are what the application layer depends on. Postgres and Null implementations sit behind them (06).

The published diagrams are SVG artifacts. If implementation changes, review the affected diagram against the source before treating it as authoritative.
