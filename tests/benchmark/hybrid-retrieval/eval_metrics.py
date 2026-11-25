#!/usr/bin/env python3
"""Metrics for the hybrid retrieval quality benchmark (DEC-112).

Usage: eval_metrics.py <eval_dataset.json> <rankings.json>
Reads the rankings dumped by retrieval_eval and prints strict + duplicate
aware Recall@{1,3,5,10} / MRR@10 per configuration, per query type and
overall, plus the near-duplicate redundancy count (both members of a pair
co-present in the top-5).
"""
import json
import sys
from collections import defaultdict

dataset = json.load(open(sys.argv[1]))
rankings = json.load(open(sys.argv[2]))
gold = {q["id"]: (list(q["gold"]), q["type"]) for q in dataset["queries"]}
pairs = [tuple(p) for p in dataset["meta"]["near_dup_pairs"]]
twins = {}
for a, b in pairs:
    twins.setdefault(a, set()).add(b)
    twins.setdefault(b, set()).add(a)


def recall_at(order, gold_set, k):
    return len(set(order[:k]) & gold_set) / len(gold_set)


def mrr_at(order, gold_set, k=10):
    for rank, item in enumerate(order[:k], 1):
        if item in gold_set:
            return 1.0 / rank
    return 0.0


def redundancy(order, k=5):
    top = set(order[:k])
    return sum(1 for a, b in pairs if a in top and b in top)


def gold_set_of(qid, expand):
    members = set(gold[qid][0])
    if expand:
        # Walk a copy: adding a twin to `members` while iterating it raises
        # "Set changed size during iteration" for every gold set with a twin.
        for member in list(members):
            members |= twins.get(member, set())
    return members


for expand, label in ((False, "strict"), (True, "dup-aware")):
    print(f"===== {label} =====")
    print(f"{'config':<22} {'type':<11} {'R@1':>6} {'R@3':>6} {'R@5':>6} "
          f"{'R@10':>6} {'MRR@10':>7} {'n':>3}")
    for config in rankings:
        per_type = defaultdict(list)
        for qid, order in rankings[config].items():
            gs, query_type = gold[qid][0], gold[qid][1]
            gs = gold_set_of(qid, expand)
            per_type[query_type].append(
                (recall_at(order, gs, 1), recall_at(order, gs, 3),
                 recall_at(order, gs, 5), recall_at(order, gs, 10),
                 mrr_at(order, gs)))
        for query_type in ("paraphrase", "lexical", "multi", "ALL"):
            rows = (per_type[query_type] if query_type != "ALL"
                    else [row for rows in per_type.values() for row in rows])
            if not rows:
                continue
            n = len(rows)
            means = [sum(row[i] for row in rows) / n for i in range(5)]
            print(f"{config:<22} {query_type:<11} {means[0]:>6.3f} {means[1]:>6.3f} "
                  f"{means[2]:>6.3f} {means[3]:>6.3f} {means[4]:>7.3f} {n:>3}")
        redundancy_total = sum(redundancy(order) for order in rankings[config].values())
        print(f"{config:<22} near-dup pairs co-present in top-5: {redundancy_total}")
        print()
