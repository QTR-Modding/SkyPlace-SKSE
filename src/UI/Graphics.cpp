#include "Graphics.h"

#include <algorithm>
#include <array>
#include <mutex>

#include "SkyPlaceCursorMenu.h"
#include "SkyrimMenuFontData.h"
#include "Texture.h"

namespace {
    constexpr float referenceHeight = 1080.0f;
    constexpr float referenceFontSize = 40.0f;
    constexpr ImWchar fontAwesomeGlyphRanges[] = {0xe005, 0xf8ff, 0};
    constexpr std::array fontAwesomePaths{
        "Data/SKSE/Plugins/SkyPlaceAssets/fa-solid-900.ttf",
        "Data/SKSE/Plugins/SkyPlaceAssets/fa-regular-400.ttf",
        "Data/SKSE/Plugins/SkyPlaceAssets/fa-brands-400.ttf"};

    ImFont* AddSkyrimMenuFont(
        ImGuiIO& io,
        float fontSize,
        ImFontConfig& fontConfig) {
        fontConfig.FontDataOwnedByAtlas = false;
        return io.Fonts->AddFontFromMemoryTTF(
            SkyrimMenuFontData::bytes,
            SkyrimMenuFontData::size,
            fontSize,
            &fontConfig);
    }

    ImFont* LoadFont(ImGuiIO& io, float fontSize, bool mergeFontAwesome) {
        ImFontConfig fontConfig{};
        fontConfig.PixelSnapH = true;
        ImFont* font = AddSkyrimMenuFont(io, fontSize, fontConfig);
        if (!font) {
            logger::warn(
                "Could not load the embedded Skyrim menu font at {} px; using the ImGui default font",
                fontSize);
            font = io.Fonts->AddFontDefault();
        }

        if (!mergeFontAwesome) {
            return font;
        }

        ImFontConfig mergeConfig{};
        mergeConfig.MergeMode = true;
        mergeConfig.PixelSnapH = true;
        for (const char* path : fontAwesomePaths) {
            if (!io.Fonts->AddFontFromFileTTF(
                    path,
                    fontSize,
                    &mergeConfig,
                    fontAwesomeGlyphRanges)) {
                logger::warn("Could not merge Font Awesome font '{}'", path);
            }
        }
        return font;
    }

    ImFont* LoadResponsiveFont(ImGuiIO& io) {
        const RE::BSGraphics::ScreenSize screenSize =
            RE::BSGraphics::Renderer::GetScreenSize();
        const float resolutionScale = screenSize.height > 0 ?
            std::max(
                static_cast<float>(screenSize.height) / referenceHeight,
                0.01f) :
            1.0f;
        const float fontSize = referenceFontSize * resolutionScale;

        logger::info(
            "Loading responsive Font Awesome UI font at {} px for {}x{}",
            fontSize,
            screenSize.width,
            screenSize.height);
        return LoadFont(io, fontSize, true);
    }
}

void Graphics::Install() {
    DrawHook::Install();
    CreateD3DAndSwapChain::Install();
}

LRESULT Graphics::WndProc::thunk(const HWND hWnd, const UINT uMsg, const WPARAM wParam, const LPARAM lParam) {
    auto& io = ImGui::GetIO();
    if (uMsg == WM_KILLFOCUS) {
        io.ClearInputKeys();
    }
    return func(hWnd, uMsg, wParam, lParam);
}


void Graphics::CreateD3DAndSwapChain::thunk() {
    func();

    if (const auto renderer = RE::BSGraphics::Renderer::GetSingleton()) {
        auto data = renderer->GetRuntimeData();
        swapChain = reinterpret_cast<IDXGISwapChain*>(data.renderWindows[0].swapChain);
        if (!swapChain) {
            logger::error("couldn't find swapChain");
            return;
        }

        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(swapChain->GetDesc(std::addressof(desc)))) {
            logger::error("IDXGISwapChain::GetDesc failed.");
            return;
        }

        device = reinterpret_cast<ID3D11Device*>(data.forwarder);
        context = reinterpret_cast<ID3D11DeviceContext*>(data.context);

        TextureManager::Init(device, context);

        logger::info("Initializing ImGui...");

        ImGui::CreateContext();

        auto& io = ImGui::GetIO();
        io.ConfigFlags = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        io.IniFilename = nullptr;
        io.MouseDrawCursor = false;

        io.FontDefault = LoadFont(io, referenceFontSize, false);
        transformMenuFont = LoadResponsiveFont(io);

        if (!ImGui_ImplWin32_Init(desc.OutputWindow)) {
            logger::error("ImGui initialization failed (Win32)");
            return;
        }
        if (!ImGui_ImplDX11_Init(device, context)) {
            logger::error("ImGui initialization failed (DX11)");
            return;
        }

        logger::info("ImGui initialized.");

        WndProc::func = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(desc.OutputWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc::thunk)));
        if (!WndProc::func) {
            logger::error("SetWindowLongPtrA failed!");
        }
    }
}

