/*
 * abulafia.c
 *
 * Reads a .txt file, builds an order-2 Markov chain (pair of consecutive
 * words -> the words that follow them in the source text) and uses it to
 * generate answers to questions typed interactively by the user.
 *
 * Usage: ./abulafia corpus.txt
 */

/*
 * strdup() is POSIX, not standard C - this feature-test macro exposes it
 * even when compiling with a strict -std=c11 (as opposed to gnu11). Must
 * be defined before any system header is included.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define HASH_SIZE      8192     /* number of buckets in the hash table */
#define MAX_LINE       4096     /* max length of a token read from file / a line typed by the user */
#define MAX_WORD        128     /* max length of a single word we manipulate */
#define GEN_MIN_WORDS     8     /* don't stop generating before this many words, even at a sentence end */
#define GEN_MAX_WORDS    40     /* hard cap on generated words, in case the chain loops forever */
#define MAX_CANDIDATES  256     /* max number of matching chain entries considered when seeding a reply */

/*
 * One node of the Markov chain: the key is the word pair (w1, w2), and
 * "nexts" is the list of words that were observed to follow that pair in
 * the source text. A word can appear multiple times in "nexts" - that
 * repetition is what gives more frequent transitions a higher probability
 * of being picked during generation.
 */
typedef struct Entry {
    char *w1, *w2;
    char **nexts;
    int n_next, cap_next;       /* number of entries used / allocated in "nexts" */
    struct Entry *next;         /* next node in this hash bucket (separate chaining) */
} Entry;

/*
 * Hash table mapping (w1, w2) -> Entry, plus a flat array of every entry
 * (all_entries) so we can iterate or pick one at random without walking
 * the whole table.
 */
static Entry *table[HASH_SIZE];
static Entry **all_entries = NULL;
static int n_entries = 0, cap_entries = 0;

/*
 * All words read from the input file, in order, kept around so build_chain()
 * can slide a 3-word window over them.
 */
static char **tokens = NULL;
static int n_tokens = 0, cap_tokens = 0;

/*
 * djb2 string hash
 * simple and good enough for this purpose.
 */
static unsigned long hash_str(const char *s)
{
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char) *s++))
        h = ((h << 5) + h) + (unsigned long) c;
    return h;
}

/*
 * Combine the hashes of both words of a pair into a single bucket index.
 */
static unsigned long hash_pair(const char *a, const char *b)
{
    return (hash_str(a) * 33) ^ hash_str(b);
}

/*
 * Append one word to the global "tokens" array, growing it (doubling
 * capacity) whenever it runs out of room.
 */
static void add_token(const char *w)
{
    if (n_tokens == cap_tokens) {
        cap_tokens = cap_tokens ? cap_tokens * 2 : 256;
        tokens = realloc(tokens, (size_t) cap_tokens * sizeof(char *));
        if (!tokens) {
            perror("realloc");
            exit(1);
        }
    }
    tokens[n_tokens++] = strdup(w);
}

/*
 * Look up the chain entry for the pair (w1, w2); returns NULL if that pair
 * was never seen in the source text.
 */
static Entry *find_entry(const char *w1, const char *w2)
{
    unsigned long h = hash_pair(w1, w2) % HASH_SIZE;
    for (Entry * e = table[h]; e; e = e->next)
        if (strcmp(e->w1, w1) == 0 && strcmp(e->w2, w2) == 0)
            return e;
    return NULL;
}

/*
 * Return the existing entry for (w1, w2), or create and register a new
 * (empty) one if this pair hasn't been seen before.
 */
static Entry *get_or_create(const char *w1, const char *w2)
{
    unsigned long h = hash_pair(w1, w2) % HASH_SIZE;
    Entry *e = find_entry(w1, w2);
    if (e)
        return e;

    e = malloc(sizeof(Entry));
    e->w1 = strdup(w1);
    e->w2 = strdup(w2);
    e->nexts = NULL;
    e->n_next = e->cap_next = 0;
    e->next = table[h];         /* insert at the head of this bucket's list */
    table[h] = e;

    /* Also track it in the flat array used for random seed selection. */
    if (n_entries == cap_entries) {
        cap_entries = cap_entries ? cap_entries * 2 : 256;
        all_entries = realloc(all_entries, (size_t) cap_entries * sizeof(Entry *));
    }
    all_entries[n_entries++] = e;
    return e;
}

