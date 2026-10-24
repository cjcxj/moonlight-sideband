/**
 * AudioModule 实现
 *
 * 依赖的 Windows 接口：
 *   - IMMDeviceEnumerator / IMMDevice / IPropertyStore   (mmdeviceapi.h，已文档化)
 *   - IAudioEndpointVolume                               (endpointvolume.h，已文档化)
 *   - IPolicyConfig                                      (未文档化，见下方说明)
 *
 * 关于 IPolicyConfig：Windows 自己的"选择播放声音的位置"面板就是用它改默认设备的，
 * 系统没有提供已文档化的替代 API。它的 IID 与 vtable 布局由社区从 Nt 层头文件里
 * 还原出来（frgnca/AudioDeviceCmdlets、SoundSwitch 等都在用），这里按那份布局
 * 原样声明。**vtable 顺序错了不会报错，只会调到别的方法上**，所以：
 *   1. 方法顺序严格照抄权威定义，一个不多一个不少；
 *   2. 调用完一定回查 GetDefaultAudioEndpoint 比对端点 ID，确认真的生效才报成功。
 */

#include "AudioModule.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include "Logger.hpp"
#include "Json.hpp"
#include "Win32Util.hpp"
#include "SidebandProtocol.hpp"

#include <sstream>
#include <chrono>
#include <cmath>

namespace
{

using Win32Util::WideToUtf8;
using Win32Util::Utf8ToWide;

// ============================================================
//  COM 小工具
// ============================================================

// 线程内 COM 初始化守卫。
// 服务器主循环线程与监控线程都不是 COM 线程，每次操作前确保初始化。
// RPC_E_CHANGED_MODE（该线程已按 STA 初始化过）时**不能**再 CoUninitialize，
// 否则会破坏别人的引用计数，所以只在 SUCCEEDED 时才配对释放。
class ComScope
{
public:
    ComScope()
        : m_uninit(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
    }
    ~ComScope()
    {
        if (m_uninit)
            CoUninitialize();
    }
    ComScope(const ComScope &) = delete;
    ComScope &operator=(const ComScope &) = delete;

private:
    bool m_uninit;
};

// 极简 COM 指针持有者：只管 Release，不带异常、不依赖 WIL/wrl
template <typename T>
class ComHandle
{
public:
    // 默认构造必须显式写：只靠成员初始化器时 MSVC 在实例化处报 C2512
    //（"no appropriate default constructor available"）。
    ComHandle() : m_p(nullptr) {}
    ~ComHandle() { if (m_p) m_p->Release(); }
    ComHandle(const ComHandle &) = delete;
    ComHandle &operator=(const ComHandle &) = delete;

