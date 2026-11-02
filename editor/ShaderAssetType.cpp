// Registers .shader files as the "Shader" asset type in the editor, so they
// show in the asset browser and in a component's shader picker, and ships
// them.
//
// A shader is text the runtime reads as it is (ShaderAsset.h): there is
// nothing to compile. But a build ships only what the cache holds, so the
// sync handler copies the source there, under its GUID, as deki-3d does with
// the meshes it compiles.

#ifdef DEKI_EDITOR

#include "../ShaderAsset.h"

#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <deki-editor/EditorExtension.h>
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/Localization.h>
#include <deki-editor/Paths.h>
#include <deki/LogSystem.h>
#include <deki/assets/AssetManager.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace DekiEditor
{
namespace
{

class ShaderAssetType : public AssetTypeEditor
{
public:
    const char* GetTypeName() const override { return DekiRendering::ShaderAsset::kAssetTypeName; }
    const char* GetDisplayName() const override { return TrMark("Shader"); }
    std::vector<std::string> GetExtensions() const override { return { ".shader" }; }
};

REGISTER_EDITOR(ShaderAssetType)

bool IsShader(const std::string& path)
{
    return path.size() >= 7 && path.compare(path.size() - 7, 7, ".shader") == 0;
}

/// Copies one .shader into the cache and points its GUID there.
void HandleShaderSync(const std::string& absolutePath, const std::string& guid, const std::string& projectPath)
{
    const fs::path cachePath = fs::path(GetCacheDirectory(projectPath)) / guid;
    std::error_code ec;
    fs::create_directories(cachePath.parent_path(), ec);
    fs::copy_file(absolutePath, cachePath, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        DEKI_LOG_WARNING("Shader: could not cache %s: %s", absolutePath.c_str(), ec.message().c_str());
        return;
    }
    if (auto* assets = Deki::AssetManager::Get())
    {
        assets->RegisterGuid(guid, guid);
    }
}

/// Points every already-cached shader at its cache entry: the sync handler
/// runs only when a source changes, so a project opening with a warm cache
/// needs this.
void RegisterCachedShaders(AssetPipeline* pipeline, const std::string& projectPath)
{
    auto* assets = Deki::AssetManager::Get();
    if (!pipeline || !assets)
    {
        return;
    }
    const fs::path cacheDir(GetCacheDirectory(projectPath));
    for (const auto& entry : pipeline->GetAllAssets())
    {
        const std::string& guid = entry.second.guid;
        std::error_code ec;
        if (IsShader(entry.first) && !guid.empty() && fs::exists(cacheDir / guid, ec))
        {
            assets->RegisterGuid(guid, guid);
        }
    }
}

// A static initialiser, not work in the package entry point, so the vtable of
// the AssetTypeEditor above stays stable across package rebuilds (see the
// note in EditorExtension.h).
struct ShaderAssetRegistrar
{
    ShaderAssetRegistrar()
    {
        AssetTypeRegistry::Instance().RegisterCategory(".shader", AssetCategory::Data);
        AssetPipeline::OnStarted([](AssetPipeline* pipeline)
                                 { pipeline->RegisterSyncHandler(".shader", HandleShaderSync); });
        AssetPipeline::OnImportComplete([](AssetPipeline* pipeline)
                                        { RegisterCachedShaders(pipeline, pipeline->GetProjectPath()); });
    }
};
ShaderAssetRegistrar s_ShaderAssetRegistrar;

}  // namespace
}  // namespace DekiEditor

#endif  // DEKI_EDITOR
