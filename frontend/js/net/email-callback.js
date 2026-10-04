// Capture an email-link bearer token in memory, then remove it from history.
// Opening a link never consumes it: the user must confirm on the screen.
export function consumeEmailCallback(location, history) {
    const match=(location.hash||'').match(/^#\/(verify-email|activate-account|reset-password)\/(.*)$/);
    if(!match) return null;
    history.replaceState(null,'',location.pathname+location.search+'#/'+match[1]);
    return { mode: match[1], token: /^[A-Za-z0-9_-]{43}$/.test(match[2]) ? match[2] : '' };
}