    T **ReleaseRef() { return &m_p; }   // 交给 CoCreateInstance / Activate 等填充
    T *operator->() const { return m_p; }
    T *get() const { return m_p; }

private:
    T *m_p;
};

// ============================================================
//  IPolicyConfig（未文档化）
// ============================================================

// {f8679f50-850a-41cf-9c72-430f290290c8}
// vtable 顺序 = IUnknown 之后 12 个槽，SetDefaultEndpoint 是第 11 个。
// 前 10 个本项目一个都不调用，但**必须**按序占位，否则调用会落到错误的方法上。
// 占位参数一律写成 void *：vtable 只关心槽位数量与顺序，参数类型无关；
// 这样也不必依赖 WAVEFORMATEX / DeviceShareMode 这些受 WIN32_LEAN_AND_MEAN 影响的声明。
//
// 这里不用 SDK 的 MIDL_INTERFACE 宏而直接写 __declspec(uuid(...))：那个宏定义在
// rpcndr.h 里，而 WIN32_LEAN_AND_MEAN 会跳过 rpc/ole2 这一串头文件，展开与否
// 取决于本项目当前恰好 include 了什么，不可靠。__declspec(uuid) 是 MSVC 原生语法，
// IID_PPV_ARGS 里的 __uuidof 照样能解析到下面这个 IID。
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8"))
IPolicyConfig : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(LPCWSTR, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(LPCWSTR, BOOL, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(LPCWSTR, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(LPCWSTR, BOOL, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(LPCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(LPCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(LPCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(LPCWSTR, BOOL, const PROPERTYKEY *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(LPCWSTR, BOOL, const PROPERTYKEY *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(LPCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(LPCWSTR, BOOL) = 0;
};

// {870af99c-171d-4f9e-af0d-e63df40c2bc9}
const CLSID kCLSIDPolicyConfigClient =
    { 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };

// {568b9108-44bf-40b4-9006-86afe5b5a620} —— Vista 时代的变体，vtable 与上面完全一致，
// 只是 IID 不同。新系统上第一个 IID 就能拿到，这里只作兜底。
const IID kIIDPolicyConfigVista =
    { 0x568b9108, 0x44bf, 0x40b4, { 0x90, 0x06, 0x86, 0xaf, 0xe5, 0xb5, 0xa6, 0x20 } };

// 拿到 IPolicyConfig。优先用 Win7+ 的 IID，失败再退到 Vista IID。
HRESULT CreatePolicyConfig(IPolicyConfig **out)
{
    *out = nullptr;

    HRESULT hr = CoCreateInstance(kCLSIDPolicyConfigClient, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(out));
    if (SUCCEEDED(hr))
        return hr;

    // 老构建只认 Vista IID。必须显式按该 IID 取接口（取 IUnknown 拿到的是
    // *默认* 接口的 vtable，两者不是一回事）。两者方法顺序完全一致，
    // 所以拿到后直接按 IPolicyConfig 布局使用，所有权交给调用方释放。
    void *raw = nullptr;
    hr = CoCreateInstance(kCLSIDPolicyConfigClient, nullptr, CLSCTX_ALL,
                          kIIDPolicyConfigVista, &raw);
    if (FAILED(hr) || !raw)
        return FAILED(hr) ? hr : E_FAIL;

    *out = reinterpret_cast<IPolicyConfig *>(raw);
    return S_OK;
}

// PKEY_Device_FriendlyName = {A45C254E-DF1C-4FFD-8020-67D146A850E0}, 14
// 本地定义而不是 include <functiondiscoverykeys_devpkey.h>：那个头文件在未
// #define INITGUID 时只给 extern 声明，得额外链 propsys.lib。
const PROPERTYKEY kPKEYFriendlyName =
    { { 0xa45c254e, 0xdf1c, 0x4ffd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

// 取设备友好名称。失败时回退到端点 ID（面板里至少能看到点东西）。
std::string GetFriendlyName(IMMDevice *device, const std::string &fallback)
{
    ComHandle<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.ReleaseRef())) || !store.get())
        return fallback;

    PROPVARIANT var = {};
    PropVariantInit(&var);
    std::string out = fallback;
    if (SUCCEEDED(store->GetValue(kPKEYFriendlyName, &var)) && var.vt == VT_LPWSTR && var.pwszVal)
    {
        std::string utf8 = WideToUtf8(var.pwszVal);
        if (!utf8.empty())
            out = utf8;
    }
    PropVariantClear(&var);
    return out;
}

// 端点 ID -> UTF-8 std::string（调用方负责 CoTaskMemFree）
std::string EndpointId(IMMDevice *device)
{
    LPWSTR raw = nullptr;
    if (FAILED(device->GetId(&raw)) || !raw)
        return std::string();
    std::string utf8 = WideToUtf8(raw);
    CoTaskMemFree(raw);
    return utf8;
}

// 标量音量 (0.0-1.0) -> 面板上的 0-100
int ScalarToPercent(float scalar)
{
    if (scalar <= 0.f)
        return 0;
    if (scalar >= 1.f)
        return 100;
    return (int)std::lround(scalar * 100.f);
}

const char *StateName(DWORD state)
{
    if (state & DEVICE_STATE_DISABLED)
        return "disabled";
    if (state & DEVICE_STATE_NOTPRESENT)
        return "not_present";
    if (state & DEVICE_STATE_UNPLUGGED)
        return "unplugged";
    return "active";
}

// 读某角色的默认端点 ID（失败返回空串）
std::string DefaultEndpointId(IMMDeviceEnumerator *enumerator, ERole role)
{
    ComHandle<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, role, device.ReleaseRef())) || !device.get())
        return std::string();
    return EndpointId(device.get());
}

} // namespace

// ============================================================
//                      生命周期与路由
// ============================================================

AudioModule::AudioModule(SidebandServer &server) : m_server(server) {}

AudioModule::~AudioModule()
{
    Stop();
}

bool AudioModule::Start()
{
    if (m_monitorThread.joinable())
        return true;  // 已启动

    ComScope com;
    ComHandle<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(enumerator.ReleaseRef()));
    if (FAILED(hr) || !enumerator.get())
    {
        Logger::Get().Error("AudioModule: 无法创建音频设备枚举器 hr=0x",
                            std::hex, (unsigned long)hr, std::dec,
                            "，音频控制不可用");
        return false;  // 单模块不可用不影响其他模块
    }

    m_exit = false;
    m_monitorThread = std::thread([this] { MonitorLoop(); });

    Logger::Get().Info("AudioModule: 已启动 (含输出设备枚举与默认设备切换)");
    return true;
}

void AudioModule::Stop()
{
    m_exit = true;
    {
        std::lock_guard<std::mutex> l(m_wakeMutex);
        m_wakeRequested = true;
    }
    m_wakeCv.notify_all();

    if (m_monitorThread.joinable())
        m_monitorThread.join();

    Logger::Get().Info("AudioModule: 已停止");
}

bool AudioModule::HandlesCommand(uint32_t cmd_id) const
{
    using namespace SidebandProtocol;
    switch (cmd_id)
    {
    case Cmd::AUDIO_LIST_REQ:
    case Cmd::AUDIO_DEFAULT_SET:
    case Cmd::AUDIO_VOLUME_SET:
    case Cmd::AUDIO_MUTE_SET:
        return true;
    default:
        return false;
    }
}

bool AudioModule::CommandRequiresAuth(uint32_t cmd_id) const
{
    using namespace SidebandProtocol;
    // 只读的列表查询无需认证；改默认设备/音量/静音会动系统状态，需要令牌
    switch (cmd_id)
    {
    case Cmd::AUDIO_DEFAULT_SET:
    case Cmd::AUDIO_VOLUME_SET:
    case Cmd::AUDIO_MUTE_SET:
        return true;
    default:
        return false;
    }
}

uint32_t AudioModule::ResponseCommandFor(uint32_t cmd_id) const
{
    using namespace SidebandProtocol;
    switch (cmd_id)
    {
    case Cmd::AUDIO_LIST_REQ:     return Cmd::AUDIO_LIST_RESP;
    case Cmd::AUDIO_DEFAULT_SET:  return Cmd::AUDIO_OP_RESP;
    case Cmd::AUDIO_VOLUME_SET:   return Cmd::AUDIO_OP_RESP;
    case Cmd::AUDIO_MUTE_SET:     return Cmd::AUDIO_OP_RESP;
    default:                      return 0;
    }
}

void AudioModule::OnClientConnected(SidebandSession &)
{
    RequestPush();
    Logger::Get().Debug("AudioModule: 新客户端连接，已请求推送当前输出设备状态");
}

void AudioModule::RequestPush()
{
    m_forcePush.store(true);
    {
        std::lock_guard<std::mutex> l(m_wakeMutex);
        m_wakeRequested = true;
    }
    m_wakeCv.notify_one();
}

// ============================================================
//                      枚举
// ============================================================

std::vector<AudioModule::DeviceInfo> AudioModule::EnumerateDevices() const
{
    std::lock_guard<std::mutex> lock(m_audioMutex);

    std::vector<DeviceInfo> result;

    ComScope com;
    ComHandle<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(enumerator.ReleaseRef()));
    if (FAILED(hr) || !enumerator.get())
    {
        Logger::Get().Warning("AudioModule: 创建枚举器失败 hr=0x",
                              std::hex, (unsigned long)hr, std::dec);
        return result;
    }

    const std::string defaultId = DefaultEndpointId(enumerator.get(), eConsole);
    const std::string defaultCommId = DefaultEndpointId(enumerator.get(), eCommunications);

    ComHandle<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.ReleaseRef());
    if (FAILED(hr) || !collection.get())
    {
        Logger::Get().Warning("AudioModule: 枚举输出设备失败 hr=0x",
                              std::hex, (unsigned long)hr, std::dec);
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i)
    {
        ComHandle<IMMDevice> device;
        if (FAILED(collection->Item(i, device.ReleaseRef())) || !device.get())
            continue;

        DeviceInfo info;
        info.id = EndpointId(device.get());
        if (info.id.empty())
            continue;

        info.name = GetFriendlyName(device.get(), info.id);

        DWORD state = 0;
        if (SUCCEEDED(device->GetState(&state)))
            info.state = StateName(state);

        ComHandle<IAudioEndpointVolume> volume;
        // Activate 的签名是 (REFIID, DWORD, PROPVARIANT*, void**) —— IID 已经在第一个
        // 参数上给了，再用 IID_PPV_ARGS 会多出第二个 IID，变成 5 个参数（C2660）。
        if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER,
                                       nullptr,
                                       reinterpret_cast<void **>(volume.ReleaseRef()))) && volume.get())
        {
            float scalar = 0.f;
            BOOL muted = FALSE;
            if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&scalar)))
                info.volume = ScalarToPercent(scalar);
            if (SUCCEEDED(volume->GetMute(&muted)))
                info.muted = (muted != FALSE);
        }

        info.isDefault = (!defaultId.empty() && defaultId == info.id);
        info.isDefaultComm = (!defaultCommId.empty() && defaultCommId == info.id);

        result.push_back(std::move(info));
    }

    Logger::Get().Debug("AudioModule: 枚举输出设备 count=", result.size(),
                        " 默认=", defaultId);
    for (const auto &d : result)
        Logger::Get().Debug("AudioModule:   ", d.id, " | ", d.name,
                            " vol=", d.volume, (d.muted ? " muted" : ""),
                            (d.isDefault ? " [default]" : ""));

    return result;
}

