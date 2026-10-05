#include "notification_service.h"
#include "board.h"
#include "application.h"
#include "custom_lcd_display.h"

#include <esp_log.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace waveshare185c {

static const char* TAG = "NotifyService";

// 服务端 Mosquitto 自签 CA 证书 (SAN 包含 193.177.220.138)
static const char kCaCertPem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDazCCAlOgAwIBAgIUdmEwsFtco7u/+cPR6PKXqsQdLgIwDQYJKoZIhvcNAQEL\n"
    "BQAwKzEXMBUGA1UEAwwOeGlhb3poaS1ub3RpZnkxEDAOBgNVBAoMB3hpYW96aGkw\n"
    "HhcNMjYxMDA1MDMzODQxWhcNMzYxMDAyMDMzODQxWjArMRcwFQYDVQQDDA54aWFv\n"
    "emhpLW5vdGlmeTEQMA4GA1UECgwHeGlhb3poaTCCASIwDQYJKoZIhvcNAQEBBQAD\n"
    "ggEPADCCAQoCggEBAKhK4f8rkNFobkGbyeXcDOY1bVWp4i5un7gmd4c+UqjqsCJD\n"
    "kRKDmmXhsW50qS1dYdq0sa2yxPXlzzhocLGNUUnpO05bpJhOwQlORNY5FCk30duA\n"
    "TE7MtfJR77eGVLnyMICa25hMhR63OH+Hl2ijYlJCbVqfPRmYeFve9UbtgmMH+Vn+\n"
    "iMK5zbjlZUn24rsAi5k9CfBzevhqzJ45g59r3VgLSpjQeYFop1wQXH7CFZRq7bys\n"
    "Pq0gGwEAjKLc608Ga1qHxx4nCaaE/GpSK6GafYcuy0Hd3MQxAi4DIGxPoExyondI\n"
    "lRQzrQc6Jm/4mCF2k4SrFKNMDuj7CvJ3Jscnn7sCAwEAAaOBhjCBgzAdBgNVHQ4E\n"
    "FgQUY4TRb9GWuTie4wmfoJiora1sRoAwHwYDVR0jBBgwFoAUY4TRb9GWuTie4wmf\n"
    "oJiora1sRoAwDwYDVR0TAQH/BAUwAwEB/zAwBgNVHREEKTAngg54aWFvemhpLW5v\n"
    "dGlmeYIJbG9jYWxob3N0hwTBsdyKhwR/AAABMA0GCSqGSIb3DQEBCwUAA4IBAQB3\n"
    "UjUElVpIPtqsERKTKWYG7ZbxrEY7h+56ilKZQ5KazHAy11e3Qv7SiY3BfifppyiS\n"
    "KvP1wE40/7XkOwAAp4PIN0XZyafMj7dSJ+jx0cF/drZZKNSyJHIj6ahjHQMxF88P\n"
    "adVpMGJS8fngD76ChtJ6MkQZYJ4pT20WSPGcVoONXDYccKlYlMtuUal58VhZnLrb\n"
    "/clXNR4s93nyI/SYSFbYghf2PJBrOh8DJ5LS7eP3l1M9vje8fTa5H3db6FXoAQz+\n"
    "H9NPyjEj889NhfOq+aHBrHJ4v7VTEGaZlSZQG/FBX2NhlZqkwiuTGBC2KFPFHHK7\n"
    "wYqurJ0BOutWPbpVaZM5\n"
    "-----END CERTIFICATE-----\n";

NotificationService::NotificationService() {
}

NotificationService::~NotificationService() {
    Stop();
}

void NotificationService::Initialize() {
    service_config_ = CreateDefaultServiceConfig();
    LoadConfig();

    // 创建队列轮询轻量定时器（每 1 秒检查一次是否有积压待播通知）
    if (!check_timer_) {
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                static_cast<NotificationService*>(arg)->ProcessQueue();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "notify_queue_chk",
            .skip_unhandled_events = true
        };
        esp_timer_create(&timer_args, &check_timer_);
    }
}

