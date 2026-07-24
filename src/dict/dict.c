#define _POSIX_C_SOURCE 200809L /* for strdup under -std=c11 */

#include "dict.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ------------------------------------------------------------------ */
/* One loaded dictd dictionary: its index in memory + the whole body   */
/* decompressed into a single buffer. reader-dict files are small      */
/* enough (tens of MB) that slurping the body once at open time is the */
/* simplest robust approach — after that a lookup is just a memcpy.    */
/* ------------------------------------------------------------------ */
typedef struct
{
  char *key;   /* normalized headword, used for sort + bsearch */
  long offset; /* byte offset into `data`                      */
  long length; /* byte length of the article                  */
} IndexEntry;

typedef struct
{
  IndexEntry *entries;
  size_t n;
  char *data; /* entire decompressed .dict body */
  size_t data_len;
} Dictionary;

struct DictDB
{
  Dictionary rd; /* reader-dict          */
  Dictionary ex; /* examples (optional)  */
  int have_ex;
  DictLemmatizer lemma;
};

/* ---- normalization: fold case so user input matches the headword ---- */
/* ASCII plus the basic Cyrillic block (А-Я -> а-я, Ё -> ё). Enough to be
 * practical; extend if you hit edge cases. Result is malloc'd. */
static char *norm_key(const char *s)
{
  size_t len = strlen(s);
  char *out = malloc(len + 1);
  if (!out)
    return NULL;
  size_t j = 0;
  for (size_t i = 0; i < len;)
  {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80)
    { /* ASCII */
      out[j++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
      i++;
    }
    else if (c == 0xD0 && i + 1 < len)
    { /* U+0400..U+043F range */
      unsigned char d = (unsigned char)s[i + 1];
      if (d >= 0x90 && d <= 0x9F)
      { /* А..П -> а..п */
        out[j++] = (char)0xD0;
        out[j++] = (char)(d + 0x20);
      }
      else if (d >= 0xA0 && d <= 0xAF)
      { /* Р..Я -> р..я (spills to D1) */
        out[j++] = (char)0xD1;
        out[j++] = (char)(d - 0x20);
      }
      else
      {
        out[j++] = (char)c;
        out[j++] = (char)d;
      }
      i += 2;
    }
    else
    {
      out[j++] = (char)c; /* leave other bytes as-is */
      i++;
    }
  }
  out[j] = '\0';
  return out;
}

/* ---- dictd stores offset/length as base64 of a big-endian integer ---- */
static int b64_val(char c)
{
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}
static long b64_int(const char *s, size_t len)
{
  long v = 0;
  for (size_t i = 0; i < len; i++)
  {
    int d = b64_val(s[i]);
    if (d < 0)
      break;
    v = (v << 6) | d;
  }
  return v;
}

static int idx_cmp(const void *a, const void *b)
{
  return strcmp(((const IndexEntry *)a)->key, ((const IndexEntry *)b)->key);
}

/* ---- load a .index file into a sorted array ---- */
static int load_index(Dictionary *d, const char *path)
{
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;

  size_t cap = 4096;
  d->entries = malloc(cap * sizeof *d->entries);
  d->n = 0;

  char line[8192];
  while (fgets(line, sizeof line, f))
  {
    char *t1 = strchr(line, '\t');
    if (!t1)
      continue;
    char *t2 = strchr(t1 + 1, '\t');
    if (!t2)
      continue;
    char *t3 = strchr(t2 + 1, '\t'); /* optional 4th field */
    *t1 = '\0';
    size_t off_len = (size_t)(t2 - (t1 + 1));
    size_t len_len = t3 ? (size_t)(t3 - (t2 + 1)) : strcspn(t2 + 1, "\r\n");

    if (d->n == cap)
    {
      cap *= 2;
      d->entries = realloc(d->entries, cap * sizeof *d->entries);
    }
    d->entries[d->n].key = norm_key(line);
    d->entries[d->n].offset = b64_int(t1 + 1, off_len);
    d->entries[d->n].length = b64_int(t2 + 1, len_len);
    d->n++;
  }
  fclose(f);

  /* Re-sort with OUR comparator so bsearch is independent of whatever
   * collation dictd used when the file was built. */
  qsort(d->entries, d->n, sizeof *d->entries, idx_cmp);
  return 0;
}

