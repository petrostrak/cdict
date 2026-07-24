#ifndef DICT_H
#define DICT_H

#include <stddef.h>

/* Everything the TUI ever sees is defined here. The ncurses layer only
 * ever calls dict_open / dict_lookup / dict_entry_free / dict_close and
 * reads the DictEntry struct. It never learns that data comes from
 * reader-dict + Tatoeba, so you can swap sources without touching the UI. */

/* One example sentence pair. All strings are UTF-8. en may be NULL. */
typedef struct
{
  char *ru; /* Russian sentence                */
  char *en; /* English translation, or NULL    */
} DictExample;

/* One sense / translation line from the Wiktionary-derived entry. */
typedef struct
{
  char *gloss; /* English translation or definition text (UTF-8) */
} DictSense;

/* The full aggregated result for one lemma. */
typedef struct
{
  char *headword;       /* lemma actually matched, e.g. "слово" */
  char *part_of_speech; /* "noun" / "verb" / ...   may be NULL  */
  char *grammar;        /* gender / aspect / etc.  may be NULL  */
  char *ipa;            /* pronunciation           may be NULL  */

  DictSense *senses;
  size_t n_senses;

  DictExample *examples;
  size_t n_examples;
} DictEntry;

/* Opaque handle holding the reader-dict data and the examples index. */
typedef struct DictDB DictDB;

/* Open the databases. Paths point at dictd files (.index + .dict or .dict.dz;
 * a plain uncompressed .dict works too). Pass NULL for the two examples paths
 * if you don't want example sentences. Returns NULL on failure. */
DictDB *dict_open(const char *rd_index_path, /* reader-dict .index        */
                  const char *rd_data_path,  /* reader-dict .dict[.dz]    */
                  const char *ex_index_path, /* examples .index or NULL   */
                  const char *ex_data_path); /* examples .dict[.dz] or NULL */

void dict_close(DictDB *db);

/* Look up a Russian word. Case-folds the query and, on a miss, retries via
 * the lemma hook (see dict_set_lemmatizer).
 *   returns  1 : hit, *out filled (free it with dict_entry_free)
 *   returns  0 : no match, *out left untouched
 *   returns -1 : error */
int dict_lookup(DictDB *db, const char *query_utf8, DictEntry *out);

void dict_entry_free(DictEntry *out);

/* Plug in a lemmatizer (e.g. a wrapper over the Snowball Russian stemmer or
 * hunspell). It receives a UTF-8 surface form and returns a malloc'd lemma,
 * or NULL. dict_lookup calls it only when the exact match fails. Optional. */
typedef char *(*DictLemmatizer)(const char *surface_utf8);
void dict_set_lemmatizer(DictDB *db, DictLemmatizer fn);

#endif /* DICT_H */