// ============================================================
//                      写操作
// ============================================================

AudioModule::OpResult AudioModule::SetDefaultDevice(const std::string &deviceId)
{
    std::lock_guard<std::mutex> lock(m_audioMutex);

    ComScope com;
    const std::wstring wideId = Utf8ToWide(deviceId);

    // 先确认端点存在，否则 SetDefaultEndpoint 会返回一个含义模糊的错误码
    ComHandle<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(enumerator.ReleaseRef()))) || !enumerator.get())
        return OpResult::ApiFailed;

    ComHandle<IMMDevice> device;
    HRESULT hr = enumerator->GetDevice(wideId.c_str(), device.ReleaseRef());
    if (FAILED(hr) || !device.get())
    {
        Logger::Get().Warning("AudioModule: GetDevice 失败 hr=0x",
                              std::hex, (unsigned long)hr, std::dec, " id=", deviceId);
        return OpResult::NotFound;
    }

    ComHandle<IPolicyConfig> policy;
    hr = CreatePolicyConfig(policy.ReleaseRef());
    if (FAILED(hr) || !policy.get())
    {
        Logger::Get().Error("AudioModule: 取不到 IPolicyConfig hr=0x",
                            std::hex, (unsigned long)hr, std::dec);
        return OpResult::ApiFailed;
    }

    // eConsole + eMultimedia 都设：串流抓的是默认播放设备，只设一个角色会出现
    // "面板上切了但声音没跟着走"。eCommunications 故意不动 ——
    // Windows 设置面板也不会因为切输出而把通讯设备一起搬走。
    for (ERole role : { eConsole, eMultimedia })
    {
        hr = policy->SetDefaultEndpoint(wideId.c_str(), role);
        if (FAILED(hr))
        {
            Logger::Get().Error("AudioModule: SetDefaultEndpoint 失败 hr=0x",
                                std::hex, (unsigned long)hr, std::dec,
                                " role=", (int)role, " id=", deviceId);
            return OpResult::ApiFailed;
        }
    }

    // 回查验证：上面那两个调用"返回成功"不等于真换成了（vtable 若对不上、
    // 或驱动拒绝切换，都会静默无效）。以系统实际报出的默认设备为准。
    const std::string nowDefault = DefaultEndpointId(enumerator.get(), eConsole);
    if (nowDefault != deviceId)
    {
        Logger::Get().Error("AudioModule: 切换后回查到的默认设备不是目标设备， 期望=", deviceId,
                            " 实际=", nowDefault);
        return OpResult::NotApplied;
    }

    Logger::Get().Info("AudioModule: 默认输出设备已切换到 ", deviceId,
                       " (", GetFriendlyName(device.get(), deviceId), ")");
    return OpResult::Ok;
}

