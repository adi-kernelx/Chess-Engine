# Frontend Implementation Plan

A consolidated delivery plan for the chess platform's static browser application: foundation, playable journeys, community features, live tournaments and release hardening.

**Current state:** the foundation and release feature set are implemented. The local acceptance checks reported by the tester are complete. Production integration checks and the explicitly marked UX backlog remain open.

## How to read the schedule

Weeks below describe a relative delivery sequence, not invented completion dates or a new promise to redo finished work. Completed stages summarize delivered functionality. Future week ranges are effort windows to estimate when those improvements are prioritized. Production gates depend on deployment rather than the calendar.

| Delivery window | Milestone | Status |
| --- | --- | --- |
| Week 1 | Design foundation, routing and shared components | Complete |
| Week 2 | Transport, board and live game | Complete |
| Week 3 | Accounts, settings, profile and leaderboard | Complete |
| Week 4 | Watch, replay and responsive baseline | Complete |
| Week 5 | Landing, invite and sharing journeys | Complete for current scope |
| Week 6 | Puzzle, streak and bot selection | Complete with local-only progression |
| Weeks 7–8 | Live tournament integration | Complete for release scope |
| Week 9 | OAuth, sealed auth and release recovery | Implemented; deployed validation pending |
| Future window A: 1 week | Mobile-first refinement | Planned beyond existing responsive baseline |
| Future window B: 1 week | Consistent effects and motion settings | Partial; optional follow-on |
| Future window C: 1–2 weeks | Community and cross-device progression | Partial; backend contracts required |
| Future window D: 1 week | Full board accessibility and onboarding audit | Planned beyond existing form/navigation accessibility |

## Product and technical constraints

The journey is **arrive → play → return → invite → belong**. A reliable board takes priority over decorative effects.

- Keep static HTML, CSS and vanilla JavaScript ES modules; no framework migration.
- Reuse the Canvas board, SVG pieces, promotion picker and move interaction across game, spectate, replay and puzzle views.
- Preserve the warm-neutral/amber design system in `frontend/css/tokens.css`.
- Use `net/protocol.js` for outbound factories and inbound normalization; screens must not invent wire contracts.
- Server authority determines identity, legal moves, clocks, results, access and tournament advancement.
- Represent loading, empty, unavailable, retrying and signed-out states separately.
- Never invent participants, presence counts, game history or live results.
- Release-required vendored cryptography is an approved dependency; no runtime CDN loading.
- Avoid new animation frameworks, WebGL or heavy dependencies for decorative features.

## Browser architecture

| Layer | Files/directories | Responsibility |
| --- | --- | --- |
| Bootstrap/configuration | `frontend/index.html`, `js/main.js`, `js/config.js` | App startup, routes, public configuration and identity pins |
| Design system | `frontend/css/` | Tokens, components, screen layouts and responsive behavior |
| State/helpers | `frontend/js/core/` | Store, events, storage, formatting, bots, streaks and sharing |
| Transport/identity | `frontend/js/net/` | WebSocket lifecycle, protocol, request correlation, sessions, OAuth and sealing |
| Shared UI | `frontend/js/ui/` | Screen lifecycle, router, modal, toast and sound |
| Board | `frontend/js/board/` | Canvas rendering, pieces, input, promotion and puzzle position logic |
| Features | `frontend/js/screens/` | Landing, lobby, game, auth, settings, profile, leaderboard, watch, replay, puzzle and tournaments |
| Static assets | `frontend/assets/`, `js/vendor/` | Piece/logo/puzzle assets and pinned crypto bundle with integrity manifest/licenses |

Route changes must unsubscribe listeners, cancel screen-owned work and release observers. A reply for an unmounted screen must not update a newer screen or revoke a valid session.

## Milestone 1 — Foundation and navigation

- [x] Shared spacing, typography, color and component tokens.
- [x] Buttons, cards, tabs, tables, badges, skeleton/error states and notifications.
- [x] Hash routing with screen mount/unmount lifecycle.
- [x] Accessible page titles, skip link and route focus handling.
- [x] Responsive navigation and modal focus trapping.
- [x] Reduced-motion baseline and printable layouts.

**Acceptance:** all routes mount and tear down cleanly; browser back/deep links are meaningful; unavailable data does not look like successful live data.

## Milestone 2 — Transport, board and gameplay

- [x] WebSocket reconnect and explicit connection state.
- [x] Protocol normalization and correlated request handling.
- [x] High-DPI Canvas rendering with shared SVG piece themes.
- [x] Drag and tap-to-move, legal-move hints and promotion selection.
- [x] Lobby creation, joining, quick-play and AI choices.
- [x] Position, move list, opponent names and interpolated authoritative clocks.
- [x] Resign, inline draw offers and ordinary-game rematches.
- [x] Active-seat recovery without converting spectators into players.

**Acceptance:** illegal moves retain the board; terminal games reject play; missing-seat tournament waits do not run clocks; a reconnect restores the same seat and position while its room still exists.

## Milestone 3 — Accounts and progression views