/*
 * Record that word "w" was observed right after entry e's pair (w1, w2).
 */
static void add_next(Entry * e, const char *w)
{
    if (e->n_next == e->cap_next) {
        e->cap_next = e->cap_next ? e->cap_next * 2 : 4;
        e->nexts = realloc(e->nexts, (size_t) e->cap_next * sizeof(char *));
    }
    e->nexts[e->n_next++] = strdup(w);
}

/*
 * Copy src into dst keeping only alphanumeric characters, lowercased.
 * Used to compare words while ignoring case and punctuation (e.g. "Casa,"
 * and "casa" should match).
 */
static void to_lower_clean(const char *src, char *dst, size_t dstsize)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dstsize - 1; i++) {
        unsigned char c = (unsigned char) src[i];
        if (isalnum(c))
            dst[j++] = (char) tolower(c);
    }
    dst[j] = '\0';
}

/*
 * Read the whole input file and split it into whitespace-separated tokens.
 * Punctuation is kept attached to the word it follows (e.g. "casa." stays
 * one token); this is what lets ends_sentence() later detect sentence
 * boundaries without a separate tokenizing pass.
 */
static void load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open file '%s'\n", path);
        exit(1);
    }
    char buf[MAX_LINE];
    while (fscanf(f, "%4095s", buf) == 1) {
        add_token(buf);
    }
    fclose(f);
}

/*
 * Build the Markov chain: slide a window of 3 consecutive tokens over the
 * whole text. For each window (w[i], w[i+1], w[i+2]), record that w[i+2]
 * follows the pair (w[i], w[i+1]).
 */
static void build_chain(void)
{
    for (int i = 0; i + 2 < n_tokens; i++) {
        Entry *e = get_or_create(tokens[i], tokens[i + 1]);
        add_next(e, tokens[i + 2]);
    }
}

/*
 * True if the word's last character is a sentence-ending punctuation mark.
 */
static int ends_sentence(const char *w)
{
    size_t l = strlen(w);
    if (l == 0)
        return 0;
    char c = w[l - 1];
    return c == '.' || c == '?' || c == '!';
}

/*
 * Index from a cleaned (lowercase, alphanumeric-only) first word to every
 * chain entry whose w1 cleans to that same word. Built once, right after
 * the chain itself, so pick_seed() can look up matches directly instead of
 * scanning every entry in the chain for each word in the question.
 */
typedef struct WordIndex {
    char *key;
    Entry **matches;
    int n_matches, cap_matches;
    struct WordIndex *next;
} WordIndex;

static WordIndex *word_index[HASH_SIZE];

/*
 * Look up the word index entry for a cleaned word; returns NULL if no
 * chain entry has that word as its (cleaned) w1.
 */
static WordIndex *find_word_index(const char *key)
{
    unsigned long h = hash_str(key) % HASH_SIZE;
    for (WordIndex * wi = word_index[h]; wi; wi = wi->next)
        if (strcmp(wi->key, key) == 0)
            return wi;
    return NULL;
}

/*
 * Return the existing word index entry for "key", or create and register
 * a new (empty) one if this word hasn't been indexed yet.
 */
static WordIndex *get_or_create_word_index(const char *key)
{
    unsigned long h = hash_str(key) % HASH_SIZE;
    WordIndex *wi = find_word_index(key);
    if (wi)
        return wi;

    wi = malloc(sizeof(WordIndex));
    wi->key = strdup(key);
    wi->matches = NULL;
    wi->n_matches = wi->cap_matches = 0;
    wi->next = word_index[h];
    word_index[h] = wi;
    return wi;
}

/*
 * Build the word index: for every chain entry, register it under its
 * cleaned w1 so pick_seed() can find it directly.
 */
static void build_word_index(void)
{
    char clean[MAX_WORD];
    for (int i = 0; i < n_entries; i++) {
        to_lower_clean(all_entries[i]->w1, clean, sizeof(clean));
        if (!clean[0])
            continue;
        WordIndex *wi = get_or_create_word_index(clean);
        if (wi->n_matches == wi->cap_matches) {
            wi->cap_matches = wi->cap_matches ? wi->cap_matches * 2 : 4;
            wi->matches = realloc(wi->matches, (size_t) wi->cap_matches * sizeof(Entry *));
        }
        wi->matches[wi->n_matches++] = all_entries[i];
    }
}

