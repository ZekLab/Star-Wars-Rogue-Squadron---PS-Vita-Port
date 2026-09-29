// Mod support compiled out (PS Vita: no dynamic libraries and no executable memory for the live recompiler).
// Replaces mods.cpp, mod_events.cpp, mod_hooks.cpp, mod_manifest.cpp and mod_config_api.cpp with no-op definitions.
// The mod containers stay empty, so none of the ModHandle/code-handle paths are ever reached.

#include "librecomp/mods.hpp"
#include "recompiler/live_recompiler.h"

namespace recomp::mods {
    class DynamicLibrary {};
}

extern "C" {
    uint32_t builtin_base_event_index = 0;
}

extern "C" void recomp_trigger_event(uint8_t* rdram, recomp_context* ctx, uint32_t event_index) {}

// Live recompiler pieces referenced by headers but never instantiated here.
void N64Recomp::live_recompiler_init() {}
N64Recomp::ShimFunction::~ShimFunction() {}
N64Recomp::LiveGeneratorOutput::~LiveGeneratorOutput() {}

// LiveRecompilerCodeHandle: out-of-line virtuals so its vtable exists for ModContext's member destructors.
void recomp::mods::LiveRecompilerCodeHandle::set_imported_function(size_t, GenericFunction) {}
recomp::mods::CodeModLoadError recomp::mods::LiveRecompilerCodeHandle::populate_reference_symbols(const N64Recomp::Context&, std::string&) { return CodeModLoadError::InternalError; }
recomp::mods::GenericFunction recomp::mods::LiveRecompilerCodeHandle::get_function_handle(size_t) { return GenericFunction{ static_cast<recomp_func_t*>(nullptr) }; }

// ModHandle
recomp::mods::ModHandle::ModHandle(ModHandle&& rhs) = default;
recomp::mods::ModHandle& recomp::mods::ModHandle::operator=(ModHandle&& rhs) = default;
recomp::mods::ModHandle::~ModHandle() = default;

// ModContext
recomp::mods::ModContext::ModContext() {}
recomp::mods::ModContext::~ModContext() {}
void recomp::mods::ModContext::register_game(const std::string&) {}
void recomp::mods::ModContext::register_embedded_mod(const std::string&, std::span<const uint8_t>) {}
std::vector<recomp::mods::ModOpenErrorDetails> recomp::mods::ModContext::scan_mod_folder(const std::filesystem::path&) { return {}; }
void recomp::mods::ModContext::close_mods() {}
void recomp::mods::ModContext::load_mods_config() {}
void recomp::mods::ModContext::enable_mod(const std::string&, bool, bool) {}
bool recomp::mods::ModContext::is_mod_enabled(const std::string&) { return false; }
bool recomp::mods::ModContext::is_mod_auto_enabled(const std::string&) { return false; }
size_t recomp::mods::ModContext::num_opened_mods() { return 0; }
std::vector<recomp::mods::ModLoadErrorDetails> recomp::mods::ModContext::load_mods(const GameEntry&, uint8_t*, int32_t, uint32_t& ram_used) {
    ram_used = 0;
    return {};
}
void recomp::mods::ModContext::unload_mods() {}
std::string recomp::mods::ModContext::get_mod_id_from_filename(const std::filesystem::path&) const { return {}; }
std::filesystem::path recomp::mods::ModContext::get_mod_filename(const std::string&) const { return {}; }
recomp_func_t* recomp::mods::ModContext::get_mod_export(const std::string&, const std::string&) const { return nullptr; }
size_t recomp::mods::ModContext::get_mod_order_index(const std::string&) const { return (size_t)-1; }
size_t recomp::mods::ModContext::get_mod_order_index(size_t) const { return (size_t)-1; }
std::optional<recomp::mods::ModDetails> recomp::mods::ModContext::get_details_for_mod(const std::string&) const { return std::nullopt; }
std::vector<recomp::mods::ModDetails> recomp::mods::ModContext::get_all_mod_details(const std::string&) { return {}; }
recomp::Version recomp::mods::ModContext::get_mod_version(size_t) { return {}; }
std::string recomp::mods::ModContext::get_mod_id(size_t) { return {}; }
void recomp::mods::ModContext::set_mod_index(const std::string&, const std::string&, size_t) {}
const recomp::mods::ConfigSchema& recomp::mods::ModContext::get_mod_config_schema(const std::string&) const { return empty_schema; }
const std::vector<char>& recomp::mods::ModContext::get_mod_thumbnail(const std::string&) const { return empty_bytes; }
void recomp::mods::ModContext::set_mod_config_value(size_t, const std::string&, const ConfigValueVariant&) {}
void recomp::mods::ModContext::set_mod_config_value(const std::string&, const std::string&, const ConfigValueVariant&) {}
recomp::mods::ConfigValueVariant recomp::mods::ModContext::get_mod_config_value(size_t, const std::string&) const { return std::monostate{}; }
recomp::mods::ConfigValueVariant recomp::mods::ModContext::get_mod_config_value(const std::string&, const std::string&) const { return std::monostate{}; }
void recomp::mods::ModContext::set_mods_config_path(const std::filesystem::path&) {}
void recomp::mods::ModContext::set_mod_config_directory(const std::filesystem::path&) {}
recomp::mods::ModContentTypeId recomp::mods::ModContext::register_content_type(const ModContentType&) { return ModContentTypeId{ 0 }; }
bool recomp::mods::ModContext::register_container_type(const std::string&, const std::vector<ModContentTypeId>&, bool) { return true; }
bool recomp::mods::ModContext::is_content_runtime_toggleable(ModContentTypeId) const { return false; }
std::string recomp::mods::ModContext::get_mod_display_name(size_t) const { return {}; }
std::filesystem::path recomp::mods::ModContext::get_mod_path(size_t) const { return {}; }
std::pair<std::string, std::string> recomp::mods::ModContext::get_mod_import_info(size_t, size_t) const { return {}; }
recomp::mods::DependencyStatus recomp::mods::ModContext::is_dependency_met(size_t, const std::string&) const { return DependencyStatus::NotFound; }

// Events and hooks
void recomp::mods::setup_events(size_t) {}
void recomp::mods::register_event_callback(size_t, size_t, GenericFunction) {}
void recomp::mods::finish_event_setup(const ModContext&) {}
void recomp::mods::reset_events() {}
void recomp::mods::setup_hooks(size_t) {}
void recomp::mods::set_hook_type(size_t, bool) {}
void recomp::mods::register_hook(size_t, size_t, GenericFunction) {}
void recomp::mods::finish_hook_setup(const ModContext&) {}
void recomp::mods::reset_hooks() {}
void recomp::mods::register_hook_exports() {}
void recomp::mods::run_hook(uint8_t*, recomp_context*, size_t) {}
void recomp::mods::register_config_exports() {}

// Manifest and errors
recomp::mods::ModOpenError recomp::mods::parse_manifest(ModManifest&, const std::vector<char>&, std::string&) { return ModOpenError::InvalidManifestSchema; }
recomp::mods::CodeModLoadError recomp::mods::validate_api_version(uint32_t, std::string&) { return CodeModLoadError::UnsupportedApiVersion; }
void recomp::mods::unmet_dependency_handler(uint8_t*, recomp_context*, uintptr_t) {}
std::string recomp::mods::error_to_string(ModOpenError) { return "Mods are not supported on this platform"; }
std::string recomp::mods::error_to_string(ModLoadError) { return "Mods are not supported on this platform"; }
std::string recomp::mods::error_to_string(CodeModLoadError) { return "Mods are not supported on this platform"; }
