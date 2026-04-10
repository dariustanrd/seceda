#include "local_models/llama_runtime.hpp"

#include <llama.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

namespace seceda::edge {
namespace {

struct SamplerDeleter {
    void operator()(llama_sampler * sampler) const {
        if (sampler != nullptr) {
            llama_sampler_free(sampler);
        }
    }
};

std::string normalize_local_role(const std::string & role) {
    if (role == "developer") {
        return "system";
    }
    if (role == "function") {
        return "tool";
    }
    if (role.empty()) {
        return "user";
    }
    return role;
}

std::string fallback_prompt(const InferenceRequest & request) {
    std::string prompt;
    for (const auto & message : request.messages) {
        if (message.content.empty()) {
            continue;
        }

        const std::string role = normalize_local_role(message.role);
        if (!prompt.empty()) {
            prompt += "\n";
        }

        if (role == "system") {
            prompt += "System";
        } else if (role == "assistant") {
            prompt += "Assistant";
        } else if (role == "tool") {
            prompt += "Tool";
        } else {
            prompt += "User";
        }

        if (!message.name.empty()) {
            prompt += "(" + message.name + ")";
        }
        prompt += ": ";
        prompt += message.content;
    }

    if (!prompt.empty()) {
        prompt += "\nAssistant:";
    }

    return prompt;
}

bool should_use_sampling(const GenerationOptions & options) {
    return options.temperature > 0.0f;
}

std::unique_ptr<llama_sampler, SamplerDeleter> create_sampler(const GenerationOptions & options) {
    auto chain_params = llama_sampler_chain_default_params();
    std::unique_ptr<llama_sampler, SamplerDeleter> sampler(
        llama_sampler_chain_init(chain_params));

    if (!should_use_sampling(options)) {
        llama_sampler_chain_add(sampler.get(), llama_sampler_init_greedy());
        return sampler;
    }

    if (options.top_k > 0) {
        llama_sampler_chain_add(sampler.get(), llama_sampler_init_top_k(options.top_k));
    }
    if (options.top_p > 0.0f && options.top_p < 1.0f) {
        llama_sampler_chain_add(sampler.get(), llama_sampler_init_top_p(options.top_p, 1));
    }
    if (options.min_p > 0.0f) {
        llama_sampler_chain_add(sampler.get(), llama_sampler_init_min_p(options.min_p, 1));
    }

    llama_sampler_chain_add(sampler.get(), llama_sampler_init_temp(options.temperature));
    llama_sampler_chain_add(sampler.get(), llama_sampler_init_dist(options.seed));
    return sampler;
}

std::string token_to_piece(const llama_vocab * vocab, llama_token token, std::string & error) {
    char buffer[256];
    const int written = llama_token_to_piece(vocab, token, buffer, sizeof(buffer), 0, true);
    if (written < 0) {
        error = "Failed to decode generated token";
        return {};
    }

    return std::string(buffer, buffer + written);
}

std::string build_prompt(
    llama_model * model,
    const InferenceRequest & request,
    std::string & error) {
    if (request.messages.empty()) {
        error = "Request messages must not be empty";
        return {};
    }

    if (!request.options.use_chat_template) {
        return fallback_prompt(request);
    }

    const char * chat_template = llama_model_chat_template(model, nullptr);
    if (chat_template == nullptr) {
        return fallback_prompt(request);
    }

    std::vector<std::string> roles;
    std::vector<std::string> contents;
    std::vector<llama_chat_message> messages;
    roles.reserve(request.messages.size());
    contents.reserve(request.messages.size());
    messages.reserve(request.messages.size());

    for (const auto & message : request.messages) {
        if (message.content.empty()) {
            continue;
        }
        roles.push_back(normalize_local_role(message.role));
        contents.push_back(message.content);
        messages.push_back({roles.back().c_str(), contents.back().c_str()});
    }

    if (messages.empty()) {
        error = "Request messages must include at least one text content block";
        return {};
    }

    std::vector<char> formatted(
        std::max<std::size_t>(
            256,
            request.normalized.routing_prompt.size() * 2 + 64));
    int required = llama_chat_apply_template(
        chat_template,
        messages.data(),
        messages.size(),
        true,
        formatted.data(),
        static_cast<int>(formatted.size()));
    if (required < 0) {
        error = "Failed to apply chat template";
        return {};
    }
    if (required > static_cast<int>(formatted.size())) {
        formatted.resize(required + 1);
        required = llama_chat_apply_template(
            chat_template,
            messages.data(),
            messages.size(),
            true,
            formatted.data(),
            static_cast<int>(formatted.size()));
        if (required < 0) {
            error = "Failed to apply chat template";
            return {};
        }
    }

    return std::string(formatted.data(), formatted.data() + required);
}

bool apply_stop_sequences(std::string & text, const std::vector<std::string> & stop_sequences) {
    std::size_t first_match = std::string::npos;
    for (const auto & stop_sequence : stop_sequences) {
        if (stop_sequence.empty()) {
            continue;
        }

        const std::size_t match = text.find(stop_sequence);
        if (match != std::string::npos && (first_match == std::string::npos || match < first_match)) {
            first_match = match;
        }
    }

    if (first_match == std::string::npos) {
        return false;
    }

    text.erase(first_match);
    return true;
}

std::size_t first_stop_sequence_match(
    const std::string & text,
    const std::vector<std::string> & stop_sequences) {
    std::size_t first_match = std::string::npos;
    for (const auto & stop_sequence : stop_sequences) {
        if (stop_sequence.empty()) {
            continue;
        }

        const std::size_t match = text.find(stop_sequence);
        if (match != std::string::npos && (first_match == std::string::npos || match < first_match)) {
            first_match = match;
        }
    }

    return first_match;
}

std::size_t max_stop_sequence_holdback(const std::vector<std::string> & stop_sequences) {
    std::size_t holdback = 0;
    for (const auto & stop_sequence : stop_sequences) {
        if (stop_sequence.size() > holdback) {
            holdback = stop_sequence.size();
        }
    }

    return holdback == 0 ? 0 : holdback - 1;
}

bool emit_stream_safe_prefix(
    const std::string & text,
    std::size_t safe_end,
    std::size_t & streamed_size,
    const StreamDeltaCallback & on_delta,
    std::string & error) {
    if (safe_end <= streamed_size) {
        return true;
    }

    StreamedChatDelta delta;
    delta.content = text.substr(streamed_size, safe_end - streamed_size);
    if (!delta.content.empty() && !on_delta(delta)) {
        error = "Streaming cancelled by client";
        return false;
    }

    streamed_size = safe_end;
    return true;
}

}  // namespace

struct LlamaRuntime::Bundle {
    LocalModelConfig config;
    llama_model * model = nullptr;
    llama_context * context = nullptr;
    const llama_vocab * vocab = nullptr;
    LocalModelInfo info;

