---
change: {{CHANGE_ID}}
cabbage_stage: {{STAGE_ID}}
change_type: {{CHANGE_TYPE}}
---

<!-- Replace every marked prompt before verifying this stage. Use N/A with a reason when a section does not apply. -->

# Strategy

<!-- CABBAGE: Define the scope, test levels, public test seams, and key quality risks. -->

| Level | Scope | Test seam | Owner |
|---|---|---|---|
| <!-- CABBAGE: Unit, integration, contract, E2E, or manual. --> | <!-- CABBAGE: State what is covered. --> | <!-- CABBAGE: Name the public interface or observable result. --> | <!-- CABBAGE: Name the owner. --> |

# Test Environment and Data

<!-- CABBAGE: Describe required environments, fixtures, accounts, data setup, isolation, and cleanup. -->

# Cases

| ID | Scenario | Level | Expected result | Priority |
|---|---|---|---|---|
| T-1 | <!-- CABBAGE: Describe a critical happy path or failure case. --> | <!-- CABBAGE: Name the test level. --> | <!-- CABBAGE: State the observable outcome. --> | High |

# Regression Coverage

- <!-- CABBAGE: Identify existing behavior and tests that protect it from regression. -->

# Non-functional Testing

| Quality attribute | Method | Threshold |
|---|---|---|
| <!-- CABBAGE: Performance, security, resilience, accessibility, or another attribute. --> | <!-- CABBAGE: Describe the verification method. --> | <!-- CABBAGE: Define the pass threshold. --> |

# Entry and Exit Criteria

- Entry: <!-- CABBAGE: Define prerequisites for starting verification. -->
- Exit: <!-- CABBAGE: Define evidence required to finish verification. -->

# Risks

- <!-- CABBAGE: Record coverage gaps, flaky dependencies, or untestable assumptions and their mitigations. -->