/* ---- slurp+decompress the body; gzread also passes plain files through ----
 */
static int load_data(Dictionary *d, const char *path)
{
  gzFile gz = gzopen(path, "rb");
  if (!gz)
    return -1;

  size_t cap = 1 << 20;
  d->data = malloc(cap);
  d->data_len = 0;

  int r;
  char buf[65536];
  while ((r = gzread(gz, buf, sizeof buf)) > 0)
  {
    if (d->data_len + (size_t)r + 1 > cap)
    {
      while (d->data_len + (size_t)r + 1 > cap)
        cap *= 2;
      d->data = realloc(d->data, cap);
    }
    memcpy(d->data + d->data_len, buf, (size_t)r);
    d->data_len += (size_t)r;
  }
  gzclose(gz);
  d->data[d->data_len] = '\0';
  return r < 0 ? -1 : 0;
}

/* ---- return a malloc'd, NUL-terminated copy of an article body ---- */
static char *fetch_article(const Dictionary *d, const IndexEntry *e)
{
  if (e->offset < 0 || (size_t)(e->offset + e->length) > d->data_len)
    return NULL;
  char *s = malloc((size_t)e->length + 1);
  if (!s)
    return NULL;
  memcpy(s, d->data + e->offset, (size_t)e->length);
  s[e->length] = '\0';
  return s;
}

static const IndexEntry *find(const Dictionary *d, const char *norm)
{
  IndexEntry probe = {(char *)norm, 0, 0};
  return bsearch(&probe, d->entries, d->n, sizeof *d->entries, idx_cmp);
}

/* ================================================================== */
/*  reader-dict article -> DictEntry fields                            */
/*  ADAPT THIS to the real body layout: run `dict -d <db> слово` or    */
/*  read one raw article and see how reader-dict formats POS / senses  */
/*  / IPA, then split accordingly. Until then, dump the body as a      */
/*  single sense so the pipeline works end to end.                     */
/* ================================================================== */
/* Duplicate the byte range [a, b) as a NUL-terminated string. */
static char *dup_range(const char *a, const char *b)
{
  size_t n = (size_t)(b - a);
  char *s = malloc(n + 1);
  if (!s)
    return NULL;
  memcpy(s, a, n);
  s[n] = '\0';
  return s;
}