AudioModule::OpResult AudioModule::SetVolume(const std::string &deviceId, int volume)
{
    std::lock_guard<std::mutex> lock(m_audioMutex);

    ComScope com;
    const std::wstring wideId = Utf8ToWide(deviceId);

    ComHandle<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(enumerator.ReleaseRef()))) || !enumerator.get())
        return OpResult::ApiFailed;

    ComHandle<IMMDevice> device;
    HRESULT hr = enumerator->GetDevice(wideId.c_str(), device.ReleaseRef());
    if (FAILED(hr) || !device.get())
        return OpResult::NotFound;

    ComHandle<IAudioEndpointVolume> endpointVolume;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER,
                          nullptr,
                          reinterpret_cast<void **>(endpointVolume.ReleaseRef()));
    if (FAILED(hr) || !endpointVolume.get())
    {
        Logger::Get().Warning("AudioModule: 该设备不支持音量接口 hr=0x",
                              std::hex, (unsigned long)hr, std::dec, " id=", deviceId);
        return OpResult::ApiFailed;
    }

    hr = endpointVolume->SetMasterVolumeLevelScalar(volume / 100.f, nullptr);
    if (FAILED(hr))
    {
        Logger::Get().Error("AudioModule: 设置音量失败 hr=0x",
                            std::hex, (unsigned long)hr, std::dec,
                            " vol=", volume, " id=", deviceId);
        return OpResult::ApiFailed;
    }

    Logger::Get().Info("AudioModule: 音量已设为 ", volume, "% (", deviceId, ")");
    return OpResult::Ok;
}

