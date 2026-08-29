/*
 * markov.c
 *
 * Legge un file .txt, costruisce una catena di Markov (ordine 2: coppia di
 * parole -> parola successiva) e la usa per generare risposte a domande
 * poste interattivamente dall'utente.
 *
 * Uso: ./markov corpus.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define HASH_SIZE     8192
#define MAX_LINE      4096
#define MAX_WORD      128
#define GEN_MIN_WORDS 8
#define GEN_MAX_WORDS 40
#define MAX_CANDIDATES 256

typedef struct Entry {
    char *w1, *w2;
    char **nexts;
    int n_next, cap_next;
    struct Entry *next; /* concatenamento hash */
} Entry;

static Entry *table[HASH_SIZE];
static Entry **all_entries = NULL;
static int n_entries = 0, cap_entries = 0;

static char **tokens = NULL;
static int n_tokens = 0, cap_tokens = 0;

static unsigned long hash_str(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + (unsigned long)c;
    return h;
}

static unsigned long hash_pair(const char *a, const char *b) {
    return (hash_str(a) * 33) ^ hash_str(b);
}

static void add_token(const char *w) {
    if (n_tokens == cap_tokens) {
        cap_tokens = cap_tokens ? cap_tokens * 2 : 256;
        tokens = realloc(tokens, (size_t)cap_tokens * sizeof(char *));
        if (!tokens) { perror("realloc"); exit(1); }
    }
    tokens[n_tokens++] = strdup(w);
}

static Entry *find_entry(const char *w1, const char *w2) {
    unsigned long h = hash_pair(w1, w2) % HASH_SIZE;
    for (Entry *e = table[h]; e; e = e->next)
        if (strcmp(e->w1, w1) == 0 && strcmp(e->w2, w2) == 0) return e;
    return NULL;
}

static Entry *get_or_create(const char *w1, const char *w2) {
    unsigned long h = hash_pair(w1, w2) % HASH_SIZE;
    Entry *e = find_entry(w1, w2);
    if (e) return e;

    e = malloc(sizeof(Entry));
    e->w1 = strdup(w1);
    e->w2 = strdup(w2);
    e->nexts = NULL;
    e->n_next = e->cap_next = 0;
    e->next = table[h];
    table[h] = e;

    if (n_entries == cap_entries) {
        cap_entries = cap_entries ? cap_entries * 2 : 256;
        all_entries = realloc(all_entries, (size_t)cap_entries * sizeof(Entry *));
    }
    all_entries[n_entries++] = e;
    return e;
}

static void add_next(Entry *e, const char *w) {
    if (e->n_next == e->cap_next) {
        e->cap_next = e->cap_next ? e->cap_next * 2 : 4;
        e->nexts = realloc(e->nexts, (size_t)e->cap_next * sizeof(char *));
    }
    e->nexts[e->n_next++] = strdup(w);
}

/* copia src in dst mantenendo solo caratteri alfanumerici, in minuscolo */
static void to_lower_clean(const char *src, char *dst, size_t dstsize) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dstsize - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c)) dst[j++] = (char)tolower(c);
    }
    dst[j] = '\0';
}

static void load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Impossibile aprire il file '%s'\n", path);
        exit(1);
    }
    char buf[MAX_LINE];
    while (fscanf(f, "%4095s", buf) == 1) {
        add_token(buf);
    }
    fclose(f);
}

static void build_chain(void) {
    for (int i = 0; i + 2 < n_tokens; i++) {
        Entry *e = get_or_create(tokens[i], tokens[i + 1]);
        add_next(e, tokens[i + 2]);
    }
}

static int ends_sentence(const char *w) {
    size_t l = strlen(w);
    if (l == 0) return 0;
    char c = w[l - 1];
    return c == '.' || c == '?' || c == '!';
}

/* cerca fra le parole della domanda una che compaia come w1 in qualche
 * coppia della catena; se ne trova piu' di una sceglie a caso, altrimenti
 * ripiega su una coppia casuale qualsiasi. */
static Entry *pick_seed(const char *question) {
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
            for (int i = 0; i < n_entries && n_candidates < MAX_CANDIDATES; i++) {
                char ew1[MAX_WORD];
                to_lower_clean(all_entries[i]->w1, ew1, sizeof(ew1));
                if (strcmp(ew1, clean) == 0) {
                    candidates[n_candidates++] = all_entries[i];
                }
            }
        }
        tok = strtok(NULL, " \t\n\r");
    }

    if (n_candidates > 0) return candidates[rand() % n_candidates];
    if (n_entries > 0) return all_entries[rand() % n_entries];
    return NULL;
}

static void generate_answer(const char *question) {
    if (n_entries == 0) {
        printf("Non ho abbastanza testo per rispondere.\n");
        return;
    }
    Entry *cur = pick_seed(question);
    if (!cur) {
        printf("...\n");
        return;
    }

    char w1[MAX_WORD], w2[MAX_WORD];
    strncpy(w1, cur->w1, sizeof(w1) - 1); w1[sizeof(w1) - 1] = '\0';
    strncpy(w2, cur->w2, sizeof(w2) - 1); w2[sizeof(w2) - 1] = '\0';

    printf("> %s %s", w1, w2);
    int count = 2;

    while (count < GEN_MAX_WORDS) {
        Entry *e = find_entry(w1, w2);
        if (!e || e->n_next == 0) break;
        const char *nxt = e->nexts[rand() % e->n_next];
        printf(" %s", nxt);
        count++;
        if (ends_sentence(nxt) && count >= GEN_MIN_WORDS) break;

        char tmp[MAX_WORD];
        strncpy(tmp, nxt, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = '\0';
        strncpy(w1, w2, sizeof(w1) - 1); w1[sizeof(w1) - 1] = '\0';
        strncpy(w2, tmp, sizeof(w2) - 1); w2[sizeof(w2) - 1] = '\0';
    }
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <file.txt>\n", argv[0]);
        return 1;
    }

    srand((unsigned)time(NULL));
    load_file(argv[1]);

    if (n_tokens < 3) {
        fprintf(stderr, "Il file contiene troppo poco testo per costruire una catena.\n");
        return 1;
    }
    build_chain();

    printf("Catena di Markov pronta (%d parole, %d coppie uniche).\n", n_tokens, n_entries);
    printf("Fai una domanda (scrivi 'exit' per uscire):\n");

    char line[MAX_LINE];
    while (1) {
        printf("\nTu: ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = '\0';
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;
        if (strlen(line) == 0) continue;
        generate_answer(line);
    }

    return 0;
}