    ~Bundle() {
        if (context != nullptr) {
            llama_free(context);
            context = nullptr;
        }
        if (model != nullptr) {
            llama_model_free(model);
            model = nullptr;
        }
    }
};

LlamaRuntime::LlamaRuntime() = default;
LlamaRuntime::~LlamaRuntime() = default;

bool LlamaRuntime::load(
    const LocalModelConfig & config,
    const std::string & warmup_prompt,
    std::string & error) {
    auto new_bundle = load_bundle(config, error);
    if (!new_bundle) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = error;
        return false;
    }

    if (!warmup_prompt.empty()) {
        InferenceRequest warmup_request;
        warmup_request.messages.push_back({"user", warmup_prompt, {}});
        refresh_request_views(warmup_request);
        warmup_request.options.max_completion_tokens = 1;
        warmup_request.options.temperature = 0.0f;
        auto warmup = generate_with_bundle(*new_bundle, warmup_request, true);
        if (!warmup.ok) {
            error = warmup.error;
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_ = error;
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    bundle_ = std::move(new_bundle);
    last_error_.clear();
    return true;
}

bool LlamaRuntime::reload(
    const LocalModelConfig & config,
    const std::string & warmup_prompt,
    std::string & error) {
    auto new_bundle = load_bundle(config, error);
    if (!new_bundle) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = error;
        return false;
    }

    if (!warmup_prompt.empty()) {
        InferenceRequest warmup_request;
        warmup_request.messages.push_back({"user", warmup_prompt, {}});
        refresh_request_views(warmup_request);
        warmup_request.options.max_completion_tokens = 1;
        warmup_request.options.temperature = 0.0f;
        auto warmup = generate_with_bundle(*new_bundle, warmup_request, true);
        if (!warmup.ok) {
            error = warmup.error;
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_ = error;
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    bundle_ = std::move(new_bundle);
    last_error_.clear();
    return true;
}

bool LlamaRuntime::is_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bundle_ != nullptr;
}

LocalModelInfo LlamaRuntime::info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bundle_ != nullptr) {
        auto info = bundle_->info;
        info.last_error = last_error_;
        return info;
    }