AudioModule::OpResult AudioModule::SetMuted(const std::string &deviceId, bool muted)
{
    std::lock_guard<std::mutex> lock(m_audioMutex);

    ComScope com;
    const std::wstring wideId = Utf8ToWide(deviceId);

    ComHandle<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(enumerator.ReleaseRef()))) || !enumerator.get())
        return OpResult::ApiFailed;

    ComHandle<IMMDevice> device;
    HRESULT hr = enumerator->GetDevice(wideId.c_str(), device.ReleaseRef());
    if (FAILED(hr) || !device.get())
        return OpResult::NotFound;

    ComHandle<IAudioEndpointVolume> endpointVolume;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER,
                          nullptr,
                          reinterpret_cast<void **>(endpointVolume.ReleaseRef()));
    if (FAILED(hr) || !endpointVolume.get())
        return OpResult::ApiFailed;

    hr = endpointVolume->SetMute(muted ? TRUE : FALSE, nullptr);
    if (FAILED(hr))
    {
        Logger::Get().Error("AudioModule: 设置静音失败 hr=0x",
                            std::hex, (unsigned long)hr, std::dec, " id=", deviceId);
        return OpResult::ApiFailed;
    }

    Logger::Get().Info("AudioModule: ", deviceId, muted ? " 已静音" : " 已取消静音");
    return OpResult::Ok;
}

// ============================================================
//                      JSON
// ============================================================

