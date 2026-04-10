#include "local_models/llama_runtime.hpp"

#include <iostream>
#include <string>

using namespace seceda::edge;

int main(int argc, char ** argv) {
    std::string model_path;
    std::string prompt = "Explain why edge inference can reduce latency.";
    int context_size = 2048;
    int max_tokens = 64;
    int n_gpu_layers = 99;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--model-path" && i + 1 < argc) {
            model_path = argv[++i];
        } else if (arg == "--prompt" && i + 1 < argc) {
            prompt = argv[++i];
        } else if (arg == "--n-ctx" && i + 1 < argc) {
            context_size = std::stoi(argv[++i]);
        } else if (arg == "--max-tokens" && i + 1 < argc) {
            max_tokens = std::stoi(argv[++i]);
        } else if (arg == "--n-gpu-layers" && i + 1 < argc) {
            n_gpu_layers = std::stoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: " << argv[0]
                << " --model-path MODEL.gguf [--prompt TEXT] [--n-ctx N] [--max-tokens N]\n";
            return 0;
        }
    }

    if (model_path.empty()) {
        std::cerr << "--model-path is required\n";
        return 1;
    }

    LocalModelConfig config;
    config.model_path = model_path;
    config.context_size = context_size;
    config.batch_size = context_size;
    config.n_gpu_layers = n_gpu_layers;

    LlamaRuntime runtime;
    std::string error;
    if (!runtime.load(config, "Hello from Seceda.", error)) {
        std::cerr << "Failed to load runtime: " << error << std::endl;
        return 1;
    }

    InferenceRequest request;
    request.text = prompt;
    request.options.max_tokens = max_tokens;

    const auto response = runtime.generate(request);
    if (!response.ok) {
        std::cerr << "Inference failed: " << response.error << std::endl;
        return 1;
    }

    std::cout << "Model: " << response.active_model_path << "\n";
    std::cout << "TTFT (ms): "
              << (response.timing.has_ttft ? response.timing.ttft_ms : 0.0) << "\n";
    std::cout << "Total latency (ms): " << response.timing.total_latency_ms << "\n";
    std::cout << "Generated tokens: " << response.timing.generated_tokens << "\n";
    std::cout << "Response:\n" << response.text << std::endl;
    return 0;
}
