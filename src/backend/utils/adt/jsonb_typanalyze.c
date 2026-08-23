/*-------------------------------------------------------------------------
 *
 * jsonb_typanalyze.c
 *	  Functions for gathering statistics from jsonb columns
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonb_typanalyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "commands/vacuum.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"


/*
 * Avoid spending too much time analyzing unusually wide values.  This matches
 * the limit used by array_typanalyze.c for its type-specific statistics.
 */
#define JSONB_WIDTH_THRESHOLD 0x10000
#define JSONB_DISTINCT_HASH_LIMIT 1024
#define JSONB_EMPTY_FRACTION_MIN 0.05

#ifndef STATISTIC_KIND_JSONB_DISTINCT_ENTRY_COUNT_HISTOGRAM
#define STATISTIC_KIND_JSONB_DISTINCT_ENTRY_COUNT_HISTOGRAM 12
#endif
#ifndef STATISTIC_KIND_JSONB_EMPTY_ENTRY_FRACTION
#define STATISTIC_KIND_JSONB_EMPTY_ENTRY_FRACTION 13
#endif

typedef struct
{
	AnalyzeAttrComputeStatsFunc std_compute_stats;
	void	   *std_extra_data;
}			JsonbAnalyzeExtraData;

static void compute_jsonb_stats(VacAttrStats *stats,
								AnalyzeAttrFetchFunc fetchfunc, int samplerows,
								double totalrows);
static int	compare_ints(const void *a, const void *b, void *arg);
static int	count_jsonb_entries(Jsonb *jb, bool path_ops);
static int	count_jsonb_distinct_entries(Jsonb *jb);
static bool store_count_histogram(VacAttrStats *stats, int kind,
								  int *counts, int analyzed_rows);
static bool store_fraction(VacAttrStats *stats, int kind, float4 fraction);


/*
 * jsonb_typanalyze -- typanalyze function for jsonb columns
 */
Datum
jsonb_typanalyze(PG_FUNCTION_ARGS)
{
	VacAttrStats *stats = (VacAttrStats *) PG_GETARG_POINTER(0);
	JsonbAnalyzeExtraData *extra_data;

	if (!std_typanalyze(stats))
		PG_RETURN_BOOL(false);

	extra_data = palloc_object(JsonbAnalyzeExtraData);
	extra_data->std_compute_stats = stats->compute_stats;
	extra_data->std_extra_data = stats->extra_data;

	stats->compute_stats = compute_jsonb_stats;
	stats->extra_data = extra_data;

	PG_RETURN_BOOL(true);
}

/*
 * compute_jsonb_stats -- compute statistics for a jsonb column
 *
 * The histograms describe the number of entries emitted by the jsonb GIN
 * extraction functions for each sampled value.  The final number in each is
 * the average entry count over the analyzed rows.
 */
static void
compute_jsonb_stats(VacAttrStats *stats, AnalyzeAttrFetchFunc fetchfunc,
					int samplerows, double totalrows)
{
	JsonbAnalyzeExtraData *extra_data = (JsonbAnalyzeExtraData *) stats->extra_data;
	int		   *ops_counts;
	int		   *path_counts;
	int		   *distinct_counts;
	int			analyzed_rows = 0;
	int			empty_rows = 0;
	int			jsonb_no;

	/* First preserve the standard statistics for scalar jsonb comparisons. */
	stats->extra_data = extra_data->std_extra_data;
	extra_data->std_compute_stats(stats, fetchfunc, samplerows, totalrows);
	stats->extra_data = extra_data;

	ops_counts = palloc_array(int, samplerows);
	path_counts = palloc_array(int, samplerows);
	distinct_counts = palloc_array(int, samplerows);

	for (jsonb_no = 0; jsonb_no < samplerows; jsonb_no++)
	{
		Datum		value;
		bool		isnull;
		Jsonb	   *jb;

		vacuum_delay_point(true);

		value = fetchfunc(stats, jsonb_no, &isnull);
		if (isnull || toast_raw_datum_size(value) > JSONB_WIDTH_THRESHOLD)
			continue;

		jb = DatumGetJsonbP(value);
		ops_counts[analyzed_rows] = count_jsonb_entries(jb, false);
		path_counts[analyzed_rows] = count_jsonb_entries(jb, true);
		distinct_counts[analyzed_rows] = count_jsonb_distinct_entries(jb);
		empty_rows += ops_counts[analyzed_rows] == 0;
		analyzed_rows++;
	}

	/* A histogram needs at least two analyzed values. */
	if (analyzed_rows < 2)
		return;

	if (!store_count_histogram(stats, STATISTIC_KIND_JSONB_ENTRY_COUNT_HISTOGRAM,
							   ops_counts, analyzed_rows))
		return;
	store_count_histogram(stats,
						  STATISTIC_KIND_JSONB_PATH_ENTRY_COUNT_HISTOGRAM,
						  path_counts, analyzed_rows);
	if (empty_rows >= analyzed_rows * JSONB_EMPTY_FRACTION_MIN)
		store_fraction(stats, STATISTIC_KIND_JSONB_EMPTY_ENTRY_FRACTION,
					   (float4) empty_rows / analyzed_rows);
	store_count_histogram(stats,
						  STATISTIC_KIND_JSONB_DISTINCT_ENTRY_COUNT_HISTOGRAM,
						  distinct_counts, analyzed_rows);
}

