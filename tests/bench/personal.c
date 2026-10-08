/* Personal ranking benchmark (see personal.h).
 *
 * Latency: every held-out query is typed byte by byte, as in the plain
 * typing benchmark, with 1000 and then 4000 random entries boosted; and
 * usage_boosts is timed for a summary filled to its caps.
 *
 * Scenario: a person has PERSONAL_HABITS habitual files, opened
 * PERSONAL_HISTORY_OPENS times over the past month with Zipf-distributed
 * frequency, each from a typed prefix of the file's name. Held-out launches
 * then type the name of a habitual file (drawn from the same distribution)
 * or of a file never opened. For each, report how many keystrokes it takes
 * until the file ranks first, and MRR@10 after three keystrokes, with plain
 * and with personalized ranking. */
#include "personal.h"
#include "stats.h"
#include "torchlight/usage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PERSONAL_LIMIT = 10,
    PERSONAL_HABITS = 200,
    PERSONAL_HISTORY_OPENS = 1500,
    PERSONAL_HISTORY_DAYS = 30,
    PERSONAL_INTENTS = 300,
    PERSONAL_PROBE_KEYSTROKES = 3,
    PERSONAL_SEED_HABITS = 3,
    PERSONAL_SEED_OTHERS = 4,
    PERSONAL_SEED_BOOSTS = 5,
    PERSONAL_DAY = 24 * 3600
};
/* A fixed clock keeps the scenario deterministic. */
static const int64_t PERSONAL_NOW = 1790000000;

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}
/* Type every query byte by byte, recording each keystroke's latency. */
static tl_status type_queries(const tl_lexical *engine, tl_lexical_workspace *workspace,
                              const bench_query *queries, size_t count, bench_latency *latency) {
    tl_result results[PERSONAL_LIMIT];
    char prefix[LEXICAL_QUERY_BYTES + 1];
    tl_status status = TL_OK;
    for (size_t q = 0; q < count && status == TL_OK; q++) {
        size_t length = strlen(queries[q].text);
        for (size_t typed = 1; typed <= length && status == TL_OK; typed++) {
            memcpy(prefix, queries[q].text, typed);
            prefix[typed] = 0;
            double start = 0, end = 0;
            size_t found = 0;
            status = bench_now(&start);
            if (status == TL_OK)
                status = lexical_query(engine, workspace, prefix, results, PERSONAL_LIMIT, &found);
            if (status == TL_OK)
                status = bench_now(&end);
            if (status == TL_OK)
                status = bench_record(latency, (end - start) * 1e3);
        }
    }
    return status;
}
/* Typing latency with boosted random entries (positions repeat-free). */
static tl_status measure_boosted(const tl_lexical *engine, tl_lexical_workspace *workspace,
                                 const bench_query *queries, size_t count, size_t boosted) {
    size_t entries = lexical_count(engine);
    if (boosted > entries)
        boosted = entries;
    tl_lexical_boost *boosts = malloc(boosted * sizeof(*boosts));
    if (boosts == NULL)
        return TL_NOMEM;
    uint64_t state = PERSONAL_SEED_BOOSTS;
    size_t stride = entries / boosted,
           value_range = USAGE_FRECENCY_BOOST_MAX + USAGE_QUERY_BOOST_MAX;
    for (size_t i = 0; i < boosted; i++)
        boosts[i] = (tl_lexical_boost){i * stride + corpus_random(&state) % stride,
                                       (int)(1 + corpus_random(&state) % value_range)};
    bench_latency latency = {0};
    tl_status status = lexical_workspace_boost(workspace, boosts, boosted);
    if (status == TL_OK)
        status = type_queries(engine, workspace, queries, count, &latency);
    if (status == TL_OK) {
        char label[64];
        snprintf(label, sizeof(label), "personal_typing_boosted_%zu", boosted);
        bench_report(label, &latency);
    }
    tl_status cleared = lexical_workspace_boost(workspace, NULL, 0);
    bench_latency_free(&latency);
    free(boosts);
    return status == TL_OK ? cleared : status;
}
/* Per-keystroke cost of usage_boosts with every item and pair slot filled. */
static tl_status measure_usage_boosts(const bench_query *queries, size_t count) {
    tl_usage *usage = NULL;
    tl_status status = usage_create((int64_t)PERSONAL_HISTORY_DAYS * PERSONAL_DAY, &usage);
    for (size_t i = 0; i < USAGE_MAX_PAIRS && status == TL_OK; i++) {
        const char *text = queries[i % count].text;
        char prefix[LEXICAL_QUERY_BYTES + 1];
        size_t length = strlen(text) < 2 + i % 6 ? strlen(text) : 2 + i % 6;
        memcpy(prefix, text, length);
        prefix[length] = 0;
        tl_usage_target target = {1 + i % USAGE_MAX_ITEMS, NULL};
        status =
            usage_record(usage, target, prefix, PERSONAL_NOW - (int64_t)(i % 7) * PERSONAL_DAY);
    }
    bench_latency latency = {0};
    char prefix[LEXICAL_QUERY_BYTES + 1];
    for (size_t q = 0; q < count && status == TL_OK; q++) {
        size_t length = strlen(queries[q].text);
        for (size_t typed = 1; typed <= length && status == TL_OK; typed++) {
            memcpy(prefix, queries[q].text, typed);
            prefix[typed] = 0;
            const int *boosts = NULL;
            double start = 0, end = 0;
            status = bench_now(&start);
            if (status == TL_OK)
                status = usage_boosts(usage, prefix, PERSONAL_NOW, &boosts);
            if (status == TL_OK)
                status = bench_now(&end);
            if (status == TL_OK)
                status = bench_record(&latency, (end - start) * 1e3);
        }
    }
    if (status == TL_OK) {
        printf("usage_items=%zu\n", usage_count(usage));
        bench_report("usage_boosts", &latency);
    }
    bench_latency_free(&latency);
    usage_destroy(usage);
    return status;
}
struct scenario {
    const bench_corpus *corpus;
    const tl_lexical *engine;
    tl_lexical_workspace *workspace;
    tl_usage *usage;
    tl_lexical_boost boosts[USAGE_MAX_ITEMS];
    size_t habits[PERSONAL_HABITS], others[PERSONAL_INTENTS], habit_count, other_count;
    double cumulative[PERSONAL_HABITS];
    uint64_t state;
};
/* Distinct exact-query targets (random ASCII-named entries) for a seed,
 * skipping those in exclude. */