void NotificationService::Start() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (is_started_) {
        return;
    }

    if (!config_.IsConfigured()) {
        ESP_LOGW(TAG, "Notification channel not configured, skipping start");
        return;
    }

    ESP_LOGI(TAG, "Starting NotificationService, broker: %s:%d, topic: %s",
             config_.mqtt_host.c_str(), (int)config_.mqtt_port, config_.mqtt_topic.c_str());

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.hostname = config_.mqtt_host.c_str();
    mqtt_cfg.broker.address.port = config_.mqtt_port;
    mqtt_cfg.broker.address.transport =
        config_.UsesTls() ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;

    if (config_.UsesTls()) {
        mqtt_cfg.broker.verification.certificate = kCaCertPem;
        mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
    }

    if (!config_.mqtt_user.empty()) {
        mqtt_cfg.credentials.username = config_.mqtt_user.c_str();
        mqtt_cfg.credentials.authentication.password = config_.mqtt_pass.c_str();
    }

    std::string client_id = "xiaozhi-" + Board::GetInstance().GetUuid().substr(0, 8);
    mqtt_cfg.credentials.client_id = client_id.c_str();
    mqtt_cfg.session.keepalive = 60;
    mqtt_cfg.network.reconnect_timeout_ms = 5000;

    mqtt_client_ = esp_mqtt_client_init(&mqtt_cfg);
    if (!mqtt_client_) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }

    esp_mqtt_client_register_event(mqtt_client_, MQTT_EVENT_ANY, MqttEventHandler, this);
    esp_err_t err = esp_mqtt_client_start(mqtt_client_);
    if (err == ESP_OK) {
        is_started_ = true;
        if (check_timer_) {
            esp_timer_start_periodic(check_timer_, 1000000); // 1秒周期
        }
    } else {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(mqtt_client_);
        mqtt_client_ = nullptr;
    }
}

void NotificationService::Stop() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (check_timer_) {
        esp_timer_stop(check_timer_);
    }
    if (mqtt_client_) {
        esp_mqtt_client_stop(mqtt_client_);
        esp_mqtt_client_destroy(mqtt_client_);
        mqtt_client_ = nullptr;
    }
    is_started_ = false;
    is_connected_ = false;
    queue_.clear();
}

void NotificationService::MqttEventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    auto* self = static_cast<NotificationService*>(handler_args);
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            self->HandleMqttConnected();
            break;
        case MQTT_EVENT_DISCONNECTED:
            self->HandleMqttDisconnected();
            break;
        case MQTT_EVENT_DATA:
            self->HandleMqttData(event->topic, event->topic_len, event->data, event->data_len);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            break;
    }
}

void NotificationService::HandleMqttConnected() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    is_connected_ = true;
    ESP_LOGI(TAG, "MQTT Connected to %s:%d", config_.mqtt_host.c_str(), (int)config_.mqtt_port);

    if (!config_.mqtt_topic.empty() && mqtt_client_) {
        int msg_id = esp_mqtt_client_subscribe(mqtt_client_, config_.mqtt_topic.c_str(), 1);
        ESP_LOGI(TAG, "Subscribed to %s, msg_id=%d", config_.mqtt_topic.c_str(), msg_id);
    }
}

void NotificationService::HandleMqttDisconnected() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    is_connected_ = false;
    ESP_LOGW(TAG, "MQTT Disconnected");
}

void NotificationService::HandleMqttData(const char* topic, int topic_len, const char* data, int data_len) {
    std::string topic_str(topic, topic_len);
    std::string payload_str(data, data_len);
    ESP_LOGI(TAG, "MQTT Data received on [%s]: %s", topic_str.c_str(), payload_str.c_str());

    cJSON* root = cJSON_Parse(payload_str.c_str());
    if (!root) {
        ESP_LOGW(TAG, "Invalid JSON payload, treating as plain text");
        EnqueueNotification(payload_str);
        return;
    }

    std::string text;
    std::string audio_url;
    int priority = 1;

    cJSON* j_text = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(j_text) && j_text->valuestring) {
        text = j_text->valuestring;
    }
    cJSON* j_audio = cJSON_GetObjectItem(root, "audio_url");
    if (cJSON_IsString(j_audio) && j_audio->valuestring) {
        audio_url = j_audio->valuestring;
    }
    cJSON* j_prio = cJSON_GetObjectItem(root, "priority");
    if (cJSON_IsNumber(j_prio)) {
        priority = j_prio->valueint;
    }

    cJSON_Delete(root);

    if (text.empty() && audio_url.empty()) {
        ESP_LOGW(TAG, "Empty notification message, ignored");
        return;
    }

    EnqueueNotification(text, audio_url, priority);
}

void NotificationService::EnqueueNotification(const std::string& text, const std::string& audio_url, int priority) {
    // 限制单次文本长度，避免恶意长报文导致 URL 溢出
    std::string clean_text = text;
    if (clean_text.length() > 300) {
        clean_text = clean_text.substr(0, 300);
    }

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        PendingNotification item;
        item.text = clean_text;
        item.audio_url = audio_url;
        item.priority = priority;
        item.created_at_ms = esp_timer_get_time() / 1000;

        // 队列满时丢弃最低优先级/最老的消息
        if (queue_.size() >= kMaxQueueSize) {
            ESP_LOGW(TAG, "Notification queue full (%zu), dropping lowest-priority notification", queue_.size());
            queue_.pop_back();
        }

        queue_.push_back(std::move(item));

        // 按 priority 升序（数值越小优先级越高）排序
        std::stable_sort(queue_.begin(), queue_.end(), [](const PendingNotification& a, const PendingNotification& b) {
            return a.priority < b.priority;
        });

        ESP_LOGI(TAG, "Notification enqueued: \"%s\" (prio=%d, total in queue=%zu)",
                 clean_text.c_str(), priority, queue_.size());
    }

    // 释放锁后再尝试立刻调度播放
    ProcessQueue();
}

