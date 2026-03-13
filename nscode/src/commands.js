// Command registry, keybinding matching, and fuzzy search.

// ── Fuzzy match ───────────────────────────────────────────────────────────────

export function fuzzy_score(query, text) {
    if (!query) return 1;
    const q = query.toLowerCase(), t = text.toLowerCase();
    if (t.includes(q)) return 2 + q.length / (t.length + 1);
    let qi = 0, score = 0;
    for (let ti = 0; ti < t.length && qi < q.length; ti++) {
        if (t[ti] === q[qi]) { score++; qi++; }
    }
    return qi === q.length ? score / (t.length + 1) : 0;
}

export function fuzzy_filter(query, items, key = x => x) {
    if (!query) return items.slice();
    return items
        .map(it => ({ it, s: fuzzy_score(query, key(it)) }))
        .filter(x => x.s > 0)
        .sort((a, b) => b.s - a.s)
        .map(x => x.it);
}

// ── Key ID from KeyboardEvent ─────────────────────────────────────────────────

export function event_key_id(e) {
    const parts = [];
    if (e.ctrlKey || e.metaKey) parts.push('ctrl');
    if (e.altKey)               parts.push('alt');
    if (e.shiftKey)             parts.push('shift');
    // Single printable chars: lowercase. Special keys: as-is (e.g. 'Enter', 'ArrowUp')
    parts.push(e.key.length === 1 ? e.key.toLowerCase() : e.key);
    return parts.join('+');
}

const DISPLAY_MAP = {
    ctrl: '⌘', alt: 'Alt', shift: '⇧',
    Enter: '↵', Tab: '⇥', Escape: 'Esc',
    ArrowUp: '↑', ArrowDown: '↓', ArrowLeft: '←', ArrowRight: '→',
    Backspace: '⌫', Delete: 'Del',
};

function fmt_key(k) {
    return k.split('+')
        .map(p => DISPLAY_MAP[p] ?? (p.length === 1 ? p.toUpperCase() : p))
        .join('');
}

// ── keymap ────────────────────────────────────────────────────────────────────

/**
 * keymap: maps keyboard events to command ids.
 * Supports localStorage overrides for customization.
 *
 * defaults: [{ id, label, category, keys: string[] }]
 */
export class key_map {
    constructor(defaults) {
        this._defaults = defaults;
        this._ov = (() => {
            try { return JSON.parse(localStorage.getItem('ns_keys') ?? '{}'); }
            catch { return {}; }
        })();
        this._rebuild();
    }

    /** Returns command id for the event, or null. */
    match(e) { return this._map.get(event_key_id(e)) ?? null; }

    /** Primary display string for a command's keybinding. */
    display(id) {
        const keys = this._eff(id);
        return keys.length ? fmt_key(keys[0]) : '';
    }

    /** All commands with their effective (possibly overridden) keys. */
    get_all() {
        return this._defaults.map(d => ({ ...d, keys: this._eff(d.id) }));
    }

    /** Persist a new key binding for a command. */
    override(id, keys) {
        this._ov[id] = keys;
        localStorage.setItem('ns_keys', JSON.stringify(this._ov));
        this._rebuild();
    }

    /** Reset a command to its default binding. */
    reset(id) {
        delete this._ov[id];
        localStorage.setItem('ns_keys', JSON.stringify(this._ov));
        this._rebuild();
    }

    _eff(id) {
        const def = this._defaults.find(d => d.id === id);
        return this._ov[id] ?? def?.keys ?? [];
    }

    _rebuild() {
        this._map = new Map();
        for (const d of this._defaults) {
            for (const k of this._eff(d.id)) this._map.set(k, d.id);
        }
    }
}
