#!/usr/bin/env bash
# Shared configuration helpers. Sourced by install.sh and update.sh; never eval the options file.
sysdvr_get_language() {
    local file="$1" value
    if [ ! -r "$file" ]; then printf 'en\n'; return; fi
    value="$(sed -nE 's/^[[:space:]]*SYSDVR_LANGUAGE=["\x27]?(en|ru)["\x27]?[[:space:]]*$/\1/p' "$file" 2>/dev/null | tail -1)"
    case "$value" in en|ru) printf '%s\n' "$value" ;; *) printf 'en\n' ;; esac
}
sysdvr_set_language() {
    local file="$1" language="$2" temp
    case "$language" in en|ru) ;; *) echo 'error: language must be en or ru' >&2; return 1 ;; esac
    temp="$(mktemp "${file}.XXXXXX")" || return 1
    if [ -f "$file" ]; then
        if ! awk '!/^[[:space:]]*SYSDVR_LANGUAGE[[:space:]]*=/' "$file" > "$temp"; then rm -f "$temp"; return 1; fi
        chmod --reference="$file" "$temp"
        chown --reference="$file" "$temp"
    else
        chmod 644 "$temp"
    fi
    printf 'SYSDVR_LANGUAGE=%s\n' "$language" >> "$temp"
    mv -f "$temp" "$file"
}
