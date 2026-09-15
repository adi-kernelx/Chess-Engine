/**
 * share.js — Small helpers for the invite / share loop (Part G2).
 *
 * Every helper is a *progressive enhancement*. If a Web platform API
 * is not available, the helper resolves with `{ ok: false, reason }`
 * so the caller can fall back cleanly. Nothing throws.
 */

/** Absolute URL for a hash route on the current origin. */
export function absoluteUrl(hashPath) {
    const path = hashPath.startsWith('#') ? hashPath : ('#' + hashPath);
    return location.origin + location.pathname + path;
}

/** Copy plain text to the clipboard. Falls back to a hidden textarea + execCommand. */
export async function copyText(text) {
    try {
        if (navigator.clipboard && window.isSecureContext) {
            await navigator.clipboard.writeText(text);
            return { ok: true, via: 'clipboard-api' };
        }
    } catch (_) { /* fall through */ }
    // Legacy fallback (http:// pages, older browsers).
    try {
        const ta = document.createElement('textarea');
        ta.value = text;
        ta.setAttribute('readonly', '');
        ta.style.position = 'absolute';
        ta.style.left = '-9999px';
        document.body.appendChild(ta);
        ta.select();
        const ok = document.execCommand('copy');
        document.body.removeChild(ta);
        return ok ? { ok: true, via: 'execCommand' } : { ok: false, reason: 'execCommand-failed' };
    } catch (e) {
        return { ok: false, reason: e.message };
    }
}

/**
 * Copy a Blob to the clipboard as an image (only PNG is broadly supported).
 * Feature-detects ClipboardItem; returns { ok: false } cleanly on Firefox etc.
 */
export async function copyImage(blob) {
    try {
        if (!navigator.clipboard || !window.ClipboardItem) return { ok: false, reason: 'no-clipboarditem' };
        const item = new ClipboardItem({ [blob.type]: blob });
        await navigator.clipboard.write([item]);
        return { ok: true };
    } catch (e) {
        return { ok: false, reason: e.message };
    }
}

/**
 * Native share via Web Share API where available. On desktop Chrome without
 * an OS share sheet this returns `{ ok: false, reason: 'unsupported' }`
 * so the caller can copy-to-clipboard instead.
 */
export async function shareUrl({ title, text, url }) {
    if (!navigator.share) return { ok: false, reason: 'unsupported' };
    try {
        await navigator.share({ title, text, url });
        return { ok: true };
    } catch (e) {
        // User cancelled the sheet — not an error to report.
        if (e && e.name === 'AbortError') return { ok: false, reason: 'cancelled' };
        return { ok: false, reason: e.message };
    }
}

/**
 * Build a minimal PGN string from a moves array (each element must have `san`).
 * Includes the standard seven-tag roster; empty tags where we don't know.
 * Result is the algebraic pair "1-0" / "0-1" / "1/2-1/2" / "*".
 */
export function buildPGN({ white, black, result = '*', event = 'Casual game', site, date, moves = [], timeControl }) {
    const yyyymmdd = date ? date : formatPgnDate(new Date());
    const tags = [
        ['Event',  event],
        ['Site',   site || location.host || 'Chess Platform'],
        ['Date',   yyyymmdd],
        ['Round',  '-'],
        ['White',  white || '?'],
        ['Black',  black || '?'],
        ['Result', normalizeResult(result)],
    ];
    if (timeControl) tags.push(['TimeControl', timeControl]);

    const header = tags.map(([k, v]) => `[${k} "${escapePgn(String(v))}"]`).join('\n');

    const body = [];
    for (let i = 0; i < moves.length; i += 2) {
        const num = (i / 2 | 0) + 1;
        const w = moves[i]     ? moves[i].san     : '';
        const b = moves[i + 1] ? moves[i + 1].san : '';
        body.push(b ? `${num}. ${w} ${b}` : `${num}. ${w}`);
    }
    return header + '\n\n' + body.join(' ') + ' ' + normalizeResult(result) + '\n';
}

function formatPgnDate(d) {
    const y = d.getFullYear();
    const m = String(d.getMonth() + 1).padStart(2, '0');
    const day = String(d.getDate()).padStart(2, '0');
    return `${y}.${m}.${day}`;
}
function normalizeResult(r) {
    if (r === '1-0' || r === '0-1' || r === '1/2-1/2' || r === '*') return r;
    return '*';
}
function escapePgn(v) { return v.replace(/\\/g, '\\\\').replace(/"/g, '\\"'); }