- [x] Password registration/sign-in and sign-out controls.
- [x] Settings and persisted browser preferences.
- [x] Real profile, rating, history and leaderboard data.
- [x] Honest empty states in place of demonstration records.
- [x] Busy-state prevention of duplicate form submission.
- [x] Username/email-only registration with explicit taken-name/address feedback; password chosen after mailbox verification, then automatic sealed login. Reset also signs in to the original account. Frontend/backend enforce the new-password complexity policy without changing existing login credentials.
- [x] Forgot-password, activation/reset and legacy recovery-email forms using sealed auth.
- [x] Single-use link tokens scrubbed from history and kept out of browser storage.
- [ ] Manual email-delivery/form acceptance after backend migration/configuration rollout.
- [x] Inline field errors and focused summaries for multi-error forms.

**Acceptance:** expired access tokens trigger refresh recovery; temporary network trouble alone does not erase credentials; an explicit invalid/revoked session returns to sign-in.

## Milestone 4 — Watch, replay and responsive baseline

- [x] Real live-game directory and authenticated read-only spectating.
- [x] Shared board rendering for saved-game replay.
- [x] Move navigation and on-demand analysis.
- [x] Original players, colors, result and termination from durable game data.
- [x] Responsive screen reflow, mobile navigation, table overflow handling and touch input.
- [x] Modal/promotion keyboard interaction and visible focus states.

**Acceptance:** spectators never receive player controls; replay IDs identify saved games, not live rooms; refresh recovers the requested replay without opening an unrelated game.

## Milestone 5 — Front door, invite and sharing

- [x] `#/` landing and `#/play` lobby with returning-visitor routing.
- [x] Clear primary action and quick match / bot / invite choices.
- [x] Lightweight layered hero piece and pointer-gated landing-card tilt.
- [x] Helpful empty-game-list calls to action.
- [x] `#/join/<id>` invite links and `#/game/<id>` game links.
- [x] Result-image/PGN sharing helpers and ordinary-game rematch handoff.

**Scope corrections:** Play now requires authenticated backend identity; a client-generated handle is not a guest session. Current invite rooms must not be described as private token-protected rooms. Live social-proof numbers are omitted when unavailable.

- [ ] Server-issued guest sessions and safe account claiming, if prioritized later.
- [ ] Private challenge visibility and scoped invite tokens, if prioritized later.
- [ ] Human-match switching while playing a bot, if supported by a future server contract.

## Milestone 6 — Puzzle, streak and bots

- [x] Daily selection from a bundled puzzle asset and shared board interaction.
- [x] Local puzzle feedback and streak/daily-goal persistence.
- [x] Eight named bot choices mapped to existing difficulty/search limits.
- [x] Progression surfaces without fabricated account history.

**Scope corrections:** puzzle/streak state is per browser, not synchronized account progress. Displayed bot strength is a product label, not a measured tournament rating.

- [ ] Server-authoritative puzzle attempts and cross-device streaks.
- [ ] Server-managed bot catalog and per-bot statistics.
- [ ] Reliable post-game best-move/blunder summaries backed by stored analysis.

## Milestone 7 — Live tournaments

### Creation and registration

- [x] Swiss and Winners Advance format selection.
- [x] Swiss round-count field hidden and irrelevant in Winners Advance.
- [x] Schedule validation, local-time preview and meaningful field errors.
- [x] Unique names, real participants and registration controls.
- [x] Deadline/pairing-lock feedback and automatic registration closure.
- [x] Tournament URL `#/tournaments/<id>` and restored deep links.

### Pairings and play

- [x] Pairings visible at the 90-second first-round lock.
- [x] Concurrent check-in confirmations and explicit waiting state.
- [x] No early board/clock through Join round or Open game.
- [x] Ordinary GameScreen reused for tournament rooms.
- [x] Correct opponent names, Resign and Offer draw.
- [x] Spectating for unregistered/eliminated users without player controls.
- [x] No Join/replay action for a bye or unrelated unplayed room.

### Results and recovery

- [x] Swiss scores/Buchholz and clear current-round states.
- [x] Winners Advance stages, wins/draw counts, draw replays and placements.
- [x] Advancement-based placement; wins only resolve final-survivor ties.
- [x] Completed pairings point to their own durable replays.
- [x] Creator correction feedback and reason validation.
- [x] Bounded polling/coalesced reads and isolated pending actions.
- [x] Local tournament acceptance journeys reported passed.

**Acceptance:** round state tells a player what action is available—waiting, checked in, playing, bye, eliminated or finished—not an unexplained generic status. No next stage before all current outcomes resolve.

## Milestone 8 — OAuth and sealed release authentication

- [x] Google logo/button, busy feedback and unavailable-configuration messaging.
- [x] Supabase redirect flow and backend-issued application session.
- [x] AuthClient uses signed, single-use sealed offers for sensitive auth actions.
- [x] Local crypto vendor bundle with pinned integrities and license files.
- [x] Public server pin verified before credentials are transmitted.
- [x] Sealing failures do not fall back to plaintext or queue credentials for reconnect.
- [x] Non-UI tests cover forms, provider failure, sessions and transport interoperability.
- [x] Local Google account/repeat-login/refresh/sign-out checks reported passed.
- [ ] Final deployed OAuth redirects, WSS/CSP and trusted identity validation.
- [ ] Production session/cold-start/reconnect soak beyond access-token expiry.

See [Security](docs/SECURITY.md) for the backend trust model and operational configuration.

## Follow-on A — Mobile-first refinement

Existing responsive support is complete; these are further improvements, not claims that the original growth proposal is fully implemented.

- [ ] Board-first layout with compact clocks and discoverable controls on small screens.
- [ ] Labeled bottom navigation with at most five primary destinations; secondary navigation remains separate.
- [ ] Safe-area spacing and dynamic viewport sizing in portrait/landscape.
- [ ] Comfortable board-adjacent touch targets; aim for 44 CSS pixels where practical, while auditing WCAG web target-size requirements.
- [ ] Explicit zoom/text-scaling and long-name overflow checks.

**Acceptance matrix:** 375 / 768 / 1024 / 1440 / 1920 CSS-pixel viewports, portrait/landscape, tap and drag. A tilted playable board is deferred unless inverse-transform hit testing is exact; the default board remains flat.

## Follow-on B — Depth and motion

Landing effects exist; a shared effect system and comprehensive effects settings are not yet delivered.

- [ ] Consolidate elevation/motion tokens and reusable opt-in effects.
- [ ] Full / Reduced / Off controls with reduced-motion respected by default.
- [ ] Disable pointer tracking on touch and decorative loops during live play.
- [ ] Use transform/opacity feedback that never blocks input or determines game correctness.
- [ ] Consider one restrained result reveal only after profiling.

**Acceptance:** no decorative live-game animation loop, no added long tasks over 50 ms during interaction, and immediate cancellation when preferences/routes change. Do not promise an exact animation-loop count without measuring the current client.

## Follow-on C — Belonging and synchronized progression

Watch/profile/leaderboard/tournaments already use real data. The remaining growth surfaces need their own contracts and privacy decisions.

- [ ] Landing live-game ticker using real game snapshots.
- [ ] Throttled presence counters—hide when unknown.
- [ ] Shareable username profile routes, subject to profile visibility rules.
- [ ] Time-class leaderboard tabs and current-user placement if supported by storage.
- [ ] Landing entry for currently open tournaments.
- [ ] Cross-device puzzle/streak progress and server-managed bot catalog.

## Follow-on D — Onboarding and full board accessibility

- [ ] Dismissible first-game clock/turn guidance and beginner help.
- [ ] Keyboard square navigation and select/drop play with a visible focus ring.
- [ ] Screen-reader SAN announcements and a textual/table position alternative to Canvas.
- [ ] Contrast audit: 4.5:1 normal text, 3:1 large text and relevant UI graphics/states.
- [ ] Complete one full game with keyboard only and verify screen-reader flow.
- [ ] Keep password-manager/paste support, stable focus and actionable error recovery.

Existing form/navigation accessibility is not a claim of full board accessibility compliance.

## Backend integration ledger

Use the [protocol reference](docs/PROTOCOL.md) and source factories for exact fields. These are capabilities, not fabricated future JSON examples.

| Capability | Current integration | Remaining extension |
| --- | --- | --- |
| Presence | No authoritative public counter contract | Throttled counters and UI |
| Guest identity | Online play requires authentication | Issued guest sessions and safe account claim |
| Invite games | Create/join URLs work | Private visibility and scoped invite tokens |
| Rematch | Server-authoritative ordinary-game offers | Not available for tournament rounds |
| Draw agreement | Offer/response with expiry | No client-only substitute |
| Puzzle attempts | Bundled/local | Authoritative attempts and account progression |
| Streak/goals | Browser storage | Cross-device persistence |
| Bot choices | Client catalog maps difficulty | Managed catalog/statistics |
| Live games | Real list and spectate routes | Landing ticker and presence integration |
| Profiles/history | Real username-based queries | Dedicated public profile route/privacy controls |
| Result sharing/replay | Saved game data and client share helpers | Rich persisted analysis summaries |
| Tournament lifecycle | Scheduled Swiss/Winners Advance | Production monitoring and larger-field validation |

## Performance and verification policy

- Treat 250 KiB compressed initial transfer and CLS below 0.1 as goals to measure, not completed benchmark results.
- Reserve space for async content; lazy-load nonessential route data/assets.
- Keep interaction work within an approximately 16 ms frame budget where possible; audit tasks longer than 50 ms.
- Maintain explicit pending/retry state rather than overlapping uncontrolled polling.
- Run frontend regression suites after lifecycle/protocol changes.
- Use narrow manual checks for changed visual behavior; preserve unrelated passed workflows.
- Verify production origins, actual browser crypto, reconnect and OAuth after deployment.

## Release handoff

- [x] Static runtime assets and required crypto bundle identified.
- [x] Public configuration separated from private backend secrets.
- [x] Local acceptance results recorded and automated regressions available.
- [ ] Production backend URL and CSP origin installed.
- [ ] Production OAuth redirect allowlist configured.
- [ ] Deployed password/Google login, gameplay, tournament/replay recovery and session soak verified.

Optional growth work is not a prerequisite for deploying the current release. Production trust/configuration checks are.
