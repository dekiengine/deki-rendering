// Registers .shader files as the "Shader" asset type in the editor, so they
// show in the asset browser and in a component's shader picker, and compiles
// them.
//
// A .shader is HLSL. The sync handler compiles it for every use whose hooks
// it defines (ShaderUses.h) into the cache, under its GUID, as deki-3d does
// with the meshes it compiles; the runtime loads that (ShaderAsset.h). A
// build compiles it again for its target: a target with OpenGL gets the same,
// one without stops the build, since nothing there can draw it.
//
// Every problem is said, with the file and line: the Console shows what a
// change broke, and the build lists what stops it.

#ifdef DEKI_EDITOR

#include "../ShaderAsset.h"
#include "ShaderUses.h"

#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <deki-editor/EditorExtension.h>
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/Localization.h>
#include <deki-editor/Paths.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki/LogSystem.h>
#include <deki/assets/AssetManager.h>

#include <filesystem>
#include <fstream>
#include <sstream>
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

/// Compiles the .shader at `absolutePath` and writes the asset to `outPath`.
/// False with the reason in `error`, and nothing written, when it does not
/// compile. `name` is what errors call the file.
bool CompileShaderAsset(const std::string& absolutePath, const std::string& name, const std::string& outPath,
                        std::string& error)
{
    std::ifstream in(absolutePath, std::ios::binary);
    if (!in)
    {
        error = name + ": cannot be read";
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();

    DekiRendering::ShaderCompileResult result;
    if (!DekiRendering::CompileShaderFile(text.str(), name, result))
    {
        error = result.errors;
        return false;
    }
    const std::vector<uint8_t> blob = DekiRendering::ShaderAsset::Encode(result.programs);
    std::error_code ec;
    fs::create_directories(fs::path(outPath).parent_path(), ec);
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size())))
    {
        error = name + ": the compiled shader cannot be written to " + outPath;
        return false;
    }
    return true;
}

/// The project-relative path errors name a file by.
std::string DisplayName(const std::string& absolutePath, const std::string& projectPath)
{
    std::error_code ec;
    const fs::path relative = fs::relative(absolutePath, projectPath, ec);
    return (ec || relative.empty() ? fs::path(absolutePath).filename() : relative).generic_string();
}

/// Compiles one .shader into the cache and points its GUID there. One that
/// does not compile leaves no cache entry, so nothing draws an older version
/// of it, and its errors go to the Console.
void HandleShaderSync(const std::string& absolutePath, const std::string& guid, const std::string& projectPath)
{
    const fs::path cachePath = fs::path(GetCacheDirectory(projectPath)) / guid;
    std::string error;
    if (!CompileShaderAsset(absolutePath, DisplayName(absolutePath, projectPath), cachePath.string(), error))
    {
        std::error_code ec;
        fs::remove(cachePath, ec);
        // One entry, the compiler's whole message: its lines (the source
        // line, the caret, a note) belong together.
        DEKI_LOG_ERROR("Shader: %s", error.c_str());
        return;
    }
    if (auto* assets = Deki::AssetManager::Get())
    {
        assets->RegisterGuid(guid, guid);
    }
}

/// A build's version of a .shader: compiled, where the target has OpenGL.
bool EncodeShaderForTarget(const AssetExportContext& ctx)
{
    std::string error;
    const std::string name = ctx.relativePath;
    if (!ctx.target.platformId.empty())
    {
        const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(ctx.target.platformId);
        if (!platform || platform->Capabilities().count("opengl") == 0)
        {
            const std::string board =
                platform && !platform->displayName.empty() ? platform->displayName : ctx.target.platformId;
            error = name + " is a GPU shader, and " + board +
                    " has no OpenGL to draw it with. Remove it from the scenes this build ships, or build for a "
                    "platform with OpenGL.";
        }
    }
    if (error.empty() && CompileShaderAsset(ctx.absolutePath, name, ctx.outPath, error))
    {
        return true;
    }
    if (ctx.error)
    {
        *ctx.error = error;
    }
    return false;
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
        AssetPipeline::OnStarted(
            [](AssetPipeline* pipeline)
            {
                pipeline->RegisterSyncHandler(".shader", HandleShaderSync);
                pipeline->RegisterExportEncoder(".shader", EncodeShaderForTarget);
            });
        AssetPipeline::OnImportComplete([](AssetPipeline* pipeline)
                                        { RegisterCachedShaders(pipeline, pipeline->GetProjectPath()); });
    }
};
ShaderAssetRegistrar s_ShaderAssetRegistrar;

}  // namespace
}  // namespace DekiEditor

#endif  // DEKI_EDITOR
