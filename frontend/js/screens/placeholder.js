/**
 * placeholder.js — Stand-in for screens not yet implemented.
 *
 * Every screen registered in main.js currently points here; as later
 * parts land, screens/*.js replace individual entries.
 */

import { Screen } from '../ui/screen.js';
import { h, icon } from '../core/dom.js';

export class PlaceholderScreen extends Screen {
    constructor(ctx, { title, subtitle, part }) {
        super(ctx);
        this._title = title;
        this._subtitle = subtitle;
        this._part = part;
    }

    render() {
        return h('div', { class: 'screen' },
            this.header(this._title, this._subtitle),
            h('div', { class: 'screen__body' },
                h('div', { class: 'card' },
                    h('div', { class: 'card__body' },
                        h('div', { class: 'empty' },
                            icon('spark', 'icon--xl empty__icon'),
                            h('div', { style: { fontSize: '15px', color: 'var(--text-secondary)', marginBottom: '4px' } },
                                'Coming in ', this._part
                            ),
                            h('div', {}, 'This screen’s implementation lands in a later build part. The router, shell, and design system are already in place.')
                        )
                    )
                )
            )
        );
    }
}

/** Convenience factory used from route registrations. */
export function placeholder(title, subtitle, part) {
    return (ctx) => new PlaceholderScreen(ctx, { title, subtitle, part });
}