static int
compare_ints(const void *a, const void *b, void *arg)
{
	int			ia = *((const int *) a);
	int			ib = *((const int *) b);

	return (ia > ib) - (ia < ib);
}

static int
count_jsonb_entries(Jsonb *jb, bool path_ops)
{
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken r;
	int			count = 0;

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		if (r == WJB_ELEM || r == WJB_VALUE || (!path_ops && r == WJB_KEY))
			count++;
	}

	return count;
}

static int
count_jsonb_distinct_entries(Jsonb *jb)
{
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken r;
	uint32		hashes[JSONB_DISTINCT_HASH_LIMIT];
	int			distinct = 0;

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		bool		is_entry;
		bool		is_key;
		uint32		hash;

		is_entry = r == WJB_KEY || r == WJB_ELEM || r == WJB_VALUE;
		if (!is_entry)
			continue;

		is_key = r == WJB_KEY || (r == WJB_ELEM && v.type == jbvString);
		hash = ((uint32) v.type << 24) | (is_key ? 0x0051u : 0x00a7u);
		JsonbHashScalarValue(&v, &hash);

		for (int i = 0; i < distinct; i++)
		{
			if (hashes[i] == hash)
				goto next_entry;
		}

		if (distinct == JSONB_DISTINCT_HASH_LIMIT)
			/* Disable consumers rather than reporting a lossy undercount. */
			return JSONB_DISTINCT_HASH_LIMIT + 1;
		hashes[distinct++] = hash;

next_entry:
		;
	}

	return distinct;
}

static bool
store_fraction(VacAttrStats *stats, int kind, float4 fraction)
{
	float4	   *numbers;
	MemoryContext old_context;
	int			slot_idx;

	for (slot_idx = 0; slot_idx < STATISTIC_NUM_SLOTS; slot_idx++)
	{
		if (stats->stakind[slot_idx] == 0)
			break;
	}
	if (slot_idx == STATISTIC_NUM_SLOTS)
		return false;

	old_context = MemoryContextSwitchTo(stats->anl_context);
	numbers = palloc(sizeof(float4));
	numbers[0] = fraction;
	MemoryContextSwitchTo(old_context);

	stats->stakind[slot_idx] = kind;
	stats->staop[slot_idx] = InvalidOid;
	stats->stacoll[slot_idx] = InvalidOid;
	stats->stanumbers[slot_idx] = numbers;
	stats->numnumbers[slot_idx] = 1;
	return true;
}

static bool
store_count_histogram(VacAttrStats *stats, int kind, int *counts,
					  int analyzed_rows)
{
	int			num_hist;
	int			slot_idx;
	int64		total = 0;
	float4	   *hist;
	MemoryContext old_context;

	for (int i = 0; i < analyzed_rows; i++)
		total += counts[i];

	for (slot_idx = 0; slot_idx < STATISTIC_NUM_SLOTS; slot_idx++)
	{
		if (stats->stakind[slot_idx] == 0)
			break;
	}
	if (slot_idx == STATISTIC_NUM_SLOTS)
		return false;

	qsort_interruptible(counts, analyzed_rows, sizeof(int), compare_ints, NULL);
	num_hist = Min(analyzed_rows, Max(stats->attstattarget, 2));
	old_context = MemoryContextSwitchTo(stats->anl_context);
	hist = palloc_array(float4, num_hist + 1);
	for (int i = 0; i < num_hist; i++)
	{
		int64		pos = ((int64) i * (analyzed_rows - 1)) / (num_hist - 1);

		hist[i] = counts[pos];
	}
	hist[num_hist] = (float8) total / analyzed_rows;
	MemoryContextSwitchTo(old_context);

	stats->stakind[slot_idx] = kind;
	stats->staop[slot_idx] = InvalidOid;
	stats->stacoll[slot_idx] = InvalidOid;
	stats->stanumbers[slot_idx] = hist;
	stats->numnumbers[slot_idx] = num_hist + 1;
	return true;
}