/* ================================================================== */
/*  Parse a FreeDict rus-eng article into DictEntry fields.            */
/*  Format (single '\n' separators, no blank lines):                   */
/*     <headword> /<pronunciation>/ <pos>                              */
/*     1. <english translations>                                       */
/*     <russian explanation>                                           */
/*     2. <english translations>                                       */
/*     <russian explanation>                                           */
/*     ...                                                             */
/*  A line starting with digits followed by '.' begins a new sense;    */
/*  the line(s) after it, up to the next numbered line, are its gloss. */
/* ================================================================== */
static void parse_article(const char *body, DictEntry *out)
{
  const char *nl = strchr(body, '\n');
  const char *header_end = nl ? nl : body + strlen(body);

  /* pronunciation: between the first '/' and the next '/' on line 1 */
  const char *s1 = memchr(body, '/', (size_t)(header_end - body));
  if (s1)
  {
    const char *s2 = memchr(s1 + 1, '/', (size_t)(header_end - (s1 + 1)));
    if (s2)
      out->ipa = dup_range(s1 + 1, s2);
  }
  /* part of speech: between '<' and '>' on line 1 */
  const char *lt = memchr(body, '<', (size_t)(header_end - body));
  if (lt)
  {
    const char *gt = memchr(lt + 1, '>', (size_t)(header_end - (lt + 1)));
    if (gt)
      out->part_of_speech = dup_range(lt + 1, gt);
  }

  size_t cap = 4, n = 0;
  out->senses = malloc(cap * sizeof *out->senses);
  if (!out->senses)
  {
    out->n_senses = 0;
    return;
  }

  const char *p = nl ? nl + 1 : header_end;
  while (*p)
  {
    const char *end = strchr(p, '\n');
    if (!end)
      end = p + strlen(p);

    /* numbered line? (one or more digits then '.') */
    const char *q = p;
    while (q < end && *q >= '0' && *q <= '9')
      q++;
    int numbered = (q > p && q < end && *q == '.');

    if (numbered)
    {
      const char *t = q + 1;
      while (t < end && *t == ' ')
        t++;
      if (n == cap)
      {
        cap *= 2;
        out->senses = realloc(out->senses, cap * sizeof *out->senses);
      }
      out->senses[n].translation = dup_range(t, end);
      out->senses[n].gloss = NULL;
      n++;
    }
    else if (n > 0)
    {
      /* explanation line for the sense in progress (append if repeated) */
      char *g = dup_range(p, end);
      char **slot = &out->senses[n - 1].gloss;
      if (*slot == NULL)
      {
        *slot = g;
      }
      else
      {
        size_t a = strlen(*slot), b = strlen(g);
        char *m = malloc(a + 1 + b + 1);
        memcpy(m, *slot, a);
        m[a] = ' ';
        memcpy(m + a + 1, g, b);
        m[a + 1 + b] = '\0';
        free(*slot);
        free(g);
        *slot = m;
      }
    }
    p = (*end) ? end + 1 : end;
  }

  /* Fallback: entry with no numbered senses — treat everything after the
   * header as a single translation. */
  if (n == 0 && nl && *(nl + 1))
  {
    const char *rest = nl + 1;
    const char *rend = rest + strlen(rest);
    while (rend > rest && (rend[-1] == '\n' || rend[-1] == ' '))
      rend--;
    out->senses[0].translation = dup_range(rest, rend);
    out->senses[0].gloss = NULL;
    n = 1;
  }
  out->n_senses = n;
}

/* ---- examples body is our own preprocessed format: "ru<TAB>en\n" lines ----
 */
static void parse_examples(const char *body, DictEntry *out)
{
  size_t cap = 4;
  out->examples = malloc(cap * sizeof *out->examples);
  out->n_examples = 0;

  const char *p = body;
  while (*p)
  {
    const char *nl = strchr(p, '\n');
    size_t linelen = nl ? (size_t)(nl - p) : strlen(p);
    const char *tab = memchr(p, '\t', linelen);
    if (tab)
    {
      if (out->n_examples == cap)
      {
        cap *= 2;
        out->examples = realloc(out->examples, cap * sizeof *out->examples);
      }
      size_t rulen = (size_t)(tab - p);
      size_t enlen = linelen - rulen - 1;
      char *ru = malloc(rulen + 1);
      memcpy(ru, p, rulen);
      ru[rulen] = '\0';
      char *en = malloc(enlen + 1);
      memcpy(en, tab + 1, enlen);
      en[enlen] = '\0';
      out->examples[out->n_examples].ru = ru;
      out->examples[out->n_examples].en = en;
      out->n_examples++;
    }
    if (!nl)
      break;
    p = nl + 1;
  }
}

/* ============================= public API ============================ */

DictDB *dict_open(const char *rd_index_path, const char *rd_data_path,
                  const char *ex_index_path, const char *ex_data_path)
{
  DictDB *db = calloc(1, sizeof *db);
  if (!db)
    return NULL;

  if (load_index(&db->rd, rd_index_path) != 0 ||
      load_data(&db->rd, rd_data_path) != 0)
  {
    dict_close(db);
    return NULL;
  }
  if (ex_index_path && ex_data_path)
  {
    if (load_index(&db->ex, ex_index_path) == 0 &&
        load_data(&db->ex, ex_data_path) == 0)
      db->have_ex = 1;
  }
  return db;
}