static tl_status pick_targets(const bench_corpus *corpus, uint64_t seed, const size_t *exclude,
                              size_t exclude_count, size_t *out, size_t capacity, size_t *count) {
    bench_query *queries = NULL;
    size_t generated = 0;
    tl_status status = queries_generate(corpus, seed, capacity * 2, &queries, &generated);
    *count = 0;
    for (size_t q = 0; q < generated && status == TL_OK && *count < capacity; q++) {
        bool seen = queries[q].kind != QUERY_EXACT;
        for (size_t i = 0; i < *count && !seen; i++)
            seen = out[i] == queries[q].target;
        for (size_t i = 0; i < exclude_count && !seen; i++)
            seen = exclude[i] == queries[q].target;
        if (!seen)
            out[(*count)++] = queries[q].target;
    }
    free(queries);
    return status;
}
static size_t pick_habit(struct scenario *scenario) {
    double draw = (double)(corpus_random(&scenario->state) % 1000000) / 1e6 *
                  scenario->cumulative[scenario->habit_count - 1];
    size_t index = 0;
    while (index + 1 < scenario->habit_count && scenario->cumulative[index] < draw)
        index++;
    return scenario->habits[index];
}
/* A month of opens, each from a typed prefix of two to eight bytes. */
static tl_status simulate_history(struct scenario *scenario) {
    tl_status status = TL_OK;
    for (size_t i = 0; i < PERSONAL_HISTORY_OPENS && status == TL_OK; i++) {
        size_t target = pick_habit(scenario);
        const char *name = base_name(scenario->corpus->paths[target]);
        size_t longest = strlen(name) < 8 ? strlen(name) : 8;
        size_t typed = longest <= 2 ? longest : 2 + corpus_random(&scenario->state) % (longest - 1);
        char prefix[16];
        memcpy(prefix, name, typed);
        prefix[typed] = 0;
        int64_t age = (int64_t)(corpus_random(&scenario->state) %
                                ((uint64_t)PERSONAL_HISTORY_DAYS * PERSONAL_DAY));
        tl_usage_target file = {(uint64_t)target + 1, NULL};
        status = usage_record(scenario->usage, file, prefix, PERSONAL_NOW - age);
    }
    return status;
}
/* One-based rank of a result named name in query's top results, 0 if absent. */
static tl_status rank_of(struct scenario *scenario, const char *query, const char *name,
                         bool personal, size_t *rank) {
    size_t count = 0;
    tl_status status = TL_OK;
    if (personal) {
        const int *values = NULL;
        status = usage_boosts(scenario->usage, query, PERSONAL_NOW, &values);
        for (size_t i = 0; i < usage_count(scenario->usage) && status == TL_OK; i++)
            scenario->boosts[count++] =
                (tl_lexical_boost){usage_target(scenario->usage, i).file_id - 1, values[i]};
    }
    if (status == TL_OK)
        status = lexical_workspace_boost(scenario->workspace, scenario->boosts, count);
    tl_result results[PERSONAL_LIMIT];
    size_t found = 0;
    if (status == TL_OK)
        status = lexical_query(scenario->engine, scenario->workspace, query, results,
                               PERSONAL_LIMIT, &found);
    *rank = 0;
    for (size_t i = 0; i < found && *rank == 0; i++)
        if (strcmp(base_name(results[i].path), name) == 0)
            *rank = i + 1;
    return status;
}
struct outcome {
    double keystrokes, reciprocal;
    size_t intents;
};
/* Type target's name until it ranks first (length + 1 when it never does),
 * noting its rank after PERSONAL_PROBE_KEYSTROKES keystrokes (or the whole
 * name when shorter). */
