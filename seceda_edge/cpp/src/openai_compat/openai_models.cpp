#include "openai_compat/openai_compat.hpp"

#include <utility>

namespace seceda::edge::openai_compat {

std::vector<ModelCatalogEntry> configured_model_catalog(const DaemonConfig & config) {
    std::vector<ModelCatalogEntry> models = config.exposed_models;

    auto add_unique = [&](ModelCatalogEntry entry) {
        if (entry.id.empty()) {
            return;
        }

        if (entry.display_name.empty()) {
            entry.display_name = entry.id;
        }
        if (entry.model_alias.empty()) {
            entry.model_alias = entry.id;
        }

        for (auto & model : models) {
            if (model.id != entry.id) {
                continue;
            }

            if (!entry.display_name.empty()) {
                model.display_name = entry.display_name;
            }
            if (!entry.owned_by.empty()) {
                model.owned_by = entry.owned_by;
            }
            if (entry.route_target != RouteTarget::kAuto) {
                model.route_target = entry.route_target;
            }
            if (!entry.engine_id.empty()) {
                model.engine_id = entry.engine_id;
            }
            if (!entry.backend_id.empty()) {
                model.backend_id = entry.backend_id;
            }
            if (!entry.model_id.empty()) {
                model.model_id = entry.model_id;
            }
            if (!entry.model_alias.empty()) {
                model.model_alias = entry.model_alias;
            }
            if (!entry.execution_mode.empty()) {
                model.execution_mode = entry.execution_mode;
            }
            if (!entry.capabilities.empty()) {
                model.capabilities = std::move(entry.capabilities);
            }
            return;
        }

        models.push_back(std::move(entry));
    };

    add_unique(
        {
            config.public_model_alias,
            "Seceda default route",
            "seceda",
            RouteTarget::kAuto,
            {},
            {},
            {},
            config.public_model_alias,
            {},
            {},
        });
    if (!config.local.model_alias.empty()) {
        add_unique(
            {
                config.local.model_alias,
                config.local.display_name,
                "seceda",
                RouteTarget::kLocal,
                config.local.engine_id,
                config.local.backend_id,
                config.local.model_id,
                config.local.model_alias,
                config.local.execution_mode,
                config.local.capabilities,
            });
    }
    if (!config.cloud.model_alias.empty()) {
        add_unique(
            {
                config.cloud.model_alias,
                config.cloud.display_name,
                "seceda",
                RouteTarget::kCloud,
                {},
                config.cloud.backend_id,
                config.cloud.model,
                config.cloud.model_alias,
                config.cloud.execution_mode,
                config.cloud.capabilities,
            });
    }
    for (const auto & backend : config.remote_backends) {
        add_unique(
            {
                backend.model_alias,
                backend.display_name,
                "seceda",
                RouteTarget::kCloud,
                {},
                backend.backend_id,
                backend.model,
                backend.model_alias,
                backend.execution_mode,
                backend.capabilities,
            });
    }

    return models;
}

json model_catalog_entry_json(const ModelCatalogEntry & entry) {
    json payload = {
        {"id", entry.id},
        {"object", "model"},
        {"created", 0},
        {"owned_by", entry.owned_by},
        {"display_name", entry.display_name},
    };

    json metadata = json::object();
    if (entry.route_target != RouteTarget::kAuto) {
        metadata["route_target"] = to_string(entry.route_target);
    }
    if (!entry.engine_id.empty()) {
        metadata["engine_id"] = entry.engine_id;
    }
    if (!entry.backend_id.empty()) {
        metadata["backend_id"] = entry.backend_id;
    }
    if (!entry.model_id.empty()) {
        metadata["model_id"] = entry.model_id;
    }
    if (!entry.model_alias.empty()) {
        metadata["model_alias"] = entry.model_alias;
    }
    if (!entry.execution_mode.empty()) {
        metadata["execution_mode"] = entry.execution_mode;
    }
    if (!entry.capabilities.empty()) {
        metadata["capabilities"] = entry.capabilities;
    }
    if (!metadata.empty()) {
        payload["metadata"] = std::move(metadata);
    }

    return payload;
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
