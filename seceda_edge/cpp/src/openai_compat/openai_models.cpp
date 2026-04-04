#include "openai_compat/openai_compat.hpp"

namespace seceda::edge::openai_compat {

std::vector<ModelCatalogEntry> configured_model_catalog(const DaemonConfig & config) {
    std::vector<ModelCatalogEntry> models = config.exposed_models;

    auto add_unique = [&](const std::string & id, const std::string & display_name) {
        if (id.empty()) {
            return;
        }

        for (const auto & model : models) {
            if (model.id == id) {
                return;
            }
        }

        models.push_back({id, display_name.empty() ? id : display_name, "seceda"});
    };

    add_unique(config.public_model_alias, "Seceda default route");
    if (!config.local.model_alias.empty()) {
        add_unique(config.local.model_alias, config.local.display_name);
    }
    if (!config.cloud.model_alias.empty()) {
        add_unique(config.cloud.model_alias, config.cloud.display_name);
    }

    return models;
}

json model_catalog_entry_json(const ModelCatalogEntry & entry) {
    return {
        {"id", entry.id},
        {"object", "model"},
        {"created", 0},
        {"owned_by", entry.owned_by},
        {"display_name", entry.display_name},
    };
}

json models_list_payload(const DaemonConfig & config) {
    json payload = {
        {"object", "list"},
        {"data", json::array()},
    };
    for (const auto & model : configured_model_catalog(config)) {
        payload["data"].push_back(model_catalog_entry_json(model));
    }
    return payload;
}

}  // namespace seceda::edge::openai_compat
