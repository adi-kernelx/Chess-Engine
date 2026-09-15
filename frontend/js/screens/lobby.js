/**
 * lobby.js — The landing screen.
 *
 * Four actions in one place:
 *   1. Quick Play        (matchmaker)
 *   2. Create Game       (post an open game and wait for a joiner)
 *   3. Play vs AI        (immediate game against the engine)
 *   4. Open Games list   (join someone else's posted game)
 *
 * Everything talks through ctx.socket + Outbound. On a successful
 * game start the router navigates to #/game and store.game is set.
 */

import { Screen } from '../ui/screen.js';
import { h, q, icon, clear } from '../core/dom.js';
import { parseAndFormatTimeControl, timeCategory, initials } from '../core/format.js';
import { absoluteUrl, copyText, shareUrl } from '../core/share.js';
import { BOTS, DEFAULT_BOT_ID, botById } from '../core/bots.js';

const TIME_PRESETS = [
    { label: '1+0',    base: 60,  inc: 0  },
    { label: '3+2',    base: 180, inc: 2  },
    { label: '5+3',    base: 300, inc: 3  },
    { label: '10+5',   base: 600, inc: 5  },
    { label: '15+10',  base: 900, inc: 10 },
];
// Kept for older builds; the bot ladder (G3) replaces this in the UI but the
// difficulty enum is still what the wire protocol sends.
const DIFFICULTIES = ['easy', 'medium', 'hard', 'max'];
const DEFAULT_PRESET_IDX = 3; // 10+5

export class LobbyScreen extends Screen {
    constructor(ctx) {
        super(ctx);
        this._quickPreset = DEFAULT_PRESET_IDX;
        this._createPreset = DEFAULT_PRESET_IDX;
        this._aiPreset = DEFAULT_PRESET_IDX;
        this._aiBotId = DEFAULT_BOT_ID;
        this._aiDifficulty = botById(this._aiBotId).difficulty;
        this._inQueue = false;
        this._queueSize = 0;
        this._refreshTimer = null;
    }