void Graphics::CreateD3DAndSwapChain::Install() {
    SKSE::AllocTrampoline(14);
    auto& trampoline = SKSE::GetTrampoline();
    const REL::Relocation<std::uintptr_t> target{REL::RelocationID(75595, 77226)};  // BSGraphics::InitD3D
    CreateD3DAndSwapChain::func =
        trampoline.write_call<5>(target.address() + REL::Relocate(0x9, 0x275), CreateD3DAndSwapChain::thunk);
}

void Graphics::DrawHook::thunk(std::uint32_t a_timer) {
    func(a_timer);
    RunQueuedTasks();

    RE::UI* ui = RE::UI::GetSingleton();
    if (ui && ui->IsMenuOpen(SkyPlaceCursorMenu::MENU_NAME)) {
        return;
    }

    Render();
}

void Graphics::Queue(std::function<void()> task) {
    std::lock_guard lock(queuedTasksMutex);
    queuedTasks.push_back(std::move(task));
}

void Graphics::RunQueuedTasks() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard lock(queuedTasksMutex);
        tasks.swap(queuedTasks);
    }

    for (std::function<void()>& task : tasks) {
        task();
    }
}

void Graphics::Render() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    {
        // trick imgui into rendering at game's real resolution (ie. if upscaled with Display Tweaks)
        static const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();

        auto& io = ImGui::GetIO();
        io.DisplaySize.x = static_cast<float>(screenSize.width);
        io.DisplaySize.y = static_cast<float>(screenSize.height);

        RE::MenuCursor* cursor = RE::MenuCursor::GetSingleton();
        if (cursor) {
            io.AddMousePosEvent(
                cursor->GetRuntimeData().cursorPosX,
                cursor->GetRuntimeData().cursorPosY);
        }
    }
    ImGui::NewFrame();



    for (auto drawFunction : drawFunctions) {
        drawFunction();
    }


    ImGui::EndFrame();
    ImGui::Render();

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void Graphics::DrawHook::Install() {
    SKSE::AllocTrampoline(14);
    auto& trampoline = SKSE::GetTrampoline();
	const REL::Relocation<std::uintptr_t> target2{REL::RelocationID(75461, 77246)};  // BSGraphics::Renderer::End
    DrawHook::func = trampoline.write_call<5>(target2.address() + 0x9, DrawHook::thunk);
}


float Graphics::GetResolutionScale() {
    static auto size = RE::BSGraphics::Renderer::GetScreenSize();
    return /* DisplayTweaks::borderlessUpscale ? DisplayTweaks::resolutionScale
                                                 :*/
           static_cast<float>(size.height) /
           static_cast<float>(size.width);
}

void Graphics::Register(std::function<void()> drawFunction) {
    drawFunctions.push_back(drawFunction);
}

ImFont* Graphics::GetTransformMenuFont() {
    return transformMenuFont;
}

ImVec2 WorldToScreenLoc(RE::NiPoint3 position) {
    static uintptr_t g_worldToCamMatrix = RELOCATION_ID(519579, 406126).address();         // 2F4C910, 2FE75F0
    static auto g_viewPort = (RE::NiRect<float>*)RELOCATION_ID(519618, 406160).address();  // 2F4DED0, 2FE8B98

    ImVec2 screenLocOut;
    const RE::NiPoint3 niWorldLoc(position.x, position.y, position.z);

    float zVal;

    RE::NiCamera::WorldPtToScreenPt3((float(*)[4])g_worldToCamMatrix, *g_viewPort, niWorldLoc, screenLocOut.x,
                                     screenLocOut.y, zVal, 1e-5f);
    ImVec2 rect = ImGui::GetIO().DisplaySize;

    screenLocOut.x = rect.x * screenLocOut.x;
    screenLocOut.y = 1.0f - screenLocOut.y;
    screenLocOut.y = rect.y * screenLocOut.y;

    return screenLocOut;
}
