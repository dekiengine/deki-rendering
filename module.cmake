# Module descriptor for deki-engine auto-discovery
set(MODULE_DISPLAY_NAME "Rendering")
set(MODULE_PREFIX "DekiRendering")
set(MODULE_UPPER "RENDERING")
set(MODULE_TARGET "deki-rendering")
set(MODULE_FILE_PREFIX "Rendering")
set(MODULE_SOURCES
    RendererComponent.cpp
    CameraComponent.cpp
    Standard2DRenderer.cpp
    QuadBlit.cpp
    DekiRenderSystem.cpp
    DekiRenderingInit.cpp
    DekiRendererRegistry.cpp
    DekiRenderPassRegistry.cpp
    DekiSortingCallbackRegistry.cpp
)
set(MODULE_HAS_SYSTEM_INIT ON)
set(MODULE_EDITOR_SOURCES
    editor/RenderPipelineEditor.cpp
)
set(MODULE_NEEDS_IMGUI ON)
set(MODULE_LINK_DEPS deki-editor)
set(MODULE_ENTRY DekiRenderingModule.cpp)