    render() {
        const { session } = this.ctx.store;
        const username = session.username || 'Player';

        return h('div', { class: 'screen' },
            this.header('Play',
                'Pick a mode and challenge someone — or the engine.',
                h('div', { style: { display: 'flex', gap: '8px', alignItems: 'center' } },
                    h('span', { class: 'badge' }, 'Rating ' + (session.elo || 1200))
                )
            ),
            h('div', { class: 'screen__body' },
                h('div', { class: 'lobby-grid' },

                    // ── Username field spanning both columns ──
                    h('div', { class: 'lobby-grid__full' },
                        h('div', { class: 'card' },
                            h('div', { class: 'card__body' },
                                h('div', { class: 'field' },
                                    h('label', { class: 'field__label', for: 'lobby-username' }, 'Your name'),
                                    h('input', {
                                        id: 'lobby-username',
                                        class: 'input',
                                        maxlength: 24,
                                        value: username,
                                        oninput: (e) => this.ctx.store.setSession({ username: e.target.value.trim() || 'Player' }),
                                        ref: (el) => this._usernameInput = el,
                                    }),
                                    h('div', { class: 'field__hint' }, 'Used for the game record — no account required.')
                                )
                            )
                        )
                    ),

                    // ── Quick Play ──
                    h('div', { class: 'card' },
                        h('div', { class: 'card__header' },
                            icon('play', 'icon--lg', 'style="color: var(--accent);"'),
                            h('div', { class: 'card__title' }, 'Quick Play'),
                        ),
                        h('div', { class: 'card__body card__body--stack' },
                            h('div', { class: 'card-hint' }, 'Get matched with an opponent by rating.'),
                            this._presetPicker('quick', TIME_PRESETS, this._quickPreset, (i) => { this._quickPreset = i; }),
                            h('div', { ref: el => this._quickAction = el }, this._renderQuickAction())
                        )
                    ),

                    // ── Create Game ──
                    h('div', { class: 'card' },
                        h('div', { class: 'card__header' },
                            icon('trophy', 'icon--lg', 'style="color: var(--accent);"'),
                            h('div', { class: 'card__title' }, 'Create Game'),
                        ),
                        h('div', { class: 'card__body card__body--stack' },
                            h('div', { class: 'card-hint' }, 'Post an open game and wait for someone to join.'),
                            this._presetPicker('create', TIME_PRESETS, this._createPreset, (i) => { this._createPreset = i; }),
                            h('button', {
                                class: 'btn btn--primary card-action',
                                onclick: () => this._onCreate(),
                                ref: el => this._createBtn = el,
                            }, 'Create game')
                        )
                    ),

                    // ── Play vs AI ──
                    h('div', { class: 'card' },
                        h('div', { class: 'card__header' },
                            icon('spark', 'icon--lg', 'style="color: var(--accent);"'),
                            h('div', { class: 'card__title' }, 'Play vs AI'),
                        ),
                        h('div', { class: 'card__body card__body--stack' },
                            h('div', { class: 'card-hint' }, 'Instant game. Pick an opponent from the ladder.'),
                            h('div', { class: 'field' },
                                h('div', { class: 'field__label' }, 'Opponent'),
                                h('div', { class: 'bot-ladder' },
                                    ...BOTS.map(bot => this._botCard(bot))
                                )
                            ),
                            this._presetPicker('ai', TIME_PRESETS, this._aiPreset, (i) => { this._aiPreset = i; }),
                            h('button', {
                                class: 'btn btn--primary card-action',
                                onclick: () => this._onPlayAI(),
                                ref: el => this._aiBtn = el,
                            }, 'Start AI game')
                        )
                    ),

                    // ── Blank spacer so open-games sits below quick play & AI ──
                    h('div', { class: 'lobby-grid__full' },
                        h('div', { class: 'card' },
                            h('div', { class: 'card__header' },
                                icon('eye', 'icon--lg', 'style="color: var(--accent);"'),
                                h('div', { class: 'card__title' }, 'Open games'),
                                h('span', { class: 'games-count', ref: el => this._gameCount = el }, '0 games')
                            ),
                            h('div', { style: { padding: 0 } },
                                h('div', { ref: el => this._gamesList = el })
                            )
                        )
                    ),
                )
            )
        );
    }

    _presetPicker(key, presets, activeIdx, onSelect) {
        return h('div', { class: 'field' },
            h('div', { class: 'field__label' }, 'Time control'),
            h('div', { class: 'pill-group', dataset: { presetKey: key } },
                ...presets.map((p, i) =>
                    h('button', {
                        class: 'pill-group__item' + (i === activeIdx ? ' is-active' : ''),
                        dataset: { presetIdx: String(i) },
                        onclick: (e) => this._selectPreset(key, i, e.currentTarget, onSelect),
                    }, p.label)
                )
            )
        );
    }

    _selectPreset(key, idx, btn, onSelect) {
        onSelect(idx);
        for (const el of btn.parentElement.children) el.classList.remove('is-active');
        btn.classList.add('is-active');
    }
    _selectDifficulty(d, btn) {
        // Legacy — the bot ladder now drives this. Kept in case the future
        // custom-strength picker wants a plain difficulty enum.
        this._aiDifficulty = d;
        for (const el of btn.parentElement.children) el.classList.remove('is-active');
        btn.classList.add('is-active');
    }

    _botCard(bot) {
        const active = bot.id === this._aiBotId;
        return h('button', {
            type: 'button',
            class: 'bot-card' + (active ? ' is-active' : ''),
            dataset: { botId: bot.id },
            onclick: (e) => this._selectBot(bot, e.currentTarget),
            title: bot.blurb,
            'data-tilt': '',
        },
            h('span', { class: 'bot-card__avatar', style: { color: bot.accent } }, bot.name[0]),
            h('div', { class: 'bot-card__body' },
                h('div', { class: 'bot-card__name' }, bot.name),
                h('div', { class: 'bot-card__elo mono' }, String(bot.elo)),
            ),
            h('div', { class: 'bot-card__blurb' }, bot.blurb),
        );
    }

    _selectBot(bot, btnEl) {
        this._aiBotId = bot.id;
        this._aiDifficulty = bot.difficulty;
        for (const el of btnEl.parentElement.children) el.classList.remove('is-active');
        btnEl.classList.add('is-active');
    }

    onMount() {
        const { socket, capability, Outbound } = this.ctx;

        // Wire server responses.
        this.sub(socket.on('game_created',  raw => this._onGameCreated(raw)));
        this.sub(socket.on('game_start',    raw => this._onGameStart(raw)));
        this.sub(socket.on('game_joined',   raw => this._onGameJoined(raw)));
        this.sub(socket.on('match_found',   raw => this._onMatchFound(raw)));
        this.sub(socket.on('queued',        raw => this._onQueued(raw)));
        this.sub(socket.on('queue_cancelled', () => this._onQueueCancelled()));
        this.sub(socket.on('game_list',     raw => this._renderGamesList(this.ctx.Inbound.normalize(raw))));
        this.sub(socket.on('error',         raw => this._onError(raw)));

        // Update state on connect/disconnect.
        this.sub(socket.onState((s) => {
            const disabled = s !== 'connected';
            if (this._createBtn) this._createBtn.disabled = disabled;
            if (this._aiBtn)     this._aiBtn.disabled = disabled;
            this._renderQuickActionInto();
            if (s === 'connected') this._refreshGamesList();
        }));

        // Poll the open games list every 5s while on the lobby.
        this._refreshTimer = this.interval(() => this._refreshGamesList(), 5000);
        this._refreshGamesList();
        this._renderGamesList({ games: [] });
        this._renderQuickActionInto();

        // Reflect current connection state on buttons.
        const s = socket.state;
        const disabled = s !== 'connected';
        if (this._createBtn) this._createBtn.disabled = disabled;
        if (this._aiBtn)     this._aiBtn.disabled = disabled;

        // Focus the username so first-time visitors can just type.
        if (this._usernameInput) this._usernameInput.focus({ preventScroll: true });

        // G1: respect the landing screen's mode hint. Deferred so it runs
        // AFTER router.onChange refocuses #outlet — otherwise our focus
        // gets clobbered a tick later.
        try {
            const hint = sessionStorage.getItem('play_focus');
            if (hint) {
                sessionStorage.removeItem('play_focus');
                this.timeout(() => {
                    const btn = hint === 'ai'     ? this._aiBtn
                              : hint === 'create' ? this._createBtn
                              : hint === 'quick'  ? this._quickAction && this._quickAction.querySelector('button')
                              : null;
                    if (btn) {
                        btn.focus({ preventScroll: false });
                        btn.scrollIntoView({ behavior: 'smooth', block: 'center' });
                    }
                }, 0);
            }
        } catch (_) { /* sessionStorage may be blocked */ }
    }

    /* ── Actions ─────────────────────────────────────────── */

    _username() { return (this.ctx.store.session.username || 'Player').slice(0, 24); }

    _onCreate() {
        const { socket, Outbound } = this.ctx;
        const preset = TIME_PRESETS[this._createPreset];
        this._pending = { kind: 'create', preset };
        socket.send(Outbound.createGame(this._username(), preset.base, preset.inc));
    }

    _onPlayAI() {
        const { socket, Outbound } = this.ctx;
        const preset = TIME_PRESETS[this._aiPreset];
        this._pending = { kind: 'ai', preset, difficulty: this._aiDifficulty };
        socket.send(Outbound.playAI(this._username(), this._aiDifficulty, preset.base, preset.inc));
    }

    _onQuickPlay() {
        const { socket, Outbound, store } = this.ctx;
        const preset = TIME_PRESETS[this._quickPreset];
        this._pending = { kind: 'quick', preset };
        socket.send(Outbound.quickPlay(this._username(), store.session.elo || 1200, preset.base, preset.inc));
        this._inQueue = true;
        this._renderQuickActionInto();
    }

