#ifndef BOARD_WAVESHARE_185C_DLNA_CONTROLLER_H_
#define BOARD_WAVESHARE_185C_DLNA_CONTROLLER_H_

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace waveshare185c {

struct DlnaDevice {
    std::string udn;          // 唯一标识 (uuid:...)
    std::string name;         // 友好名称，例如 "客厅的小米电视" 或 "小爱音箱 Pro"
    std::string location;     // description.xml 的完整 URL
    std::string control_url;  // AVTransport 控制端点完整 URL
};

class DlnaController {
public:
    using DeviceListCallback = std::function<void(const std::vector<DlnaDevice>&)>;

    static DlnaController& GetInstance();

    // 启动 SSDP 局域网设备扫描（后台异步任务）
    void StartDiscovery(DeviceListCallback on_update = nullptr);

    // 获取当前发现的所有有效 DLNA 渲染器
    std::vector<DlnaDevice> GetDevices() const;

    // 设置/获取当前选中的播放设备（-1 表示本机喇叭，>=0 表示 devices 索引）
    void SetTargetIndex(int index);
    int GetTargetIndex() const;
    std::string GetTargetName() const;

    // 对指定设备或当前选定设备进行控制
    bool Play(int device_idx, const std::string& media_url, const std::string& title = "",
              const std::string& artist = "");
    bool PlayCurrent(const std::string& media_url, const std::string& title = "",
                     const std::string& artist = "");
    bool Pause(int device_idx = -1);
    bool Stop(int device_idx = -1);

    bool IsScanning() const { return is_scanning_; }

private:
    DlnaController() = default;
    ~DlnaController() = default;
    DlnaController(const DlnaController&) = delete;
    DlnaController& operator=(const DlnaController&) = delete;

    // 解析 description.xml，提取 friendlyName 和 AVTransport controlURL
    static bool ParseDeviceXml(const std::string& xml, const std::string& base_url,
                               std::string& out_name, std::string& out_control_url,
                               std::string& out_udn);

    // 执行 SOAP HTTP POST 请求
    static bool SendSoapAction(const std::string& control_url, const std::string& action,
                               const std::string& soap_body);

    static std::string EscapeXml(const std::string& input);

    mutable std::mutex mutex_;
    std::vector<DlnaDevice> devices_;
    int target_index_ = -1;  // 默认 -1: 本地扬声器播放
    bool is_scanning_ = false;
    DeviceListCallback on_update_cb_ = nullptr;
};

}  // namespace waveshare185c

#endif  // BOARD_WAVESHARE_185C_DLNA_CONTROLLER_H_
