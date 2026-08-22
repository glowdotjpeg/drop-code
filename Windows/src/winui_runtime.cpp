#include "winui_runtime.h"

#include <MddBootstrap.h>
#include <WindowsAppSDK-VersionInfo.h>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <memory>
#include <functional>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

namespace dc::winui {
namespace {

using winrt::Microsoft::UI::Xaml::Application;
using winrt::Microsoft::UI::Xaml::ApplicationT;
using winrt::Microsoft::UI::Xaml::Controls::XamlControlsResources;
using winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs;
using winrt::Microsoft::UI::Xaml::Markup::IXamlMetadataProvider;
using winrt::Microsoft::UI::Xaml::Markup::IXamlType;
using winrt::Microsoft::UI::Xaml::Markup::XmlnsDefinition;
using winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider;
using winrt::Windows::UI::Xaml::Interop::TypeName;

std::function<int()> g_launchCallback;
int g_launchResult = 0;

struct DropCodeApplication : ApplicationT<DropCodeApplication,
                                          IXamlMetadataProvider> {
    DropCodeApplication() = default;

    void OnLaunched(const LaunchActivatedEventArgs&) {
        Resources().MergedDictionaries().Append(XamlControlsResources());

        if (g_launchCallback) {
            g_launchResult = g_launchCallback();
            g_launchCallback = {};
            Exit();
        }
    }

    IXamlType GetXamlType(TypeName const& type) {
        return provider.GetXamlType(type);
    }

    IXamlType GetXamlType(winrt::hstring const& fullName) {
        return provider.GetXamlType(fullName);
    }

    winrt::com_array<XmlnsDefinition> GetXmlnsDefinitions() {
        return provider.GetXmlnsDefinitions();
    }

private:
    XamlControlsXamlMetaDataProvider provider;
};

struct XamlRuntimeState {
    Application application{nullptr};
};

std::unique_ptr<XamlRuntimeState> g_xamlRuntime;
bool g_bootstrapped = false;

}

HRESULT InitializeWindowsAppSdk() {
    if (g_bootstrapped) return S_OK;

    PACKAGE_VERSION minimumVersion{};
    minimumVersion.Major = WINDOWSAPPSDK_RUNTIME_VERSION_MAJOR;
    minimumVersion.Minor = WINDOWSAPPSDK_RUNTIME_VERSION_MINOR;
    minimumVersion.Build = WINDOWSAPPSDK_RUNTIME_VERSION_BUILD;
    minimumVersion.Revision = WINDOWSAPPSDK_RUNTIME_VERSION_REVISION;

    const HRESULT result = MddBootstrapInitialize2(
        WINDOWSAPPSDK_RELEASE_MAJORMINOR,
        WINDOWSAPPSDK_RELEASE_VERSION_TAG_W, minimumVersion,
        MddBootstrapInitializeOptions_None);
    if (SUCCEEDED(result)) g_bootstrapped = true;
    return result;
}

void ShutdownWindowsAppSdk() {
    if (!g_bootstrapped) return;
    MddBootstrapShutdown();
    g_bootstrapped = false;
}

int RunXamlApplication(std::function<int()> launchCallback) {
    if (g_xamlRuntime || !launchCallback) return 1;

    g_xamlRuntime = std::make_unique<XamlRuntimeState>();
    g_launchResult = 0;
    g_launchCallback = std::move(launchCallback);

    try {
        Application::Start([](auto&&) {
            auto application = winrt::make<DropCodeApplication>();
            g_xamlRuntime->application = application;
        });
    } catch (...) {
        g_launchCallback = {};
        ShutdownXaml();
        throw;
    }

    g_launchCallback = {};
    const int result = g_launchResult;
    ShutdownXaml();
    return result;
}

void ShutdownXaml() {
    if (g_xamlRuntime) {
        g_xamlRuntime->application = nullptr;
        g_xamlRuntime.reset();
    }
}

}