    _onCancelQueue() {
        this.ctx.socket.send(this.ctx.Outbound.cancelQueue());
    }

    _joinGame(gameId) {
        const { socket, Outbound } = this.ctx;
        this._pending = { kind: 'join' };
        socket.send(Outbound.joinGame(this._username(), gameId));
    }

    _refreshGamesList() {
        if (this.ctx.socket.isConnected()) {
            this.ctx.socket.send(this.ctx.Outbound.listGames());
        }
    }

    /* ── Server responses ────────────────────────────────── */

    _startFrom(pending, norm) {
        const preset = (pending && pending.preset) || TIME_PRESETS[this._createPreset];
        this.ctx.store.setGame({
            gameId:   norm.gameId,
            color:    norm.color === 'black' ? 'b' : 'w',
            opponent: norm.opponent || (norm.isAI ? 'AI' : 'Opponent'),
            isAI:     !!norm.isAI,
            whiteMs:  norm.whiteMs,
            blackMs:  norm.blackMs,
            timeBaseSec: preset.base,
            timeIncSec:  preset.inc,
            difficulty: pending && pending.difficulty || null,
        });
        this.ctx.router.go('/game');
    }

    _onGameCreated(raw) {
        const norm = this.ctx.Inbound.normalize(raw);
        this._createdGameId = norm.gameId;
        // G2: surface the invite link + a copy button as a persistent panel
        // (not a toast — toasts disappear right when a user wants to click
        // Copy). See _renderInvitePanel below.
        this._renderInvitePanel(norm.gameId);
        this.ctx.toast.info(`Game #${norm.gameId} posted — share the link to bring a friend.`,
            { duration: 3200 });
    }

    _renderInvitePanel(gameId) {
        // Prefer to reuse the games-list container so this sits where the
        // eye already is; if it's not there, no-op cleanly.
        if (!this._gamesList) return;
        const link = absoluteUrl('#/join/' + gameId);
        const panel = h('div', { class: 'invite-panel' },
            h('div', { class: 'invite-panel__body' },
                h('div', {},
                    h('div', { class: 'invite-panel__title' }, 'Waiting for a friend'),
                    h('div', { class: 'invite-panel__sub' }, 'Share this link — it opens straight into the game.'),
                ),
                h('div', { class: 'invite-row' },
                    h('input', {
                        class: 'input mono invite-row__input',
                        readonly: true,
                        value: link,
                        onclick: (e) => e.target.select(),
                    }),
                    h('button', {
                        class: 'btn btn--sm btn--primary',
                        onclick: async () => {
                            const shared = await shareUrl({
                                title: 'Play me in chess',
                                text: 'Join my game — one click, no signup.',
                                url: link,
                            });
                            if (shared.ok) return;
                            const res = await copyText(link);
                            if (res.ok) this.ctx.toast.success('Link copied.', { duration: 1800 });
                            else this.ctx.toast.warning('Copy blocked — select the field manually.', { duration: 2600 });
                        },
                    }, 'Copy link'),
                ),
            ),
        );
        clear(this._gamesList);
        this._gamesList.appendChild(panel);
    }

    _onGameJoined(raw) {
        const norm = this.ctx.Inbound.normalize(raw);
        this._startFrom(this._pending, norm);
    }

    _onGameStart(raw) {
        const norm = this.ctx.Inbound.normalize(raw);
        this._startFrom(this._pending, norm);
    }

    _onMatchFound(raw) {
        const norm = this.ctx.Inbound.normalize(raw);
        this._inQueue = false;
        this._renderQuickActionInto();
        this._startFrom(this._pending, norm);
    }

    _onQueued(raw) {
        const norm = this.ctx.Inbound.normalize(raw);
        this._inQueue = true;
        this._queueSize = norm.queueSize || 0;
        this._renderQuickActionInto();
    }

