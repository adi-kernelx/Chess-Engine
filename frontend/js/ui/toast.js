/**
 * toast.js — Non-blocking notifications.
 * Replaces native alert() / confirm() error surfaces.
 */

import { h, clear } from '../core/dom.js';

export class Toaster {
    constructor(region) {
        this.region = region;
        this._nextId = 0;
    }

    /**
     * Show a toast.
     * @param {string} message
     * @param {Object} opts - { variant: 'info'|'success'|'warning'|'danger',
     *                          title?: string, duration?: number ms, sticky?: bool }
     */
    show(message, opts = {}) {
        const variant = opts.variant || 'info';
        const duration = opts.duration ?? 4200;
        const id = ++this._nextId;

        const closeBtn = h('button', {
            class: 'toast__close',
            'aria-label': 'Dismiss',
            onclick: () => this.dismiss(id),
        }, '×');

        const el = h('div', {
            class: `toast toast--${variant}`,
            role: 'status',
            dataset: { toastId: String(id) },
        },
            h('div', { class: 'toast__body' },
                opts.title ? h('div', { class: 'toast__title' }, opts.title) : null,
                h('div', { class: 'toast__message' }, message)
            ),
            closeBtn
        );

        this.region.appendChild(el);

        if (!opts.sticky) {
            setTimeout(() => this.dismiss(id), duration);
        }
        return id;
    }

    info(message, opts)    { return this.show(message, { ...opts, variant: 'info' }); }
    success(message, opts) { return this.show(message, { ...opts, variant: 'success' }); }
    warning(message, opts) { return this.show(message, { ...opts, variant: 'warning' }); }
    danger(message, opts)  { return this.show(message, { ...opts, variant: 'danger' }); }

    dismiss(id) {
        const el = this.region.querySelector(`[data-toast-id="${id}"]`);
        if (!el || el.classList.contains('is-leaving')) return;
        el.classList.add('is-leaving');
        setTimeout(() => el.remove(), 220);
    }

    clearAll() { clear(this.region); }
}