    LocalModelInfo info;
    info.last_error = last_error_;
    return info;
}

LocalCompletionResult LlamaRuntime::generate(const InferenceRequest & request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bundle_ == nullptr) {
        LocalCompletionResult result;
        result.error = "Local llama runtime is not loaded";
        return result;
    }

    auto result = generate_with_bundle(*bundle_, request, false);
    if (!result.ok) {
        last_error_ = result.error;
    }
    return result;
}

LocalCompletionResult LlamaRuntime::generate_stream(
    const InferenceRequest & request,
    const StreamDeltaCallback & on_delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bundle_ == nullptr) {
        LocalCompletionResult result;
        result.error = "Local llama runtime is not loaded";
        return result;
    }

    auto result = generate_with_bundle(*bundle_, request, false, &on_delta);
    if (!result.ok) {
        last_error_ = result.error;
    }
    return result;
}

bool LlamaRuntime::backend_ready() {
    static std::once_flag init_once;
    static bool initialized = false;

    std::call_once(init_once, []() {
        llama_log_set(
            [](ggml_log_level level, const char * text, void *) {
                if (level >= GGML_LOG_LEVEL_ERROR) {
                    std::fprintf(stderr, "%s", text);
                }
            },
            nullptr);
        ggml_backend_load_all();
        initialized = true;
    });

    return initialized;
}

std::unique_ptr<LlamaRuntime::Bundle> LlamaRuntime::load_bundle(
    const LocalModelConfig & config,
    std::string & error) {
    error.clear();
    if (config.model_path.empty()) {
        error = "Local model path is empty";
        return nullptr;
    }
    if (!backend_ready()) {
        error = "Failed to initialize llama.cpp backends";
        return nullptr;
    }

    auto bundle = std::make_unique<Bundle>();
    bundle->config = config;

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = config.n_gpu_layers;

    bundle->model = llama_model_load_from_file(config.model_path.c_str(), model_params);
    if (bundle->model == nullptr) {
        error = "Unable to load model from " + config.model_path;
        return nullptr;
    }

    bundle->vocab = llama_model_get_vocab(bundle->model);

    llama_context_params context_params = llama_context_default_params();
    context_params.n_ctx = config.context_size;
    context_params.n_batch = std::min(config.batch_size, config.context_size);
    context_params.no_perf = false;

    bundle->context = llama_init_from_model(bundle->model, context_params);
    if (bundle->context == nullptr) {
        error = "Unable to initialize llama context for " + config.model_path;
        return nullptr;
    }

    const int configured_threads =
        config.n_threads > 0 ? config.n_threads : llama_n_threads(bundle->context);
    const int configured_threads_batch =
        config.n_threads_batch > 0 ? config.n_threads_batch : llama_n_threads_batch(bundle->context);
    llama_set_n_threads(bundle->context, configured_threads, configured_threads_batch);

    char description[256] = {};
    llama_model_desc(bundle->model, description, sizeof(description));

    bundle->info.ready = true;
    bundle->info.engine_id = config.engine_id;
    bundle->info.backend_id = config.backend_id;
    bundle->info.model_id = config.model_id;
    bundle->info.model_alias = config.model_alias;
    bundle->info.display_name = config.display_name.empty() ? description : config.display_name;
    bundle->info.execution_mode = config.execution_mode;
    bundle->info.capabilities = config.capabilities;
    bundle->info.model_path = config.model_path;
    bundle->info.description = description;
    bundle->info.context_size = config.context_size;
    bundle->info.batch_size = context_params.n_batch;
    bundle->info.n_gpu_layers = config.n_gpu_layers;
    bundle->info.has_chat_template = llama_model_chat_template(bundle->model, nullptr) != nullptr;
    bundle->info.model_size_bytes = llama_model_size(bundle->model);
    bundle->info.parameter_count = llama_model_n_params(bundle->model);
    bundle->info.trained_context_size = llama_model_n_ctx_train(bundle->model);
    bundle->info.layer_count = llama_model_n_layer(bundle->model);

    return bundle;
}