std::string AudioModule::DevicesToJson(const std::vector<DeviceInfo> &devices) const
{
    std::ostringstream ss;
    ss << "{\"devices\":[";
    for (size_t i = 0; i < devices.size(); ++i)
    {
        const auto &d = devices[i];
        if (i > 0)
            ss << ",";
        ss << "{";
        ss << "\"id\":\"" << Json::Escape(d.id) << "\",";
        ss << "\"name\":\"" << Json::Escape(d.name) << "\",";
        ss << "\"volume\":" << d.volume << ",";
        ss << "\"muted\":" << (d.muted ? "true" : "false") << ",";
        ss << "\"is_default\":" << (d.isDefault ? "true" : "false") << ",";
        ss << "\"is_default_comm\":" << (d.isDefaultComm ? "true" : "false") << ",";
        ss << "\"state\":\"" << Json::Escape(d.state) << "\"";
        ss << "}";
    }
    ss << "]}";
    return ss.str();
}

uint64_t AudioModule::HashState(const std::vector<DeviceInfo> &devices) const
{
    uint64_t h = 1469598103934665603ull;  // FNV-1a 64 位偏移基数
    auto feed = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i)
        {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 1099511628211ull;
        }
    };

    feed(devices.size());
    for (const auto &d : devices)
    {
        for (unsigned char c : d.id)
            feed(c);
        feed((uint64_t)d.volume);
        feed(d.muted ? 1 : 0);
        feed(d.isDefault ? 2 : 0);
        feed(d.isDefaultComm ? 4 : 0);
    }
    return h;
}

std::string AudioModule::OpResultToJson(OpResult r, const std::string &deviceId) const
{
    const std::string id = Json::Escape(deviceId);
    switch (r)
    {
    case OpResult::Ok:
        return R"({"ok":true,"device_id":")" + id + "\"}";
    case OpResult::NotFound:
        return R"({"ok":false,"error":"not_found","device_id":")" + id + "\"}";
    case OpResult::InvalidPayload:
        return R"({"ok":false,"error":"invalid_payload","msg":"missing or out-of-range field"})";
    case OpResult::NotApplied:
        return R"({"ok":false,"error":"not_applied","device_id":")" + id + "\"}";
    case OpResult::ApiFailed:
    default:
        return R"({"ok":false,"error":"api_failed","device_id":")" + id + "\"}";
    }
}

// ============================================================
//                      指令处理
// ============================================================

void AudioModule::OnCommand(SidebandSession &session,
                            uint32_t cmd_id,
                            uint32_t req_id,
                            const uint8_t *payload,
                            uint32_t payload_len)
{
    using namespace SidebandProtocol;

    const std::string payloadStr(payload ? (const char *)payload : "",
                                 payload ? payload_len : 0);

    auto sendJson = [&session, req_id](uint32_t cmd, const std::string &json)
    {
        std::vector<uint8_t> p(json.begin(), json.end());
        session.SendCommand(cmd, req_id, p);
    };

    switch (cmd_id)
    {
    case Cmd::AUDIO_LIST_REQ:
    {
        auto devices = EnumerateDevices();
        sendJson(Cmd::AUDIO_LIST_RESP, DevicesToJson(devices));
        Logger::Get().Debug("AudioModule: 列出输出设备 (count=", devices.size(), ")");
        break;
    }

    case Cmd::AUDIO_DEFAULT_SET:
    {
        Logger::Get().Debug("AudioModule: AUDIO_DEFAULT_SET payload=", payloadStr);
        std::string deviceId = Json::GetString(payloadStr, "device_id");
        OpResult r = deviceId.empty() ? OpResult::InvalidPayload
                                      : SetDefaultDevice(deviceId);
        sendJson(Cmd::AUDIO_OP_RESP, OpResultToJson(r, deviceId));
        RequestPush();
        break;
    }

    case Cmd::AUDIO_VOLUME_SET:
    {
        Logger::Get().Debug("AudioModule: AUDIO_VOLUME_SET payload=", payloadStr);
        std::string deviceId = Json::GetString(payloadStr, "device_id");
        bool found = false;
        int volume = Json::GetInt(payloadStr, "volume", -1, &found);
        bool volumeOk = found && volume >= 0 && volume <= 100;

        OpResult r = (deviceId.empty() || !volumeOk) ? OpResult::InvalidPayload
                                                     : SetVolume(deviceId, volume);
        sendJson(Cmd::AUDIO_OP_RESP, OpResultToJson(r, deviceId));
        RequestPush();
        break;
    }

    case Cmd::AUDIO_MUTE_SET:
    {
        Logger::Get().Debug("AudioModule: AUDIO_MUTE_SET payload=", payloadStr);
        std::string deviceId = Json::GetString(payloadStr, "device_id");
        bool found = false;
        bool muted = Json::GetBool(payloadStr, "muted", false, &found);

        OpResult r = deviceId.empty() ? OpResult::InvalidPayload
                                      : (found ? SetMuted(deviceId, muted)
                                               : OpResult::InvalidPayload);
        sendJson(Cmd::AUDIO_OP_RESP, OpResultToJson(r, deviceId));
        RequestPush();
        break;
    }

    default:
        break;
    }
}

