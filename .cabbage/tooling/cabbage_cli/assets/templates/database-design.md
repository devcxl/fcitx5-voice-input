---
change: {{CHANGE_ID}}
cabbage_stage: {{STAGE_ID}}
change_type: {{CHANGE_TYPE}}
---

<!-- Replace every marked prompt before verifying this stage. Use N/A with a reason when a section does not apply. -->

# Summary

<!-- CABBAGE: Describe the data change, affected stores, ownership, and expected scale. -->

# Schema

| Object and field | Type | Nullable | Default | Constraint or index | Rationale |
|---|---|---|---|---|---|
| <!-- CABBAGE: Name the table, collection, and field. --> | <!-- CABBAGE: State the type. --> | <!-- CABBAGE: Yes or No. --> | <!-- CABBAGE: State the default. --> | <!-- CABBAGE: Describe keys, checks, or indexes. --> | <!-- CABBAGE: Explain the design choice. --> |

<!-- CABBAGE: Record invariants, relationships, retention, and ownership rules. -->

# Migration

| Phase | Operation | Batching or lock risk | Verification | Owner |
|---|---|---|---|---|
| <!-- CABBAGE: Name expand, backfill, switch, or contract phase. --> | <!-- CABBAGE: Describe the operation. --> | <!-- CABBAGE: Describe load, duration, and concurrency risk. --> | <!-- CABBAGE: Define pass criteria. --> | <!-- CABBAGE: Name the owner. --> |

# Data Verification

<!-- CABBAGE: Define row counts, checksums, invariants, reconciliation, and post-migration sampling. -->

# Compatibility and Operations

<!-- CABBAGE: Describe mixed-version behavior, query performance, capacity, replication, backups, and observability. -->

# Rollback

<!-- CABBAGE: Define rollback triggers, ordering, recovery commands, and irreversible data impact. -->