/*
 * Choose which chain entry to start generating from, based on the user's
 * question. Strategy: split the question into words and look for any of
 * them among the first words (w1) of known chain pairs; if there are
 * matches, pick one of them at random (this "seeds" the answer with a
 * topic related to the question). If nothing matches, fall back to a
 * uniformly random pair from the whole chain, so the program always
 * produces some kind of answer.
 */
static Entry *pick_seed(const char *question)
{
    char qcopy[MAX_LINE];
    strncpy(qcopy, question, sizeof(qcopy) - 1);
    qcopy[sizeof(qcopy) - 1] = '\0';

    char clean[MAX_WORD];
    Entry *candidates[MAX_CANDIDATES];
    int n_candidates = 0;

    char *tok = strtok(qcopy, " \t\n\r");
    while (tok && n_candidates < MAX_CANDIDATES) {
        to_lower_clean(tok, clean, sizeof(clean));
        if (clean[0]) {
            WordIndex *wi = find_word_index(clean);
            if (wi) {
                for (int i = 0; i < wi->n_matches && n_candidates < MAX_CANDIDATES; i++)
                    candidates[n_candidates++] = wi->matches[i];
            }
        }
        tok = strtok(NULL, " \t\n\r");
    }

    if (n_candidates > 0)
        return candidates[rand() % n_candidates];
    if (n_entries > 0)
        return all_entries[rand() % n_entries];
    return NULL;
}

/*
 * Generate and print a reply to "question":
 *   1. Pick a starting pair (w1, w2) via pick_seed().
 *   2. Repeatedly look up the entry for the current pair, randomly pick one
 *      of its recorded "next" words (weighted naturally by how often that
 *      transition occurred in the source text, since duplicates are
 *      stored), print it, and slide the pair forward by one word.
 *   3. Stop when: the chain reaches a pair with no known continuation, a
 *      sentence-ending word is produced (after a minimum length), or the
 *      maximum word count is reached.
 */
static void generate_answer(const char *question)
{
    if (n_entries == 0) {
        printf("Not enough text to generate an answer.\n");
        return;
    }
    Entry *cur = pick_seed(question);
    if (!cur) {
        printf("...\n");
        return;
    }

    char w1[MAX_WORD], w2[MAX_WORD];
    strncpy(w1, cur->w1, sizeof(w1) - 1);
    w1[sizeof(w1) - 1] = '\0';
    strncpy(w2, cur->w2, sizeof(w2) - 1);
    w2[sizeof(w2) - 1] = '\0';

    printf("> %s %s", w1, w2);
    int count = 2;

    while (count < GEN_MAX_WORDS) {
        Entry *e = find_entry(w1, w2);
        if (!e || e->n_next == 0)
            break;              /* dead end in the chain */
        const char *nxt = e->nexts[rand() % e->n_next]; /* random walk step */
        printf(" %s", nxt);
        count++;
        if (ends_sentence(nxt) && count >= GEN_MIN_WORDS)
            break;

        /* Slide the (w1, w2) window forward by one word: w2 becomes w1,
         * and the freshly generated word becomes the new w2. */
        char tmp[MAX_WORD];
        strncpy(tmp, nxt, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        strncpy(w1, w2, sizeof(w1) - 1);
        w1[sizeof(w1) - 1] = '\0';
        strncpy(w2, tmp, sizeof(w2) - 1);
        w2[sizeof(w2) - 1] = '\0';
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.txt>\n", argv[0]);
        return 1;
    }

    srand((unsigned) time(NULL));       /* seed the RNG once, used by rand() below */
    load_file(argv[1]);

    if (n_tokens < 3) {
        fprintf(stderr, "The file doesn't contain enough text to build a chain.\n");
        return 1;
    }
    build_chain();
    build_word_index();

    printf("Markov chain ready (%d words, %d unique pairs).\n", n_tokens, n_entries);
    printf("Ask a question (type 'exit' to quit):\n");

    /* Main interactive loop: read a question, generate and print an
     * answer, repeat until the user types "exit"/"quit" or closes stdin. */
    char line[MAX_LINE];
    while (1) {
        printf("\nYou: ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
            break;
        line[strcspn(line, "\n")] = '\0';       /* strip the trailing newline */
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0)
            break;
        if (strlen(line) == 0)
            continue;
        generate_answer(line);
    }

    return 0;
}