// ============================================================
//                      状态监控与推送
// ============================================================

void AudioModule::MonitorLoop()
{
    Logger::Get().Debug("AudioModule: 监控线程已启动");

    uint64_t lastHash = 0;
    bool hasLast = false;
    bool lastRoundFailed = false;

    // 异常日志节流：同一个故障最多 30 秒刷一条，别每 2 秒刷屏
    auto lastErrorLog = std::chrono::steady_clock::now() - std::chrono::seconds(30);

    while (!m_exit)
    {
        bool failedThisRound = false;
        try
        {
            bool force = m_forcePush.exchange(false);

            auto devices = EnumerateDevices();
            uint64_t hash = HashState(devices);

            // 枚举失败时 devices 为空，与"真的一个输出设备都没有"无法区分。
            // 空列表不推送，避免枚举器临时故障时把客户端的界面清空。
            if (devices.empty())
                failedThisRound = true;
            else if (force || !hasLast || hash != lastHash)
            {
                lastHash = hash;
                hasLast = true;

                if (m_server.HasClients())
                    PushState(devices, 0);
            }
        }
        catch (const std::exception &e)
        {
            failedThisRound = true;
            hasLast = false;
            auto now = std::chrono::steady_clock::now();
            if (now - lastErrorLog >= std::chrono::seconds(30))
            {
                lastErrorLog = now;
                Logger::Get().Error("AudioModule: 监控线程异常: ", e.what());
            }
        }
        catch (...)
        {
            failedThisRound = true;
            hasLast = false;
            auto now = std::chrono::steady_clock::now();
            if (now - lastErrorLog >= std::chrono::seconds(30))
            {
                lastErrorLog = now;
                Logger::Get().Error("AudioModule: 监控线程未知异常");
            }
        }

        lastRoundFailed = failedThisRound;

        // 正常 2 秒；COM 出错时放慢到 5 秒，原地重试而不是重建状态
        const auto pollInterval =
            lastRoundFailed ? std::chrono::seconds(5) : std::chrono::seconds(2);

        std::unique_lock<std::mutex> l(m_wakeMutex);
        m_wakeCv.wait_for(l, pollInterval,
                          [this] { return m_wakeRequested || m_exit; });
        m_wakeRequested = false;
    }

    Logger::Get().Debug("AudioModule: 监控线程退出");
}

void AudioModule::PushState(const std::vector<DeviceInfo> &devices, uint32_t req_id)
{
    std::string json = DevicesToJson(devices);
    std::vector<uint8_t> p(json.begin(), json.end());
    m_server.BroadcastCommand(SidebandProtocol::Cmd::AUDIO_STATE, req_id, p);

    for (const auto &d : devices)
    {
        if (!d.isDefault)
            continue;
        Logger::Get().Debug("AudioModule: 推送默认输出 ", d.id, " ", d.name,
                            " vol=", d.volume, (d.muted ? " muted" : ""));
        break;
    }
}
