#pragma once
#include "../preview/renderer.hpp"
#include <obs-module.h>

namespace vrst_plugin {
void visual_defaults(obs_data_t *);
vrst::preview::VisualSettings visual_settings(obs_data_t *);
void visual_store(obs_data_t *, const vrst::preview::VisualSettings &);
void visual_add_properties(obs_properties_t *);
struct OpenXRSettings {
    vrst::preview::Options options;
    unsigned fps{60};
    bool paused{};
};
void openxr_defaults(obs_data_t *);
OpenXRSettings openxr_settings(obs_data_t *);
void openxr_store(obs_data_t *, const OpenXRSettings &);
void openxr_add_properties(obs_properties_t *);
bool openxr_import(obs_data_t *, const std::wstring &path, std::string &error);
} // namespace vrst_plugin
