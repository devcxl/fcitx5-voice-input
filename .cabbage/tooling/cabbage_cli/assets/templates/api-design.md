---
change: {{CHANGE_ID}}
cabbage_stage: {{STAGE_ID}}
change_type: {{CHANGE_TYPE}}
---

<!-- Replace every marked prompt before verifying this stage. Use N/A with a reason when a section does not apply. -->

# Overview

<!-- CABBAGE: Identify the API purpose, protocol, owners, and consumers. -->

# Contract

## Operations

| Method or event | Path or topic | Purpose | Authentication | Idempotency |
|---|---|---|---|---|
| <!-- CABBAGE: Name the method or event. --> | <!-- CABBAGE: Provide the path or topic. --> | <!-- CABBAGE: Describe the operation. --> | <!-- CABBAGE: State the required identity and permission. --> | <!-- CABBAGE: Describe retry and deduplication behavior. --> |

## Inputs and Outputs

| Name | Location | Type | Required | Validation or semantics |
|---|---|---|---|---|
| <!-- CABBAGE: Name a field. --> | <!-- CABBAGE: Path, query, header, or body. --> | <!-- CABBAGE: State the type. --> | <!-- CABBAGE: Yes or No. --> | <!-- CABBAGE: State constraints and meaning. --> |

<!-- CABBAGE: Provide representative request, response, or event examples without secrets. -->

# Error Model

| Code or condition | Meaning | Client action | Retryable |
|---|---|---|---|
| <!-- CABBAGE: Name an error. --> | <!-- CABBAGE: Explain the failure. --> | <!-- CABBAGE: Describe recovery. --> | <!-- CABBAGE: Yes or No, with policy. --> |

# Compatibility

<!-- CABBAGE: Describe versioning, backward compatibility, deprecation, and consumer migration. -->

# Security

<!-- CABBAGE: Describe authorization, input validation, sensitive fields, rate limits, and abuse controls. -->

# Observability

<!-- CABBAGE: Define logs, metrics, traces, audit events, SLOs, and alerts for the contract. -->
