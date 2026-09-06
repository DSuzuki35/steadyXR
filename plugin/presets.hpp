#pragma once
#include <obs-module.h>
#include <string>
#include <vector>
namespace vrst_plugin {
std::vector<std::string> preset_names(obs_data_t *settings);
bool save_preset(obs_data_t *settings, const std::string &name);
bool load_preset(obs_data_t *settings, const std::string &name);
bool delete_preset(obs_data_t *settings, const std::string &name);
std::string tuning_signature(obs_data_t *settings);
std::string pipeline_signature(obs_data_t *settings);
} // namespace vrst_plugin
