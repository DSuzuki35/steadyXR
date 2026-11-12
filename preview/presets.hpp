#pragma once
#include "theme.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vrst::preview {
enum class BuiltinPreset { natural, stream_comfort, strong_leveling, dan_suzuki };

// Snapshots are opaque INI bytes, preserving every section and future setting.
class PresetStore {
  public:
    explicit PresetStore(std::filesystem::path directory);
    static bool valid_name(const std::wstring &name, std::wstring &error);
    static bool valid_snapshot(const std::string &bytes, std::wstring &error);
    std::vector<std::wstring> list(std::wstring &error) const;
    std::optional<std::string> read(const std::wstring &name, std::wstring &error) const;
    bool save(const std::wstring &name, const std::string &bytes, bool overwrite, std::wstring &error) const;
    bool remove(const std::wstring &name, std::wstring &error) const;

  private:
    std::filesystem::path directory_;
};

class PresetsDialog {
  public:
    enum Id {
        names_id = 3001,
        name_id,
        save_id,
        overwrite_id,
        load_id,
        delete_id,
        natural_id,
        comfort_id,
        strong_id,
        status_id,
        close_id,
        dan_id
    };
    struct Callbacks {
        std::function<std::string()> capture;
        std::function<bool(const std::string &)> apply;
        std::function<void(BuiltinPreset)> builtin;
        std::function<void()> list_changed;
    };
    PresetsDialog();
    ~PresetsDialog();
    void initialize(HWND parent, Theme &, const std::filesystem::path &directory, Callbacks);
    void open(bool visible = true);
    HWND handle() const;
    bool route(MSG &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview
