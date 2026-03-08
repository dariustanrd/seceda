# Worklog for Seceda

## Getting started
- Structured the repo as a mixed C++ and Python monorepo, with first-party code under `seceda/` and vendored stacks under `thirdparty/`
- Followed https://modal.com/docs/examples/vllm_inference for `seceda/cloud/vllm_inference.py`
- Slightly modified it because I wanted to save costs: started with T4, but it was too old and had a few CUDA mismatch errors, so changed to L40S.
- Then realized the vLLM version was too old and not properly specified. Updated to the latest version and can now use Qwen3.5.

## Cold start optimization
- Realised cold start time is quite long (~1 min). Reading https://modal.com/docs/guide/cold-start and https://modal.com/docs/examples/lfm_snapshot#low-latency-serverless-lfm2-with-vllm-and-modal for more inspiration