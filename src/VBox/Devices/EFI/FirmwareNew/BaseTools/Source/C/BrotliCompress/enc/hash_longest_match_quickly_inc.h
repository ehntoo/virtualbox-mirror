/* NOLINT(build/header_guard) */
/* Copyright 2010 Google Inc. All Rights Reserved.

   Distributed under MIT license.
   See file LICENSE for detail or copy at https://opensource.org/licenses/MIT
*/

/* template parameters: FN, BUCKET_BITS, BUCKET_SWEEP, USE_DICTIONARY */

#define HashLongestMatchQuickly HASHER()

#define BUCKET_SIZE (1 << BUCKET_BITS)

#define HASH_MAP_SIZE (4 << BUCKET_BITS)

static BROTLI_INLINE size_t FN(HashTypeLength)(void) { return 8; }
static BROTLI_INLINE size_t FN(StoreLookahead)(void) { return 8; }

/* HashBytes is the function that chooses the bucket to place
   the address in. The HashLongestMatch and HashLongestMatchQuickly
   classes have separate, different implementations of hashing. */
static uint32_t FN(HashBytes)(const uint8_t *data) {
  /* Computing a hash based on 5 bytes works much better for
     qualities 1 and 3, where the next hash value is likely to replace */
  uint64_t h = (BROTLI_UNALIGNED_LOAD64(data) << 24) * kHashMul32;
  /* The higher bits contain more mixture from the multiplication,
     so we take our results from there. */
  return (uint32_t)(h >> (64 - BUCKET_BITS));
}

/* A (forgetful) hash table to the data seen by the compressor, to
   help create backward references to previous data.

   This is a hash map of fixed size (BUCKET_SIZE). Starting from the
   given index, BUCKET_SWEEP buckets are used to store values of a key. */
typedef struct HashLongestMatchQuickly {
  uint32_t buckets_[BUCKET_SIZE + BUCKET_SWEEP];
  /* True if buckets_ array needs to be initialized. */
  BROTLI_BOOL is_dirty_;
  DictionarySearchStatictics dict_search_stats_;
} HashLongestMatchQuickly;

static void FN(Reset)(HashLongestMatchQuickly* self) {
  self->is_dirty_ = BROTLI_TRUE;
  DictionarySearchStaticticsReset(&self->dict_search_stats_);
}

static void FN(InitEmpty)(HashLongestMatchQuickly* self) {
  if (self->is_dirty_) {
    /* It is not strictly necessary to fill this buffer here, but
       not filling will make the results of the compression stochastic
       (but correct). This is because random data would cause the
       system to find accidentally good backward references here and there. */
    memset(&self->buckets_[0], 0, sizeof(self->buckets_));
    self->is_dirty_ = BROTLI_FALSE;
  }
}

static void FN(InitForData)(HashLongestMatchQuickly* self, const uint8_t* data,
    size_t num) {
  size_t i;
  for (i = 0; i < num; ++i) {
    const uint32_t key = FN(HashBytes)(&data[i]);
    memset(&self->buckets_[key], 0, BUCKET_SWEEP * sizeof(self->buckets_[0]));
  }
  if (num != 0) {
    self->is_dirty_ = BROTLI_FALSE;
  }
}

static void FN(Init)(
    MemoryManager* m, HashLongestMatchQuickly* self, const uint8_t* data,
    const BrotliEncoderParams* params, size_t position, size_t bytes,
    BROTLI_BOOL is_last) {
  /* Choose which init method is faster.
     Init() is about 100 times faster than InitForData(). */
  const size_t kMaxBytesForPartialHashInit = HASH_MAP_SIZE >> 7;
  BROTLI_UNUSED(m);
  BROTLI_UNUSED(params);
  if (position == 0 && is_last && bytes <= kMaxBytesForPartialHashInit) {
    FN(InitForData)(self, data, bytes);
  } else {
    FN(InitEmpty)(self);
  }
}

/* Look at 5 bytes at &data[ix & mask].
   Compute a hash from these, and store the value somewhere within
   [ix .. ix+3]. */
static BROTLI_INLINE void FN(Store)(HashLongestMatchQuickly* self,
    const uint8_t *data, const size_t mask, const size_t ix) {
  const uint32_t key = FN(HashBytes)(&data[ix & mask]);
  /* Wiggle the value with the bucket sweep range. */
  const uint32_t off = (ix >> 3) % BUCKET_SWEEP;
  self->buckets_[key + off] = (uint32_t)ix;
}

static BROTLI_INLINE void FN(StoreRange)(HashLongestMatchQuickly* self,
    const uint8_t *data, const size_t mask, const size_t ix_start,
    const size_t ix_end) {
  size_t i;
  for (i = ix_start; i < ix_end; ++i) {
    FN(Store)(self, data, mask, i);
  }
}

