Benchmark display pack
======================
All values are parsed from 261125-BENCH.md.

Charts:
01 Scalability: PGO throughput vs thread count for Producer_Consumer and Mixed_Multi.
02 PGO lift: median paired throughput change for exact matching benchmark names.
03 Throughput vs variability: PGO core contention workloads; dashed line marks CV=10% as a visual caution threshold, not a statistical test.
04 Single operations: Push_Single and Pop_Single throughput, PGO.
05 Slot sensitivity: ElimiBackoff Slots16/32/64/128, FullContention, 4 threads.

Cautions:
- PGO lift uses exact benchmark-name pairs between PGO and N-PGO.
- Means/CV values are summary statistics; raw per-repetition samples are not available here.
- Do not compare real_time and manual_time benchmarks as if they were the same timing protocol.
- The chart set emphasizes within-workload comparisons and avoids mixing timing modes.
