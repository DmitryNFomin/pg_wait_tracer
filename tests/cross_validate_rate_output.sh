#!/usr/bin/env bash
# Labels for informational per-rate cross-validation output.

rate_label() {
    if [[ "$1" == 10 ]]; then
        echo "shipped default"
    else
        echo "exploratory rate"
    fi
}

print_rate_result() {
    local rate="$1" output="$2" label
    label=$(rate_label "$rate")
    printf '%s\n' "$output" | sed \
        "s/^RESULT: PASS$/RATE ${rate}Hz (${label}): within tolerance (characterisation, non-gating)/; s/^RESULT: FAIL$/RATE ${rate}Hz (${label}): outside tolerance (characterisation, non-gating)/; s/^/  /"
}

print_rate_summary() {
    local rate="$1" result="$2" verdict label
    label=$(rate_label "$rate")
    case "$result" in
        PASS) verdict="within tolerance" ;;
        FAIL) verdict="outside tolerance" ;;
        *) verdict="no result" ;;
    esac
    printf '  RATE %sHz (%s): %s (characterisation, non-gating)\n' "$rate" "$label" "$verdict"
}