LocalCompletionResult LlamaRuntime::generate_with_bundle(
    Bundle & bundle,
    const InferenceRequest & request,
    bool warmup_mode,
    const StreamDeltaCallback * on_delta) {
    LocalCompletionResult result;
    result.active_model_path = bundle.info.model_path;
    result.identity.route_target = RouteTarget::kLocal;
    result.identity.engine_id = bundle.info.engine_id;
    result.identity.backend_id = bundle.info.backend_id;
    result.identity.model_id = bundle.info.model_id;
    result.identity.model_alias = bundle.info.model_alias;
    result.identity.display_name = bundle.info.display_name;
    result.identity.execution_mode = bundle.info.execution_mode;
    result.identity.capabilities = bundle.info.capabilities;

    if (bundle.context == nullptr || bundle.model == nullptr || bundle.vocab == nullptr) {
        result.error = "llama runtime bundle is incomplete";
        return result;
    }

    std::string prompt_error;
    const std::string prompt = build_prompt(bundle.model, request, prompt_error);
    if (!prompt_error.empty()) {
        result.error = prompt_error;
        return result;
    }

    const int prompt_token_count =
        -llama_tokenize(bundle.vocab, prompt.c_str(), prompt.size(), nullptr, 0, true, true);
    if (prompt_token_count <= 0) {
        result.error = "Failed to tokenize prompt";
        return result;
    }

    if (prompt_token_count + request.options.max_completion_tokens > bundle.config.context_size) {
        result.error = "Prompt and generation budget exceed configured context size";
        return result;
    }

    std::vector<llama_token> prompt_tokens(prompt_token_count);
    if (llama_tokenize(
            bundle.vocab,
            prompt.c_str(),
            prompt.size(),
            prompt_tokens.data(),
            static_cast<int>(prompt_tokens.size()),
            true,
            true) < 0) {
        result.error = "Failed to tokenize prompt";
        return result;
    }

    llama_memory_clear(llama_get_memory(bundle.context), true);
    llama_set_warmup(bundle.context, warmup_mode);

    auto sampler = create_sampler(request.options);
    const auto request_start = std::chrono::steady_clock::now();

    if (llama_model_has_encoder(bundle.model)) {
        if (static_cast<int>(prompt_tokens.size()) > bundle.config.batch_size) {
            result.error = "Encoder-decoder prompt exceeds configured batch size";
            llama_set_warmup(bundle.context, false);
            return result;
        }

        llama_batch encoder_batch =
            llama_batch_get_one(prompt_tokens.data(), static_cast<int>(prompt_tokens.size()));
        if (llama_encode(bundle.context, encoder_batch) != 0) {
            result.error = "llama_encode failed during prompt processing";
            llama_set_warmup(bundle.context, false);
            return result;
        }

        llama_token decoder_start_token = llama_model_decoder_start_token(bundle.model);
        if (decoder_start_token == LLAMA_TOKEN_NULL) {
            decoder_start_token = llama_vocab_bos(bundle.vocab);
        }
        llama_batch decoder_batch = llama_batch_get_one(&decoder_start_token, 1);
        if (llama_decode(bundle.context, decoder_batch) != 0) {
            result.error = "llama_decode failed to prime decoder";
            llama_set_warmup(bundle.context, false);
            return result;
        }
    } else {
        for (std::size_t offset = 0; offset < prompt_tokens.size(); offset += bundle.config.batch_size) {
            const int chunk_size = static_cast<int>(
                std::min<std::size_t>(bundle.config.batch_size, prompt_tokens.size() - offset));
            llama_batch batch = llama_batch_get_one(prompt_tokens.data() + offset, chunk_size);
            if (llama_decode(bundle.context, batch) != 0) {
                result.error = "llama_decode failed during prompt processing";
                llama_set_warmup(bundle.context, false);
                return result;
            }
        }
    }

    result.timing.prompt_tokens = prompt_token_count;
    bool stopped_normally = false;
    bool stopped_on_sequence = false;
    std::string generated_text;
    std::size_t streamed_size = 0;
    const bool stream_enabled = !warmup_mode && on_delta != nullptr;
    const std::size_t stop_holdback = max_stop_sequence_holdback(request.advanced.stop_sequences);

    for (int generated = 0; generated < request.options.max_completion_tokens; ++generated) {
        const llama_token next_token = llama_sampler_sample(sampler.get(), bundle.context, -1);
        if (llama_vocab_is_eog(bundle.vocab, next_token)) {
            stopped_normally = true;
            break;
        }

        if (!result.timing.has_ttft) {
            result.timing.has_ttft = true;
            result.timing.ttft_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - request_start)
                                        .count();
        }

        std::string piece_error;
        const std::string piece = token_to_piece(bundle.vocab, next_token, piece_error);
        if (!piece_error.empty()) {
            result.error = piece_error;
            llama_set_warmup(bundle.context, false);
            return result;
        }

        if (!warmup_mode) {
            generated_text += piece;
            if (stream_enabled) {
                const std::size_t stop_match =
                    first_stop_sequence_match(generated_text, request.advanced.stop_sequences);
                if (stop_match != std::string::npos) {
                    if (!emit_stream_safe_prefix(
                            generated_text,
                            stop_match,
                            streamed_size,
                            *on_delta,
                            result.error)) {
                        llama_set_warmup(bundle.context, false);
                        return result;
                    }
                    generated_text.erase(stop_match);
                    stopped_on_sequence = true;
                    result.timing.generated_tokens += 1;
                    break;
                }

                const std::size_t safe_emit_end =
                    generated_text.size() > stop_holdback ? generated_text.size() - stop_holdback : 0;
                if (!emit_stream_safe_prefix(
                        generated_text,
                        safe_emit_end,
                        streamed_size,
                        *on_delta,
                        result.error)) {
                    llama_set_warmup(bundle.context, false);
                    return result;
                }
            }
        }
        result.timing.generated_tokens += 1;

        if (generated + 1 >= request.options.max_completion_tokens) {
            break;
        }

        llama_token sampled_token = next_token;
        llama_batch step_batch = llama_batch_get_one(&sampled_token, 1);
        if (llama_decode(bundle.context, step_batch) != 0) {
            result.error = "llama_decode failed during generation";
            llama_set_warmup(bundle.context, false);
            return result;
        }
    }

    if (!warmup_mode && stream_enabled && !stopped_on_sequence) {
        if (!emit_stream_safe_prefix(
                generated_text,
                generated_text.size(),
                streamed_size,
                *on_delta,
                result.error)) {
            llama_set_warmup(bundle.context, false);
            return result;
        }
    }

    if (!warmup_mode) {
        result.text = generated_text;
    }

    if (!warmup_mode && !stream_enabled) {
        stopped_on_sequence = apply_stop_sequences(result.text, request.advanced.stop_sequences);
    }

    result.ok = true;
    result.message.role = "assistant";
    result.message.content = result.text;
    result.finish_reason = (stopped_normally || stopped_on_sequence) ? "stop" : "length";
    result.timing.total_latency_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - request_start)
                                         .count();
    llama_set_warmup(bundle.context, false);
    return result;
}

}  // namespace seceda::edge
