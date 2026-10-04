#include "dlna_controller.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <sstream>

#include "application.h"
#include "board.h"

namespace waveshare185c {

static const char* TAG = "DlnaController";

namespace {

// 辅助：从字符串提取标签内容 <tag>content</tag>
std::string ExtractXmlTag(const std::string& xml, const std::string& tag) {
    std::string open_tag = "<" + tag + ">";
    std::string close_tag = "</" + tag + ">";

    size_t start = xml.find(open_tag);
    if (start == std::string::npos) {
        return "";
    }
    start += open_tag.length();
    size_t end = xml.find(close_tag, start);
    if (end == std::string::npos) {
        return "";
    }
    return xml.substr(start, end - start);
}

// 辅助：获取 URL 的 baseUrl (例如 http://192.168.1.100:49152)
std::string GetBaseUrl(const std::string& url) {
    size_t scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) {
        return "";
    }
    size_t slash_pos = url.find('/', scheme_pos + 3);
    if (slash_pos == std::string::npos) {
        return url;
    }
    return url.substr(0, slash_pos);
}

}  // namespace

DlnaController& DlnaController::GetInstance() {
    static DlnaController instance;
    return instance;
}

std::string DlnaController::EscapeXml(const std::string& input) {
    std::string out;
    out.reserve(input.size() * 1.2);
    for (char c : input) {
        switch (c) {
            case '&':
                out.append("&amp;");
                break;
            case '<':
                out.append("&lt;");
                break;
            case '>':
                out.append("&gt;");
                break;
            case '"':
                out.append("&quot;");
                break;
            case '\'':
                out.append("&apos;");
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

bool DlnaController::ParseDeviceXml(const std::string& xml, const std::string& base_url,
                                   std::string& out_name, std::string& out_control_url,
                                   std::string& out_udn) {
    out_name = ExtractXmlTag(xml, "friendlyName");
    out_udn = ExtractXmlTag(xml, "UDN");

    // 寻找包含 AVTransport:1 的 <service> 节点
    std::string target_service = "urn:schemas-upnp-org:service:AVTransport:1";
    size_t s_pos = xml.find(target_service);
    if (s_pos == std::string::npos) {
        return false;
    }

    // 从 s_pos 往前寻找最近的 <service>，往后寻找最近的 </service>
    size_t start_service = xml.rfind("<service>", s_pos);
    size_t end_service = xml.find("</service>", s_pos);
    if (start_service == std::string::npos || end_service == std::string::npos) {
        return false;
    }

    std::string service_block = xml.substr(start_service, end_service - start_service);
    std::string ctrl = ExtractXmlTag(service_block, "controlURL");
    if (ctrl.empty()) {
        return false;
    }

    if (ctrl.rfind("http://", 0) == 0 || ctrl.rfind("https://", 0) == 0) {
        out_control_url = ctrl;
    } else {
        if (!ctrl.empty() && ctrl.front() != '/') {
            out_control_url = base_url + "/" + ctrl;
        } else {
            out_control_url = base_url + ctrl;
        }
    }

    return !out_name.empty() && !out_control_url.empty();
}

bool DlnaController::ProbeAndAddDevice(const std::string& location_url) {
    if (location_url.empty()) return false;
    auto network = Board::GetInstance().GetNetwork();
    if (!network) return false;

    std::vector<std::string> urls_to_try;
    if (location_url.rfind("http://", 0) == 0 || location_url.rfind("https://", 0) == 0) {
        urls_to_try.push_back(location_url);
    } else {
        // 用户传入的是单纯 IP 或 IP:端口
        std::string host = location_url;
        if (host.find(':') != std::string::npos) {
            urls_to_try.push_back("http://" + host + "/description.xml");
            urls_to_try.push_back("http://" + host + "/devicedesc.xml");
            urls_to_try.push_back("http://" + host + "/rootDesc.xml");
        } else {
            // 常见 DLNA 端口列表（小米电视/音箱常用 49152, 49153, 49154, 8080, 8200）
            const char* common_ports[] = {"49152", "49153", "49154", "8080", "8200", "1900"};
            for (const char* port : common_ports) {
                urls_to_try.push_back("http://" + host + ":" + port + "/description.xml");
                urls_to_try.push_back("http://" + host + ":" + port + "/devicedesc.xml");
                urls_to_try.push_back("http://" + host + ":" + port + "/rootDesc.xml");
            }
        }
    }

    for (const auto& url : urls_to_try) {
        ESP_LOGI(TAG, "Probing DLNA at: %s", url.c_str());
        auto http = network->CreateHttp(0);
        if (!http) continue;
        http->SetTimeout(2500);
        if (http->Open("GET", url)) {
            auto status = http->GetStatusCode();
            if (status && *status == 200) {
                std::string xml = http->ReadAll();
                std::string base_url = GetBaseUrl(url);
                std::string name, ctrl_url, udn;
                if (ParseDeviceXml(xml, base_url, name, ctrl_url, udn)) {
                    DlnaDevice dev;
                    dev.name = name;
                    dev.control_url = ctrl_url;
                    dev.location = url;
                    dev.udn = udn;
                    ESP_LOGI(TAG, "Manual DLNA probe success: [%s] -> %s", name.c_str(), ctrl_url.c_str());
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        // 去重
                        bool exists = false;
                        for (auto& d : devices_) {
                            if (d.control_url == ctrl_url || (!udn.empty() && d.udn == udn)) {
                                d = dev;
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            devices_.push_back(std::move(dev));
                        }
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

void DlnaController::StartDiscovery(DeviceListCallback on_update) {
    if (is_scanning_) {
        ESP_LOGI(TAG, "Discovery already running, skip");
        return;
    }

    is_scanning_ = true;
    on_update_cb_ = std::move(on_update);

    // 提升任务栈至 8192 字节，避免接收长 XML 时发生栈溢出重启
    xTaskCreate(
        [](void* param) {
            auto self = static_cast<DlnaController*>(param);
            ESP_LOGI(TAG, "Starting robust DLNA SSDP discovery task (stack 8KB)...");

            int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock < 0) {
                ESP_LOGE(TAG, "Failed to create UDP socket for SSDP");
                self->is_scanning_ = false;
                vTaskDelete(NULL);
                return;
            }

            // 关键：必须显式绑定本地地址和随机端口，否则无法接收小米电视/音箱单播回包
            struct sockaddr_in local_addr;
            memset(&local_addr, 0, sizeof(local_addr));
            local_addr.sin_family = AF_INET;
            local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            local_addr.sin_port = htons(0);
            if (bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
                ESP_LOGW(TAG, "SSDP bind local port failed, errno=%d", errno);
            }

            // 设置 2000ms 接收超时
            struct timeval tv;
            tv.tv_sec = 2;
            tv.tv_usec = 0;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            // 设置组播 TTL
            uint8_t ttl = 4;
            setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

            struct sockaddr_in dest_addr;
            memset(&dest_addr, 0, sizeof(dest_addr));
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_addr.s_addr = inet_addr("239.255.255.250");
            dest_addr.sin_port = htons(1900);

            // 针对小米电视/小爱音箱等设备的标准广谱探测报文
            const char* queries[] = {
                "M-SEARCH * HTTP/1.1\r\n"
                "HOST: 239.255.255.250:1900\r\n"
                "MAN: \"ssdp:discover\"\r\n"
                "MX: 2\r\n"
                "ST: ssdp:all\r\n\r\n",

                "M-SEARCH * HTTP/1.1\r\n"
                "HOST: 239.255.255.250:1900\r\n"
                "MAN: \"ssdp:discover\"\r\n"
                "MX: 2\r\n"
                "ST: upnp:rootdevice\r\n\r\n",

                "M-SEARCH * HTTP/1.1\r\n"
                "HOST: 239.255.255.250:1900\r\n"
                "MAN: \"ssdp:discover\"\r\n"
                "MX: 2\r\n"
                "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n",

                "M-SEARCH * HTTP/1.1\r\n"
                "HOST: 239.255.255.250:1900\r\n"
                "MAN: \"ssdp:discover\"\r\n"
                "MX: 2\r\n"
                "ST: urn:schemas-upnp-org:service:AVTransport:1\r\n\r\n"
            };

            // 广播发送探测报文
            for (const char* q : queries) {
                sendto(sock, q, strlen(q), 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
            }

            std::set<std::string> discovered_locations;
            auto rx_buf = std::make_unique<char[]>(2048);

            while (true) {
                struct sockaddr_in src_addr;
                socklen_t addr_len = sizeof(src_addr);
                int len = recvfrom(sock, rx_buf.get(), 2047, 0,
                                   (struct sockaddr*)&src_addr, &addr_len);
                if (len <= 0) {
                    break;  // 超时退出
                }
                rx_buf[len] = '\0';
                std::string resp(rx_buf.get());

                // 提取 LOCATION 字段（不区分大小写）
                size_t loc_pos = resp.find("LOCATION:");
                if (loc_pos == std::string::npos) {
                    loc_pos = resp.find("Location:");
                }
                if (loc_pos == std::string::npos) {
                    loc_pos = resp.find("location:");
                }

                if (loc_pos != std::string::npos) {
                    size_t start = loc_pos + 9;
                    while (start < resp.size() && (resp[start] == ' ' || resp[start] == '\t')) {
                        start++;
                    }
                    size_t end = resp.find("\r\n", start);
                    if (end != std::string::npos) {
                        std::string loc = resp.substr(start, end - start);
                        discovered_locations.insert(loc);
                    }
                }
            }

            close(sock);
            ESP_LOGI(TAG, "SSDP probing complete, found %d candidate locations",
                     (int)discovered_locations.size());

            auto network = Board::GetInstance().GetNetwork();
            std::vector<DlnaDevice> valid_devs;

            if (network) {
                for (const auto& loc : discovered_locations) {
                    ESP_LOGI(TAG, "Fetching device desc from: %s", loc.c_str());
                    auto http = network->CreateHttp(0);
                    if (!http) {
                        continue;
                    }
                    http->SetTimeout(3500);
                    if (http->Open("GET", loc)) {
                        auto status = http->GetStatusCode();
                        if (status && *status == 200) {
                            std::string xml = http->ReadAll();
                            std::string base_url = GetBaseUrl(loc);
                            std::string name, ctrl_url, udn;
                            if (ParseDeviceXml(xml, base_url, name, ctrl_url, udn)) {
                                DlnaDevice dev;
                                dev.name = name;
                                dev.control_url = ctrl_url;
                                dev.location = loc;
                                dev.udn = udn;
                                ESP_LOGI(TAG, "Valid DLNA device found: [%s] -> %s", name.c_str(),
                                         ctrl_url.c_str());
                                valid_devs.push_back(std::move(dev));
                            }
                        }
                    }
                }
            }

            // 更新设备列表
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                // 合并现有设备与新发现设备
                for (auto& new_d : valid_devs) {
                    bool exists = false;
                    for (auto& old_d : self->devices_) {
                        if (old_d.control_url == new_d.control_url ||
                            (!new_d.udn.empty() && old_d.udn == new_d.udn)) {
                            old_d = new_d;
                            exists = true;
                            break;
                        }
                    }
                    if (!exists) {
                        self->devices_.push_back(std::move(new_d));
                    }
                }

                if (self->target_index_ >= (int)self->devices_.size()) {
                    self->target_index_ = -1;
                }
                self->is_scanning_ = false;
            }

            if (self->on_update_cb_) {
                auto cb = self->on_update_cb_;
                auto current_devices = self->GetDevices();
                Application::GetInstance().Schedule([cb, current_devices]() {
                    cb(current_devices);
                });
            }

            vTaskDelete(NULL);
        },
        "dlna_ssdp", 8192, this, 3, NULL);
}

std::vector<DlnaDevice> DlnaController::GetDevices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_;
}

void DlnaController::SetTargetIndex(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < -1 || index >= (int)devices_.size()) {
        target_index_ = -1;
    } else {
        target_index_ = index;
    }
    ESP_LOGI(TAG, "Selected playback target: %d (%s)", target_index_,
             target_index_ == -1 ? "Local Speaker" : devices_[target_index_].name.c_str());
}

int DlnaController::GetTargetIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return target_index_;
}

std::string DlnaController::GetTargetName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (target_index_ >= 0 && target_index_ < (int)devices_.size()) {
        return devices_[target_index_].name;
    }
    return "本地扬声器";
}

bool DlnaController::SendSoapAction(const std::string& control_url, const std::string& action,
                                   const std::string& soap_body) {
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "Network not available for SOAP action %s", action.c_str());
        return false;
    }

    auto http = network->CreateHttp(0);
    if (!http) {
        return false;
    }

    http->SetTimeout(6000);
    http->SetHeader("Content-Type", "text/xml; charset=\"utf-8\"");
    std::string soap_action_hdr = "\"urn:schemas-upnp-org:service:AVTransport:1#" + action + "\"";
    http->SetHeader("SOAPAction", soap_action_hdr);

    std::string payload =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
        "  <s:Body>\r\n" +
        soap_body +
        "  </s:Body>\r\n"
        "</s:Envelope>\r\n";

    http->SetContent(std::move(payload));
    if (http->Open("POST", control_url)) {
        auto status = http->GetStatusCode();
        ESP_LOGI(TAG, "SOAP action [%s] response status: %d", action.c_str(),
                 status ? *status : -1);
        return (status && *status >= 200 && *status < 300);
    }
    ESP_LOGW(TAG, "SOAP action [%s] request failed to open", action.c_str());
    return false;
}

bool DlnaController::Play(int device_idx, const std::string& media_url, const std::string& title,
                         const std::string& artist) {
    std::string ctrl_url;
    std::string dev_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (device_idx < 0 || device_idx >= (int)devices_.size()) {
            ESP_LOGE(TAG, "Invalid DLNA device index: %d", device_idx);
            return false;
        }
        ctrl_url = devices_[device_idx].control_url;
        dev_name = devices_[device_idx].name;
    }

    ESP_LOGI(TAG, "Casting to [%s] -> %s", dev_name.c_str(), media_url.c_str());

    // 1. 构建 SetAVTransportURI 报文 (XML 特殊字符转义)
    std::string esc_url = EscapeXml(media_url);
    std::string esc_title = EscapeXml(title.empty() ? "Navidrome Music" : title);
    std::string esc_artist = EscapeXml(artist.empty() ? "XiaoZhi Cast" : artist);

    // 构建 DIDL-Lite 元数据（用于电视/音箱屏幕展示歌名与歌手）
    std::string raw_didl =
        "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
        "<item id=\"0\" parentID=\"-1\" restricted=\"1\">"
        "<dc:title>" + esc_title + "</dc:title>"
        "<dc:creator>" + esc_artist + "</dc:creator>"
        "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
        "</item></DIDL-Lite>";
    std::string esc_didl = EscapeXml(raw_didl);

    std::ostringstream set_uri_body;
    set_uri_body << "    <u:SetAVTransportURI xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">\r\n"
                 << "      <InstanceID>0</InstanceID>\r\n"
                 << "      <CurrentURI>" << esc_url << "</CurrentURI>\r\n"
                 << "      <CurrentURIMetaData>" << esc_didl << "</CurrentURIMetaData>\r\n"
                 << "    </u:SetAVTransportURI>\r\n";

    if (!SendSoapAction(ctrl_url, "SetAVTransportURI", set_uri_body.str())) {
        ESP_LOGW(TAG, "SetAVTransportURI failed for %s", dev_name.c_str());
        return false;
    }

    // 2. 构建 Play 报文
    std::string play_body =
        "    <u:Play xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">\r\n"
        "      <InstanceID>0</InstanceID>\r\n"
        "      <Speed>1</Speed>\r\n"
        "    </u:Play>\r\n";

    return SendSoapAction(ctrl_url, "Play", play_body);
}

bool DlnaController::PlayCurrent(const std::string& media_url, const std::string& title,
                                const std::string& artist) {
    int idx = GetTargetIndex();
    if (idx < 0) {
        return false;
    }
    return Play(idx, media_url, title, artist);
}

bool DlnaController::Pause(int device_idx) {
    std::string ctrl_url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int idx = (device_idx >= 0) ? device_idx : target_index_;
        if (idx < 0 || idx >= (int)devices_.size()) {
            return false;
        }
        ctrl_url = devices_[idx].control_url;
    }

    std::string pause_body =
        "    <u:Pause xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">\r\n"
        "      <InstanceID>0</InstanceID>\r\n"
        "    </u:Pause>\r\n";

    return SendSoapAction(ctrl_url, "Pause", pause_body);
}

bool DlnaController::Stop(int device_idx) {
    std::string ctrl_url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int idx = (device_idx >= 0) ? device_idx : target_index_;
        if (idx < 0 || idx >= (int)devices_.size()) {
            return false;
        }
        ctrl_url = devices_[idx].control_url;
    }

    std::string stop_body =
        "    <u:Stop xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">\r\n"
        "      <InstanceID>0</InstanceID>\r\n"
        "    </u:Stop>\r\n";

    return SendSoapAction(ctrl_url, "Stop", stop_body);
}

}  // namespace waveshare185c
