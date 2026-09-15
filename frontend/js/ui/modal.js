/**
 * modal.js — Promise-based confirm and generic dialog.
 * Replaces native confirm() / alert().
 */

import { h } from '../core/dom.js';

export class ModalHost {
    constructor() {
        this._activeBackdrop = null;
        this._prevFocus = null;
        this._keyHandler = null;
    }

    /**
     * Confirm dialog.
     * @param {Object} opts - { title, message, confirmLabel, cancelLabel, danger }
     * @returns {Promise<boolean>}
     */
    confirm(opts = {}) {
        const title = opts.title || 'Confirm';
        const confirmLabel = opts.confirmLabel || 'Confirm';
        const cancelLabel = opts.cancelLabel || 'Cancel';

        return new Promise(resolve => {
            const close = (result) => { this._close(); resolve(result); };
            const confirmBtn = h('button', {
                class: 'btn ' + (opts.danger ? 'btn--danger' : 'btn--primary'),
                onclick: () => close(true),
            }, confirmLabel);
            const cancelBtn = h('button', {
                class: 'btn btn--ghost',
                onclick: () => close(false),
            }, cancelLabel);

            this._open({
                title,
                subtitle: null,
                body: h('div', {}, opts.message || ''),
                footer: h('div', { style: { display: 'flex', gap: '8px' } }, cancelBtn, confirmBtn),
                onDismiss: () => close(false),
                initialFocus: confirmBtn,
            });
        });
    }

    /**
     * Generic dialog. Caller supplies body and footer elements.
     * @param {Object} opts - { title, subtitle?, body: Node, footer: Node, dismissible?: bool }
     * @returns {{ close: () => void }}
     */
    open(opts) {
        this._open({
            title: opts.title,
            subtitle: opts.subtitle,
            body: opts.body,
            footer: opts.footer,
            onDismiss: opts.dismissible === false ? null : () => this._close(),
            initialFocus: opts.initialFocus,
        });
        return { close: () => this._close() };
    }

    _open({ title, subtitle, body, footer, onDismiss, initialFocus }) {
        this._close(); // one modal at a time

        const modal = h('div', {
            class: 'modal',
            role: 'dialog',
            'aria-modal': 'true',
            'aria-label': title,
        },
            h('div', { class: 'modal__header' },
                h('div', { class: 'modal__title' }, title),
                subtitle ? h('div', { class: 'modal__subtitle' }, subtitle) : null
            ),
            h('div', { class: 'modal__body' }, body),
            footer ? h('div', { class: 'modal__footer' }, footer) : null
        );

        const backdrop = h('div', {
            class: 'modal-backdrop',
            onclick: (e) => { if (e.target === backdrop && onDismiss) onDismiss(); },
        }, modal);

        document.body.appendChild(backdrop);
        this._activeBackdrop = backdrop;
        this._prevFocus = document.activeElement;

        this._keyHandler = (e) => {
            if (e.key === 'Escape' && onDismiss) { onDismiss(); return; }
            if (e.key === 'Tab') this._trapFocus(e, modal);
        };
        window.addEventListener('keydown', this._keyHandler);

        // Focus target after paint
        requestAnimationFrame(() => {
            if (initialFocus && typeof initialFocus.focus === 'function') initialFocus.focus();
            else {
                const first = this._focusable(modal)[0];
                if (first) first.focus();
                else { modal.setAttribute('tabindex', '-1'); modal.focus(); }
            }
        });
    }

    _focusable(container) {
        return [...container.querySelectorAll(
            'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])'
        )];
    }

    _trapFocus(e, modal) {
        const els = this._focusable(modal);
        if (els.length === 0) return;
        const first = els[0];
        const last = els[els.length - 1];
        if (e.shiftKey) {
            if (document.activeElement === first || !modal.contains(document.activeElement)) {
                e.preventDefault();
                last.focus();
            }
        } else {
            if (document.activeElement === last || !modal.contains(document.activeElement)) {
                e.preventDefault();
                first.focus();
            }
        }
    }

    _close() {
        if (!this._activeBackdrop) return;
        window.removeEventListener('keydown', this._keyHandler);
        this._activeBackdrop.remove();
        this._activeBackdrop = null;
        this._keyHandler = null;
        if (this._prevFocus && typeof this._prevFocus.focus === 'function') {
            try { this._prevFocus.focus(); } catch (_) {}
        }
        this._prevFocus = null;
    }
}