void dict_set_lemmatizer(DictDB *db, DictLemmatizer fn) { db->lemma = fn; }

#define DICT_MAX_MATCHES 1000

int dict_matches(DictDB *db, const char *prefix_utf8, char ***out,
                 size_t *n_out)
{
  *out = NULL;
  *n_out = 0;

  char *pfx = norm_key(prefix_utf8);
  if (!pfx)
    return -1;
  size_t plen = strlen(pfx);
  if (plen == 0)
  {
    free(pfx);
    return 0;
  } /* empty prefix -> no menu */

  /* lower bound: first index whose key >= pfx */
  size_t lo = 0, hi = db->rd.n;
  while (lo < hi)
  {
    size_t mid = lo + (hi - lo) / 2;
    if (strcmp(db->rd.entries[mid].key, pfx) < 0)
      lo = mid + 1;
    else
      hi = mid;
  }

  size_t cap = 32, n = 0;
  char **list = malloc(cap * sizeof *list);
  if (!list)
  {
    free(pfx);
    return -1;
  }

  for (size_t i = lo; i < db->rd.n && n < DICT_MAX_MATCHES; i++)
  {
    if (strncmp(db->rd.entries[i].key, pfx, plen) != 0)
      break;
    if (n == cap)
    {
      cap *= 2;
      list = realloc(list, cap * sizeof *list);
    }
    list[n++] = strdup(db->rd.entries[i].key);
  }
  free(pfx);
  *out = list;
  *n_out = n;
  return 0;
}

void dict_matches_free(char **list, size_t n)
{
  if (!list)
    return;
  for (size_t i = 0; i < n; i++)
    free(list[i]);
  free(list);
}

int dict_lookup(DictDB *db, const char *query_utf8, DictEntry *out)
{
  char *norm = norm_key(query_utf8);
  if (!norm)
    return -1;

  const IndexEntry *hit = find(&db->rd, norm);

  /* miss -> try the lemma, if a lemmatizer is installed */
  if (!hit && db->lemma)
  {
    char *lemma = db->lemma(query_utf8);
    if (lemma)
    {
      char *lnorm = norm_key(lemma);
      free(lemma);
      if (lnorm)
      {
        free(norm);
        norm = lnorm;
        hit = find(&db->rd, norm);
      }
    }
  }
  if (!hit)
  {
    free(norm);
    return 0;
  }

  memset(out, 0, sizeof *out);
  out->headword = strdup(hit->key);

  char *body = fetch_article(&db->rd, hit);
  if (!body)
  {
    free(norm);
    free(out->headword);
    return -1;
  }
  parse_article(body, out);
  free(body);

  if (db->have_ex)
  {
    const IndexEntry *ex = find(&db->ex, norm);
    if (ex)
    {
      char *eb = fetch_article(&db->ex, ex);
      if (eb)
      {
        parse_examples(eb, out);
        free(eb);
      }
    }
  }
  free(norm);
  return 1;
}

void dict_entry_free(DictEntry *e)
{
  if (!e)
    return;
  free(e->headword);
  free(e->part_of_speech);
  free(e->grammar);
  free(e->ipa);
  for (size_t i = 0; i < e->n_senses; i++)
  {
    free(e->senses[i].translation);
    free(e->senses[i].gloss);
  }
  free(e->senses);
  for (size_t i = 0; i < e->n_examples; i++)
  {
    free(e->examples[i].ru);
    free(e->examples[i].en);
  }
  free(e->examples);
  memset(e, 0, sizeof *e);
}

static void free_dict(Dictionary *d)
{
  for (size_t i = 0; i < d->n; i++)
    free(d->entries[i].key);
  free(d->entries);
  free(d->data);
}

void dict_close(DictDB *db)
{
  if (!db)
    return;
  free_dict(&db->rd);
  free_dict(&db->ex);
  free(db);
}