static tl_status launch(struct scenario *scenario, size_t target, bool personal,
                        struct outcome *outcome) {
    const char *name = base_name(scenario->corpus->paths[target]);
    size_t length = strlen(name), first = length + 1, probe = 0;
    size_t probe_at = length < PERSONAL_PROBE_KEYSTROKES ? length : PERSONAL_PROBE_KEYSTROKES;
    char prefix[LEXICAL_QUERY_BYTES + 1];
    tl_status status = TL_OK;
    for (size_t typed = 1; typed <= length && status == TL_OK; typed++) {
        if (first <= length && typed > probe_at)
            break;
        memcpy(prefix, name, typed);
        prefix[typed] = 0;
        size_t rank = 0;
        status = rank_of(scenario, prefix, name, personal, &rank);
        if (rank == 1 && first > length)
            first = typed;
        if (typed == probe_at)
            probe = rank;
    }
    outcome->keystrokes += (double)first;
    outcome->reciprocal += probe == 0 ? 0.0 : 1.0 / (double)probe;
    outcome->intents++;
    return status;
}
static void report_group(const char *label, const struct outcome *plain,
                         const struct outcome *personal) {
    double n = plain->intents == 0 ? 1.0 : (double)plain->intents;
    printf("personal_scenario %s n=%zu keystrokes_to_first plain=%.2f personal=%.2f "
           "mrr@10_after_%d plain=%.3f personal=%.3f\n",
           label, plain->intents, plain->keystrokes / n, personal->keystrokes / n,
           PERSONAL_PROBE_KEYSTROKES, plain->reciprocal / n, personal->reciprocal / n);
}
static tl_status run_group(struct scenario *scenario, const char *label, bool habitual) {
    struct outcome plain = {0}, personal = {0};
    tl_status status = TL_OK;
    size_t intents = habitual ? PERSONAL_INTENTS : scenario->other_count;
    for (size_t i = 0; i < intents && status == TL_OK; i++) {
        size_t target = habitual ? pick_habit(scenario) : scenario->others[i];
        status = launch(scenario, target, false, &plain);
        if (status == TL_OK)
            status = launch(scenario, target, true, &personal);
    }
    if (status == TL_OK)
        report_group(label, &plain, &personal);
    return status;
}
static tl_status run_scenario(const bench_corpus *corpus, const tl_lexical *engine,
                              tl_lexical_workspace *workspace) {
    struct scenario *scenario = calloc(1, sizeof(*scenario));
    if (scenario == NULL)
        return TL_NOMEM;
    *scenario = (struct scenario){
        .corpus = corpus, .engine = engine, .workspace = workspace, .state = PERSONAL_SEED_HABITS};
    tl_status status = pick_targets(corpus, PERSONAL_SEED_HABITS, NULL, 0, scenario->habits,
                                    PERSONAL_HABITS, &scenario->habit_count);
    if (status == TL_OK)
        status = pick_targets(corpus, PERSONAL_SEED_OTHERS, scenario->habits, scenario->habit_count,
                              scenario->others, PERSONAL_INTENTS, &scenario->other_count);
    if (status == TL_OK && scenario->habit_count == 0)
        status = TL_STATE;
    /* Zipf weights: the k-th habit is opened about 1/k as often as the first. */
    for (size_t i = 0; i < scenario->habit_count; i++)
        scenario->cumulative[i] =
            (i == 0 ? 0.0 : scenario->cumulative[i - 1]) + 1.0 / (double)(i + 1);
    if (status == TL_OK)
        status = usage_create((int64_t)PERSONAL_HISTORY_DAYS * PERSONAL_DAY, &scenario->usage);
    if (status == TL_OK)
        status = simulate_history(scenario);
    if (status == TL_OK)
        status = run_group(scenario, "habitual", true);
    if (status == TL_OK)
        status = run_group(scenario, "never_opened", false);
    tl_status cleared = lexical_workspace_boost(workspace, NULL, 0);
    usage_destroy(scenario->usage);
    free(scenario);
    return status == TL_OK ? cleared : status;
}
tl_status personal_benchmark(const bench_corpus *corpus, const tl_lexical *engine,
                             tl_lexical_workspace *workspace, const bench_query *held_out,
                             size_t held_out_count) {
    /* 2048 is the usage summary's cap; 4000 nears the engine's own. */
    static const size_t BOOSTED[] = {1000, USAGE_MAX_ITEMS, 4000};
    tl_status status = held_out_count == 0 ? TL_STATE : TL_OK;
    for (size_t i = 0; i < sizeof(BOOSTED) / sizeof(BOOSTED[0]) && status == TL_OK; i++)
        status = measure_boosted(engine, workspace, held_out, held_out_count, BOOSTED[i]);
    if (status == TL_OK)
        status = measure_usage_boosts(held_out, held_out_count);
    if (status == TL_OK)
        status = run_scenario(corpus, engine, workspace);
    return status;
}