void NotificationService::ProcessQueue() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    // 严格状态保护：只有在系统处于待命 Idle 状态，且没有播放音乐时才播报！
    if (state != kDeviceStateIdle) {
        return;
    }

    auto* display = Board::GetInstance().GetDisplay();
    auto* custom_disp = static_cast<CustomLcdDisplay*>(display);
    if (custom_disp && custom_disp->IsPlaying()) {
        return; // 正在播放音乐，暂不打扰
    }

    PendingNotification item;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (queue_.empty()) {
            return;
        }
        item = queue_.front();
        queue_.erase(queue_.begin());
    }

    std::string play_url = item.audio_url;
    if (play_url.empty()) {
        play_url = BuildTtsUrl(item.text);
    }

    ESP_LOGI(TAG, "Triggering notification playback: text=\"%s\", url=\"%s\"",
             item.text.c_str(), play_url.c_str());

    // 调度到主线程安全调用核心通知接口
    app.Schedule([text = item.text, url = play_url]() {
        auto& current_app = Application::GetInstance();
        if (current_app.GetDeviceState() == kDeviceStateIdle) {
            std::vector<NotifySubtitle> subs;
            if (!text.empty()) {
                subs.push_back({0, text});
            }
            current_app.StartNotification(url, std::move(subs));
        } else {
            // 若最后一刻状态发生改变（例如被即刻唤醒），重新将此消息推回队列
            NotificationService::GetInstance().EnqueueNotification(text, url, 1);
        }
    });
}

std::string NotificationService::UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (unsigned char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << (char)c;
        } else {
            escaped << '%' << std::setw(2) << std::uppercase << (int)c;
        }
    }
    return escaped.str();
}

std::string NotificationService::BuildTtsUrl(const std::string& text) {
    std::string encoded = UrlEncode(text);
    std::string url = config_.tts_base_url;
    if (url.find('?') == std::string::npos) {
        url += "?";
    } else {
        url += "&";
    }
    url += "text=" + encoded;
    if (!config_.tts_token.empty()) {
        url += "&token=" + UrlEncode(config_.tts_token);
    }
    if (!config_.tts_voice.empty()) {
        url += "&voice=" + UrlEncode(config_.tts_voice);
    }
    if (!config_.tts_format.empty()) {
        url += "&fmt=" + UrlEncode(config_.tts_format);
    }
    return url;
}

std::string NotificationService::GetStatusJson() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "configured", config_.IsConfigured());
    cJSON_AddBoolToObject(root, "connected", is_connected_);
    cJSON_AddStringToObject(root, "broker",
                            (config_.mqtt_host + ":" + std::to_string(config_.mqtt_port)).c_str());
    cJSON_AddStringToObject(root, "topic", config_.mqtt_topic.c_str());
    cJSON_AddStringToObject(root, "tts_voice", config_.tts_voice.c_str());
    cJSON_AddStringToObject(root, "tts_format", config_.tts_format.c_str());
    cJSON_AddNumberToObject(root, "queue_size", (int)queue_.size());

    char* printed = cJSON_PrintUnformatted(root);
    std::string res = printed ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(root);
    return res;
}

bool NotificationService::TestNotify(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    EnqueueNotification(text, "", 0); // 0 为最高优先级
    return true;
}

bool NotificationService::SetConfig(const NotifyConfig& new_config) {
    if (!service_config_) {
        ESP_LOGE(TAG, "Notification channel not initialized");
        return false;
    }

    NotifyConfig cur = service_config_->GetNotifyConfig();
    std::string err_msg;
    if (!service_config_->SetNotifyConfig(new_config.mqtt_host, new_config.mqtt_port,
                                          new_config.mqtt_user, new_config.mqtt_pass,
                                          new_config.mqtt_topic, new_config.tts_base_url,
                                          new_config.tts_token, new_config.tts_voice,
                                          new_config.tts_format, err_msg)) {
        ESP_LOGE(TAG, "Failed to persist notification config: %s", err_msg.c_str());
        return false;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        config_ = service_config_->GetNotifyConfig();
    }

    ESP_LOGI(TAG, "Notification config updated, reconnecting...");
    Stop();
    Start();

    (void)cur;
    return true;
}

void NotificationService::LoadConfig() {
    if (service_config_) {
        config_ = service_config_->GetNotifyConfig();
    }
}

} // namespace waveshare185c
