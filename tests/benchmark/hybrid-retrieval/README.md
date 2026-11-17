# Hybrid retrieval quality benchmark (DEC-112, MNT-202611-HR)

Manual live-provider benchmark for the memory store retrieval quality.
Registered results: `docs/benchmarks/memory-hybrid-retrieval-v1.md`
(dataset digest `c8767eba…`, 44 records / 20 gold queries, bilingual).

This directory is NOT wired into ctest: the benchmark calls the real
embedding/rerank endpoints and needs a key. Offline regression gates for the
fusion machinery live in `tests/m4/m4_fusion_eval.cpp`.

Files:
- `eval_dataset.json` - the frozen gold dataset (records, queries, gold
  relevance, near-duplicate pairs). Digest over canonical JSON is asserted in
  the registered benchmark document.
- `retrieval_eval.cpp` - drives the shipped `SqliteMemoryStore` through five
  configurations (lexical-only / weighted / RRF / RRF+dedup / +rerank) with
  embeddings and rerank from the shipped providers; dumps per-query rankings.
- `eval_metrics.py` - Recall@{1,3,5,10} / MRR@10 (strict and duplicate-aware)
  and near-duplicate redundancy.

Reproduce (manual, needs `SF_EMBEDDING_KEY` in the environment):

    # 1. build the consumer (adjust lib paths to your build dir)
    g++ -std=c++20 -I include -I third_party/kairo/include \
        tests/benchmark/hybrid-retrieval/retrieval_eval.cpp -o /tmp/retrieval_eval \
        build/debug/libmira_state_store.a build/debug/libmira_net_transport.a \
        build/debug/libmira_openssl_transport.a build/debug/libmira_core.a \
        build/debug/third_party/kairo/src/libkairo.a build/debug/third_party/sqlite/libsqlite3.a \
        -lssl -lcrypto -lpthread
    # 2. run through the local proxy
    /tmp/retrieval_eval tests/benchmark/hybrid-retrieval/eval_dataset.json /tmp/rankings.json
    # 3. metrics
    python3 tests/benchmark/hybrid-retrieval/eval_metrics.py \
        tests/benchmark/hybrid-retrieval/eval_dataset.json /tmp/rankings.json