static BROTLI_INLINE void FN(StitchToPreviousBlock)(
    HashLongestMatchQuickly* self, size_t num_bytes, size_t position,
    const uint8_t* ringbuffer, size_t ringbuffer_mask) {
  if (num_bytes >= FN(HashTypeLength)() - 1 && position >= 3) {
    /* Prepare the hashes for three last bytes of the last write.
       These could not be calculated before, since they require knowledge
       of both the previous and the current block. */
    FN(Store)(self, ringbuffer, ringbuffer_mask, position - 3);
    FN(Store)(self, ringbuffer, ringbuffer_mask, position - 2);
    FN(Store)(self, ringbuffer, ringbuffer_mask, position - 1);
  }
}

/* Find a longest backward match of &data[cur_ix & ring_buffer_mask]
   up to the length of max_length and stores the position cur_ix in the
   hash table.

   Does not look for matches longer than max_length.
   Does not look for matches further away than max_backward.
   Writes the best match into |out|.
   Returns true if match is found, otherwise false. */
static BROTLI_INLINE BROTLI_BOOL FN(FindLongestMatch)(
    HashLongestMatchQuickly* self, const uint8_t* BROTLI_RESTRICT data,
    const size_t ring_buffer_mask, const int* BROTLI_RESTRICT distance_cache,
    const size_t cur_ix, const size_t max_length, const size_t max_backward,
    HasherSearchResult* BROTLI_RESTRICT out) {
  const size_t best_len_in = out->len;
  const size_t cur_ix_masked = cur_ix & ring_buffer_mask;
  const uint32_t key = FN(HashBytes)(&data[cur_ix_masked]);
  int compare_char = data[cur_ix_masked + best_len_in];
  score_t best_score = out->score;
  size_t best_len = best_len_in;
  size_t cached_backward = (size_t)distance_cache[0];
  size_t prev_ix = cur_ix - cached_backward;
  BROTLI_BOOL is_match_found = BROTLI_FALSE;
  out->len_x_code = 0;
  if (prev_ix < cur_ix) {
    prev_ix &= (uint32_t)ring_buffer_mask;
    if (compare_char == data[prev_ix + best_len]) {
      size_t len = FindMatchLengthWithLimit(&data[prev_ix],
                                            &data[cur_ix_masked],
                                            max_length);
      if (len >= 4) {
        best_score = BackwardReferenceScoreUsingLastDistance(len, 0);
        best_len = len;
        out->len = len;
        out->distance = cached_backward;
        out->score = best_score;
        compare_char = data[cur_ix_masked + best_len];
        if (BUCKET_SWEEP == 1) {
          self->buckets_[key] = (uint32_t)cur_ix;
          return BROTLI_TRUE;
        } else {
          is_match_found = BROTLI_TRUE;
        }
      }
    }
  }
  if (BUCKET_SWEEP == 1) {
    size_t backward;
    size_t len;
    /* Only one to look for, don't bother to prepare for a loop. */
    prev_ix = self->buckets_[key];
    self->buckets_[key] = (uint32_t)cur_ix;
    backward = cur_ix - prev_ix;
    prev_ix &= (uint32_t)ring_buffer_mask;
    if (compare_char != data[prev_ix + best_len_in]) {
      return BROTLI_FALSE;
    }
    if (PREDICT_FALSE(backward == 0 || backward > max_backward)) {
      return BROTLI_FALSE;
    }
    len = FindMatchLengthWithLimit(&data[prev_ix],
                                   &data[cur_ix_masked],
                                   max_length);
    if (len >= 4) {
      out->len = len;
      out->distance = backward;
      out->score = BackwardReferenceScore(len, backward);
      return BROTLI_TRUE;
    }
  } else {
    uint32_t *bucket = self->buckets_ + key;
    int i;
    prev_ix = *bucket++;
    for (i = 0; i < BUCKET_SWEEP; ++i, prev_ix = *bucket++) {
      const size_t backward = cur_ix - prev_ix;
      size_t len;
      prev_ix &= (uint32_t)ring_buffer_mask;
      if (compare_char != data[prev_ix + best_len]) {
        continue;
      }
      if (PREDICT_FALSE(backward == 0 || backward > max_backward)) {
        continue;
      }
      len = FindMatchLengthWithLimit(&data[prev_ix],
                                     &data[cur_ix_masked],
                                     max_length);
      if (len >= 4) {
        const score_t score = BackwardReferenceScore(len, backward);
        if (best_score < score) {
          best_score = score;
          best_len = len;
          out->len = best_len;
          out->distance = backward;
          out->score = score;
          compare_char = data[cur_ix_masked + best_len];
          is_match_found = BROTLI_TRUE;
        }
      }
    }
  }
  if (USE_DICTIONARY && !is_match_found) {
    is_match_found = SearchInStaticDictionary(&self->dict_search_stats_,
        &data[cur_ix_masked], max_length, max_backward, out, BROTLI_TRUE);
  }
  self->buckets_[key + ((cur_ix >> 3) % BUCKET_SWEEP)] = (uint32_t)cur_ix;
  return is_match_found;
}

#undef HASH_MAP_SIZE
#undef BUCKET_SIZE

#undef HashLongestMatchQuickly
