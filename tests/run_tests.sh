#!/usr/bin/env bash
#
# Basic invariant tests for abulafia.
#
# The program's output is randomized, so these are not classic input/output
# unit tests. Instead they check properties that must always hold: the
# binary builds, edge cases exit with the expected status, and generated
# answers only ever use words that actually appear in the source corpus
# (which would catch, e.g., a memory-corruption or tokenization bug).

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BIN="$ROOT_DIR/abulafia"
CORPUS="$SCRIPT_DIR/mini_corpus.txt"
VOCAB="$(mktemp)"
WORKDIR="$(mktemp -d)"

FAILURES=0

pass() { printf '  PASS: %s\n' "$1"; }
fail() {
    printf '  FAIL: %s\n' "$1"
    FAILURES=$((FAILURES + 1))
}

cleanup() { rm -rf "$VOCAB" "$WORKDIR"; }
trap cleanup EXIT

echo "==> Building abulafia"
if ! make -C "$ROOT_DIR" >/dev/null; then
    fail "build (make) failed"
    exit 1
fi

# Vocabulary of the mini corpus, tokenized the same way the program does
# (split on whitespace, punctuation stays attached to words).
tr -s '[:space:]' '\n' <"$CORPUS" | sed '/^$/d' | sort -u >"$VOCAB"

echo "==> Checking generated answers only use known words"
QUESTIONS=(
    "where does the cat sleep?"
    "tell me about the garden"
    "this question shares no words with the corpus at all"
)
for q in "${QUESTIONS[@]}"; do
    for run in 1 2 3; do
        # The prompt ("You: ") and the generated answer ("> ...") are
        # printed on the same line, with no newline in between.
        answer="$(printf '%s\nexit\n' "$q" | "$BIN" "$CORPUS" 2>/dev/null | sed -n 's/^You: > //p')"
        if [ -z "$answer" ]; then
            fail "no answer produced for question '$q' (run $run)"
            continue
        fi
        bad_word=""
        for word in $answer; do
            if ! grep -Fxq "$word" "$VOCAB"; then
                bad_word="$word"
                break
            fi
        done
        if [ -n "$bad_word" ]; then
            fail "answer to '$q' (run $run) contains unknown word '$bad_word': $answer"
        fi
    done
done
[ "$FAILURES" -eq 0 ] && pass "all generated words belong to the corpus vocabulary"

echo "==> Checking edge cases"

# Empty file: too little text, must exit non-zero.
: >"$WORKDIR/empty.txt"
if "$BIN" "$WORKDIR/empty.txt" </dev/null >/dev/null 2>&1; then
    fail "empty file should exit non-zero"
else
    pass "empty file exits non-zero"
fi

# Too-short file (fewer than 3 tokens): same as above.
printf 'one two\n' >"$WORKDIR/short.txt"
if "$BIN" "$WORKDIR/short.txt" </dev/null >/dev/null 2>&1; then
    fail "too-short file should exit non-zero"
else
    pass "too-short file exits non-zero"
fi

# Nonexistent file: must exit non-zero.
if "$BIN" "$WORKDIR/does-not-exist.txt" </dev/null >/dev/null 2>&1; then
    fail "nonexistent file should exit non-zero"
else
    pass "nonexistent file exits non-zero"
fi

# "exit", "quit" and plain EOF must all terminate cleanly (exit 0).
for input in "exit" "quit" ""; do
    if printf '%s\n' "$input" | "$BIN" "$CORPUS" >/dev/null 2>&1; then
        pass "input '$input' exits 0"
    else
        fail "input '$input' should exit 0"
    fi
done

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "All tests passed."
    exit 0
else
    echo "$FAILURES test(s) failed."
    exit 1
fi