    _onQueueCancelled() {
        this._inQueue = false;
        this._renderQuickActionInto();
        this.ctx.toast.info('Left the queue.', { duration: 2200 });
    }

    _onError(raw) {
        const norm = this.ctx.Inbound.normalize(raw);
        // Suppress the "Unknown message type" reply which capability.js swallows.
        if (norm.isUnknownType) return;
        this.ctx.toast.danger(norm.message, { title: 'Server' });
    }

    /* ── Quick-play button rendering ─────────────────────── */

    _renderQuickAction() {
        if (this._inQueue) {
            return h('div', {},
                h('div', { class: 'queue-status' },
                    h('span', { class: 'queue-status__spinner' }),
                    h('span', {}, `Waiting for an opponent — ${this._queueSize} in queue`)
                ),
                h('button', {
                    class: 'btn btn--danger card-action',
                    style: { marginTop: 'var(--sp-3)' },
                    onclick: () => this._onCancelQueue(),
                }, 'Cancel')
            );
        }
        return h('button', {
            class: 'btn btn--primary card-action',
            onclick: () => this._onQuickPlay(),
            disabled: !this.ctx.socket.isConnected(),
        }, 'Find match');
    }

    _renderQuickActionInto() {
        if (!this._quickAction) return;
        clear(this._quickAction);
        this._quickAction.appendChild(this._renderQuickAction());
    }

    /* ── Open games list ─────────────────────────────────── */

    _renderGamesList(payload) {
        if (!this._gamesList) return;
        clear(this._gamesList);
        const games = payload.games || [];
        // Hide the game you just created from your own list.
        const filtered = games.filter(g => g.gameId !== this._createdGameId);
        this._gameCount.textContent = `${filtered.length} game${filtered.length === 1 ? '' : 's'}`;

        if (filtered.length === 0) {
            // G1 empty-state rewrite: active recruit, not passive dead-end.
            this._gamesList.appendChild(
                h('div', { class: 'empty empty--recruit' },
                    icon('play', 'icon--xl empty__icon'),
                    h('div', { class: 'empty__title' }, 'Nobody’s waiting right now.'),
                    h('div', { class: 'empty__sub' }, 'Post a game and we’ll ping you the moment someone joins — or play a bot while you wait.'),
                    h('div', { class: 'empty__actions' },
                        h('button', {
                            class: 'btn btn--primary btn--sm',
                            onclick: () => this._onCreate(),
                        }, 'Post a game'),
                        h('button', {
                            class: 'btn btn--ghost btn--sm',
                            onclick: () => this._onPlayAI(),
                        }, 'Play a bot'),
                    )
                )
            );
            return;
        }

        const table = h('table', { class: 'table' },
            h('thead', {},
                h('tr', {},
                    h('th', {}, 'Host'),
                    h('th', {}, 'Time control'),
                    h('th', {}, 'Category'),
                    h('th', { style: { textAlign: 'right' } }, ''),
                )),
            h('tbody', {},
                ...filtered.map(g => this._gameRow(g))
            )
        );
        const wrap = h('div', { class: 'table-wrap' }, table);
        this._gamesList.appendChild(wrap);
    }

    _gameRow(g) {
        const [baseStr, incStr] = String(g.timeControl || '').split('+');
        const base = parseInt(baseStr, 10) || 0;
        const inc  = parseInt(incStr, 10)  || 0;
        return h('tr', {},
            h('td', {},
                h('div', { style: { display: 'flex', alignItems: 'center', gap: '10px' } },
                    h('div', { class: 'avatar avatar--sm' }, initials(g.host)),
                    h('span', {}, g.host)
                )
            ),
            h('td', { class: 'mono' }, parseAndFormatTimeControl(g.timeControl)),
            h('td', {}, h('span', { class: 'badge' }, timeCategory(base, inc))),
            h('td', { style: { textAlign: 'right' } },
                h('button', {
                    class: 'btn btn--sm btn--primary',
                    onclick: () => this._joinGame(g.gameId),
                }, 'Join')
            ),
        );
    }
}
