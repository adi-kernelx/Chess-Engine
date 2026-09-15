/**
 * dom.js — Safe DOM builder.
 *
 * Replaces `innerHTML` and template-string interpolation across the app.
 * All string children are set via `textContent`, so untrusted values
 * (usernames, error messages) can never become markup.
 *
 * Usage:
 *   h('div', { class: 'card', onclick: fn }, 'text')
 *   h('table', {},
 *      h('thead', {}, h('tr', {}, h('th', {}, 'ID'))),
 *      h('tbody', { class: 't-body' }, ...rows)
 *   )
 */

const SVG_NS = 'http://www.w3.org/2000/svg';
const SVG_TAGS = new Set(['svg', 'g', 'path', 'symbol', 'use', 'defs', 'rect', 'circle', 'line', 'polyline', 'polygon', 'text']);

/** Create an element with props and children. */
export function h(tag, props = null, ...children) {
    const el = SVG_TAGS.has(tag)
        ? document.createElementNS(SVG_NS, tag)
        : document.createElement(tag);

    if (props) {
        for (const key in props) {
            const val = props[key];
            if (val == null || val === false) continue;

            if (key === 'class' || key === 'className') {
                el.setAttribute('class', val);
            } else if (key === 'style' && typeof val === 'object') {
                Object.assign(el.style, val);
            } else if (key === 'dataset' && typeof val === 'object') {
                for (const dk in val) el.dataset[dk] = val[dk];
            } else if (key.startsWith('on') && typeof val === 'function') {
                el.addEventListener(key.slice(2).toLowerCase(), val);
            } else if (key === 'ref' && typeof val === 'function') {
                val(el);
            } else if (key in el && !SVG_TAGS.has(tag)) {
                // Property assignment (value, checked, disabled, ...)
                el[key] = val;
            } else {
                el.setAttribute(key, val === true ? '' : String(val));
            }
        }
    }

    appendChildren(el, children);
    return el;
}

function appendChildren(el, children) {
    for (const child of children) {
        if (child == null || child === false) continue;
        if (Array.isArray(child)) {
            appendChildren(el, child);
        } else if (child instanceof Node) {
            el.appendChild(child);
        } else {
            el.appendChild(document.createTextNode(String(child)));
        }
    }
}

/** Convenience: <use> for the icon sprite. */
export function icon(name, extraClass = '') {
    const svg = document.createElementNS(SVG_NS, 'svg');
    svg.setAttribute('class', ('icon ' + extraClass).trim());
    svg.setAttribute('aria-hidden', 'true');
    const use = document.createElementNS(SVG_NS, 'use');
    use.setAttribute('href', '#i-' + name);
    svg.appendChild(use);
    return svg;
}

/** Remove all children. */
export function clear(el) {
    while (el.firstChild) el.removeChild(el.firstChild);
}

/** Replace an element's children in one operation. */
export function mount(el, ...children) {
    clear(el);
    appendChildren(el, children);
    return el;
}

/** Query helpers. */
export const q  = (sel, root = document) => root.querySelector(sel);
export const qa = (sel, root = document) => Array.from(root.querySelectorAll(sel));
