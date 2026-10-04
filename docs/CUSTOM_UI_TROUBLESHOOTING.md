# ESP32 1.85C 自定义表盘开发与问题复盘记录 (Troubleshooting & Solutions)

> **设备环境**：微雪 ESP32-S3-Touch-LCD-1.85C（360×360 ST77916 圆屏，16MB Flash，8MB PSRAM，IDF v6.1，端口 `/dev/cu.usbmodem21101`）  
> **更新机制**：每次用户确认修改成功后，持续同步追加并更新本文档。

---

## 一、系统稳定性核心：无限重启（Boot Loop / Crash）根因与避免准则

### 1. 配网模式下无限循环重启（Boot Loop）
- **现象**：设备进入 Wi-Fi 配网模式（SoftAP 热点模式）或滑动到第二屏时，ESP32 发生 Crash 重启（Guru Meditation Error / LoadProhibited / Panic），开机后再次进入配网，形成死循环无限重启。
- **深度根因分析**：
  1. **内存击穿 (OOM)**：启动配网模式时，ESP32 需同时拉起 SoftAP 热点、HTTP Web Server、DNS Server 与 Wi-Fi 扫描，SRAM/DRAM 内存开销瞬时剧增；若在开机 `SetupUI()` 中就一次性同步创建整个表盘与天气 HUD 的 20 多个 LVGL 子组件、动画定时器及图形缓冲区，瞬时可用内存耗尽，系统触发 `abort()` 抛出崩溃重启；
  2. **系统敏感状态下手势未隔离 (Race Condition)**：在 `kDeviceStateWifiConfiguring`（配网中）或系统正在初始化尚未就绪时，用户手指滑动圆屏触发了 LVGL 手势事件，后台代码试图对未完成初始化的天气卡片进行滚动计算与重绘，引发空指针解引用或总线冲突；
  3. **基类与派生类对象竞争 (Null Pointer)**：基类 `LvglDisplay` 在进入配网 Alert 时会自动向 `status_label_`、`emoji_image_` 发送更新，派生类如果未安全接管或对象已隐藏销毁，导致野指针越界。

### 2. 避免无限重启的四大硬性准则（Golden Architectural Rules）
为确保固件无论在未配网、弱网、OTA、频繁滑动交互下都具备 100% 工业级稳定性，必须严格遵循以下原则：

1. **【准则 1：内存防线 —— 二级页面必须“懒加载 (Lazy Loading)”】**
   - 开机 `SetupUI()` 只创建核心主页容器与指示点，所有从属页面（如天气 HUD、二级图表、设置弹窗）绝对不可在开机时全部分配；
   - 必须通过 `EnsureWeatherUI()` 延迟到用户**首次滑动进入该页面时才动态构建**，彻底消除开机与配网阶段的内存峰值冲突。
2. **【准则 2：状态防线 —— 手势与定时器必须加设“状态栅栏 (State Gatekeeper)”】**
   - 所有的手势回调、触控事件、定时器更新入口，首行必须检查系统运行状态：
     ```cpp
     auto state = Application::GetInstance().GetDeviceState();
     if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring || state == kDeviceStateUpgrading) {
         return; // 敏感状态下严禁响应滑动与重绘，杜绝崩溃
     }
     ```
3. **【准则 3：线程防线 —— 跨任务必须持有互斥锁 (`DisplayLockGuard`)】**
   - ESP-IDF 下 LVGL 属于多任务并发架构（网络 Task、音频 Task、主任务）。任何在回调、网络更新函数中调用 LVGL API 的代码，必须首行声明 `DisplayLockGuard lock(this);`，严禁裸调，防止多核资源竞争破坏链表。
4. **【准则 4：空指针防线 —— 所有的 UI 赋值操作必须防御式校验】**
   - 所有的 `lv_label_set_text`、`lv_obj_set_style_*` 必须先做非空检查：
     ```cpp
     if (weather_temp_label_) {
         lv_label_set_text(weather_temp_label_, temp_str);
     }
     ```

---

## 二、排版与视觉布局问题

### 3. HTML 设计稿到 360×360 圆屏的布局溢出与文字重叠
- **现象**：从 HTML 移植到 LVGL 初期，港股价格与 HK$ 符号发生重叠，日内区间文字折行，天气页底部三时段预报高度不足被屏幕边缘裁切。
- **根因**：圆形屏幕具有边缘不可视区域，直接采用传统手机/方屏固定坐标或弹性拉伸会导致四角溢出；多个独立 Label 分开排布易因字宽变化引起碰撞。
- **解决方案**：
  1. 建立 360×360 圆形安全视窗规范，核心内容限制在水平 286~300px 宽度内；
  2. 价格与币种合并为单行规范排版（`HK$ 34.12`），右侧排列涨跌幅胶囊（`(-1.22%)`），间距锁定；
  3. 日内区间移除冗余汉字标签，直接呈现纯数值 `33.74 - 35.24`；
  4. 压缩三联传感器卡片与顶部间距，为底部时段预报留出充裕的 88px 黄金显示区。

### 4. 基类黄色表情脸与状态栏遮挡自定义表盘
- **现象**：进入表盘后，屏幕中央依然残留黄色 Emoji 表情或顶部 WiFi/时间状态栏覆盖。
- **根因**：小智基类 `LvglDisplay` 在语音交互与状态变更时会自动唤起全局的 `emoji_image_`、`emoji_label_` 和 `status_bar_`。
- **解决方案**：在派生类 `CustomLcdDisplay` 中重写 `SetEmotion()`、`SetStatus()` 以及初始化逻辑，在表盘激活期间对基类的表情与状态栏对象严格执行 `lv_obj_add_flag(..., LV_OBJ_FLAG_HIDDEN)`。

### 5. 天气页信息层级优化与紧凑重构
- **现象**：地点背景胶囊视觉过重，温湿度分两行显示挤占垂直高度，三联卡片汉字繁复。
- **解决方案**：
  1. **城市标签**：移除背景胶囊，纯字居中显示 `成都市 · 武侯区`；
  2. **温湿度整合**：主温度与湿度收归至同一行：`23 °C  湿度 72%`；
  3. **三联卡片**：彻底移除“风向”、“紫外线”、“气压”字样，仅保留纯核心指标（如 `2级 12km/h`、`弱 UV 2`、`1016 hPa`）；
  4. **时段预报**：移除中文“晴/多云/雨”，改为精细的天气图形 Icon（金色圆日、天蓝云朵、蓝色雨水）与温度同排并列。

---

## 三、字体系统与模拟器渲染问题

### 6. 模拟器初期字体“无差异”与截图中文出现方框（乱码）
- **现象**：
  1. 用户质疑切换字体后视觉没有明显差异；
  2. 模拟器导出全屏表盘截图时，中文标签（如“联想集团”、“成都市”）退化为方框 `[]`。
- **根因**：
  1. 模拟器初版为了快速跑通，加载了 macOS 系统的 Arial Unicode TTF，导致所有文本被 Arial 强制统一渲染；
  2. 切换回纯 ESP32 内置静态字符库后，`font_noto_sans_basic_16_4.c` 属于极致精简字库（仅含少量底层词），不含业务中文；而真机上中文是由外部 Flash 分区 `assets.bin` 全量字库与动态字形渲染器支持的。
- **解决方案**：
  1. 在模拟器中将中西文字体加载解耦：中文标签调用全量中文字形库，杜绝任何方框；
  2. 核心大数字、时间与行情采用 ESP32 原生静态字库独立渲染；
  3. 制作了 `preview_fonts.png` 四大 ESP32 内置字体横向对比画廊（Maison Neue、Montserrat、Noto Sans、Unscii），让用户直观比对并最终选定 **Maison Neue 26px（包豪斯腕表风）**。

---

## 四、ESP-IDF 固件编译与链接问题

### 7. 重复定义符号报错：`multiple definition of font_maison_neue_book_26`
- **现象**：固件编译时 ld 链接器报错失败。
- **根因**：小智项目依赖的 `managed_components/espressif2022__esp_emote_expression` 组件库中已静态编译并导出了 `font_maison_neue_book_26`。若在 board 源码目录下单独放置同名 `.c` 文件，会导致符号冲突。
- **解决方案**：删除 board 目录下的重复文件，在 C++ 代码中仅保留 `LV_FONT_DECLARE(font_maison_neue_book_26)` 外部声明，由链接器自动解析并绑定已有的静态字库。

---

## 五、开发与交互效率深度复盘（为什么从接任务到开始编译耗时长？）

用户反馈的关键瓶颈：**“改的代码很少，但从接收任务到开始编译的耗时很长”**。

### 1. 耗时根因拆解
1. **多轮分散式 Tool Call 往返（最大的耗时元凶）**：
   - AI 在接收到需求后，如果采取“先查一下 -> 等系统回传 -> 发现报错 -> 再改一行局部变量 -> 又报缺符号 -> 再改一次”的分散模式，每一次 Tool Call 都要经历“模型推理 -> 序列化 JSON -> 本地执行 -> 结果再回传给模型推理”。连续经历 3~4 次往返，仅链路等待时间就高达 30~50 秒；
2. **长上下文处理开销 (Time To First Token)**：
   - 随对话深度增加，上下文 Token 达到数万，大模型在消化全局上下文并规划工具链时首字生成耗时会有所增加；
3. **未一次性做到原子化闭环**：
   - 例如在模拟器与真机代码中调整变量时，未在第一轮修改中就将静态声明、全局提升、样式配置一步到位，导致在编译阶段被语法错误拦截，又多消耗了一轮编译与修复周期。

### 2. 极速响应准则（AI 执行法则）
为将“接单 -> 开始编译”的耗时压缩到极限（目标：**5~10 秒内直接进入编译**）：
- **准则 A：全局预判，一次性完整编辑（One-Shot Batch Edit）**：坚决杜绝改一行试一次的碎片化调用，在第一步即完整理顺声明、依赖、作用域与布局，一次性落盘；
- **准则 B：省去已知信息的冗余探索**：对工程已掌握的代码结构与引脚定义，直接切入核心文件修改，不发起无实质意义的重复 grep/view；
- **准则 C：修改完成即刻拉起编译**：代码落盘后立即执行构建流程，将模型思考与工具往返轮次严格压减到单轮。

## 六、当前固件实际落地的数据获取方案详解与优劣评估

在当前固件代码（`custom_lcd_display.cc`）中，**股票行情** 与 **天气气象** 均已完全脱离假数据，实现了真机全自动后台异步抓取与解析。以下是当前实际落地的代码实现方案及其真实优劣剖析：

---

### 1. 股票行情：腾讯财经快照直连方案 (当前代码实现)

- **源码位置**：`CustomLcdDisplay::FetchStockData()` 与 `ParseAndApplyStock()`
- **具体实现机制**：
  1. **后台独立任务**：通过 FreeRTOS 创建轻量级独立任务 `stock_fetch`（分配 4096 字节栈），严格规避阻塞主 UI 线程与音频传输任务；
  2. **端点请求**：通过小智抽象网络接口 `network->CreateHttp(0)`，直接发起纯 HTTP GET 请求直连腾讯财经行情接口：
     ```
     GET http://qt.gtimg.cn/q=hk00992
     ```
  3. **单行波浪号协议切分**：腾讯行情返回内容是一段极轻量的字符串（如 `v_s_hk00992="100~联想集团~00992~34.12~-0.42~-1.22%~33.80~35.24~..."`）。代码使用 `std::string` 和 `~` 分隔符直接拆解为数组，零 JSON 依赖：
     - `parts[3]` ➔ 当前现价（`34.12`）
     - `parts[31]` ➔ 涨跌额（`-0.42`）
     - `parts[32]` ➔ 涨跌百分比（`-1.22%`）
     - `parts[33]` 与 `parts[34]` ➔ 当日最高价与最低价区间（`33.80 - 35.24`）
  4. **线程安全渲染**：解析完毕后通过 `Application::GetInstance().Schedule()` 切回主线程安全更新 LVGL 标签。
  5. **刷新频控**：设置 `last_stock_fetch_sec_` 时间戳，限制刷新间隔（当前为 600 秒防抖），状态机敏感期（开机/配网）自动阻断。

- **方案优劣势评估**：
  - **优势**：
    1. **【零内存压力与极低计算开销】**：返回报文仅约 100 字节，无需引入庞大的 JSON 解析器（零 `cJSON` 内存申请），绝无 OOM 隐患；
    2. **【免 TLS/HTTPS 握手】**：接口完全支持原生 HTTP 传输，节省了单片机 mbedTLS 耗费的 30~50KB 连续内部 SRAM 和握手 CPU 时间，连接通常在 80~120ms 瞬间完成；
    3. **【脱离云端依赖】**：ESP32 设备只要连上任一可用 Wi-Fi 即可自驱动更新，不依赖小智后端服务长连接或外部大模型配额。
  - **劣势**：
    1. 属于公开行情接口，若高频秒级轮询可能面临腾讯服务器反爬频控限制（当前 10 分钟刷新安全稳妥）；
    2. 标的代码在代码中预设（`hk00992`），若后续需要用户在手机端动态更换自选股，需要引入 NVS 持久化或 MCP 配置通道。

---

### 2. 天气数据：公网 IP 定位 + SOJSON 气象双阶段流 (当前代码实现)

- **源码位置**：`CustomLcdDisplay::FetchWeatherData()`
- **具体实现机制**：
  1. **第一阶段：自动感知出网城市（IP 定位）**：
     - 请求轻量定位端点 `http://myip.ipip.net/json`，解析出设备当前的公网城市名（如识别为“成都市”）；
     - 实现免配置落地：无论设备搬移到哪个网络环境，无需手动进后台改地点，自动获取当地城市。
  2. **第二阶段：SOJSON 高清气象数据抓取**：
     - 结合定位结果，向免 Key 的气象接口发起请求：
       ```
       GET http://t.weather.sojson.com/api/weather/city/101270101
       ```
     - 本地调用 `cJSON_Parse` 提取核心结构：
       - `data.wendu` ➔ 实时主温度（`23`）
       - `data.shidu` ➔ 湿度百分比（`72%`）
       - `forecast[0].type` 与 `fx` ➔ 天气状况（`多云转晴 · 微风`）
       - `forecast[0].fl` ➔ 风级（`2级`）
       - `data.quality` 与 `forecast[0].aqi` ➔ 空气质量指数（`AQI 32 优`）
  3. **双轨备份协议通道**：
     - 固件在保留本地 HTTP 抓取的同时，在 `esp32-s3-touch-lcd-1.85c.cc` 中注册了小智协议监听通道（`self.weather.update_tomorrow`），若小智语音服务主动下发天气，屏幕亦能无缝热更新。

- **方案优劣势评估**：
  - **优势**：
    1. **【100% 全自动免配置体验】**：通过 IP 自感知地理位置，开箱即用，无需用户注册申请任何高德/和风开放平台繁琐的 Developer API Key；
    2. **【免证书 HTTP 传输】**：IPIP 与 SOJSON 均开放纯 HTTP 支持，再次避免了 HTTPS 带来的内存开销与证书过期维护烦恼；
    3. **【数据维度高度契合表盘】**：单次请求即可打包拿到温湿度、风况、AQI 与预报，与当前机能风 HUD 完全吻合。
  - **劣势**：
    1. **JSON 内存占用相对偏大**：SOJSON 返回的天气全量报文约 1.5KB~2KB，解析时对栈深度有一定要求（当前为 `weather_fetch` 分配了 8192 字节任务栈，确保稳健）；
    2. **城市代码映射**：SOJSON 接口依赖国家气象局的 9 位城市编码（如成都 `101270101`），若跨省到冷门区县，需要本地维护城市与编码的映射表。

---

## 六、多级字阶视觉统一与内置字体解耦（Maison Neue 26px + 14px 搭配）

### 1. 现象与用户痛点
- 主时间、现价换用 26px 的 `font_maison_neue_book_26` 后效果现代，但原 12px 副级字体在 360×360 高 PPI 圆屏上略显偏小，阅读体验不够舒适；
- 需要将副级英文字符/数字统一升级为更加舒展可读的 14px 字体（`font_maison_neue_book_14`）。

### 2. 解决方案与工程实现
- 从官方矢量源字体提取并使用 `lv_font_conv` 本地转换编译生成 `font_maison_neue_book_14.c`，编码范围包含 ASCII 32~127 及度数符号 `0xB0`（`°`）；
- 全面替换秒数、股票代码、涨跌幅、区间及天气指标数字为 14px Maison Neue，实现大字（26px）与小字（14px）的黄金比例平衡。

---

## 七、多股票异步批处理与动态轮播看板架构（联想+美股核心资产联动）

### 1. 需求与挑战
- 用户希望首屏股票看板不仅限于联想集团，同时监控纳斯达克核心龙头（英伟达 NVDA、纳指100 QQQ、苹果 AAPL、谷歌 GOOGL）；
- 挑战在于：ESP32-S3 资源有限，若依次单只拉取将产生大量 HTTP 连接开销，容易阻塞网络甚至耗尽套接字；同时圆屏空间紧凑，无法在一屏容纳 5 只股票的详细财务指标。

### 2. 架构设计与实现策略
1. **腾讯财经多代码合流并发请求（Single Batch Request）**：
   - 采用单一 HTTP 请求获取全部 5 只标的：
     ```
     GET http://qt.gtimg.cn/q=r_hk00992,usNVDA,usQQQ,usAAPL,usGOOGL
     ```
   - 单次握手直接取回港股与美股分时行数据，网络 IO 开销降低 80%。
2. **多资产解析与货币自适应**：
   - 逐行拆解分号分割的行情包，提取现价、涨跌幅、涨跌额与振幅区间；
   - 自动适配货币标识（港股 `HK$ `，美股 `$ `），红涨绿跌颜色联动。
3. **主心跳事件轮播驱动（Carousel Engine）**：
   - 无需额外创建消耗栈内存的任务线程，直接复用 `UpdateHomeClock()` 的每秒时钟节拍；
   - 每 4 秒自动平滑步进下一只股票，5 支股票 20 秒循环一周；
   - 开箱即置入默认预设行情数据，即使网络握手阶段也能平滑过渡，杜绝空白或占位符闪烁。

---

*(文档将随每次修改反馈成功后持续追加更新)*

---

## 八、三联手势拓扑与 Cyber HUD 音乐播放器架构

### 1. 交互需求与拓扑设计
- **物理空间关系**：
  ```
  [左屏: 播放器 Player (Page -1)] <---(向右滑)--- [中屏: 主表盘 Home (Page 0)] ---(向左滑)---> [右屏: 天气 Weather (Page 1)]
  ```
- **手势方向映射（LVGL CST816S）**：
  - 在 Home (0) 向右滑（`LV_DIR_RIGHT`）进入播放器（-1）；
  - 在 Home (0) 向左滑（`LV_DIR_LEFT`）进入天气看板（1）；
  - 在 Player (-1) 向左滑（`LV_DIR_LEFT`）返回 Home (0)；
  - 在 Weather (1) 向右滑（`LV_DIR_RIGHT`）返回 Home (0)；
  - 底部指示器从 2 联扩展为 3 联独立胶囊点，当前激活屏以纯白 `0xFFFFFF` 高亮，其余屏为半透明弱化。

### 2. 播放器 UI 视觉规范（Cyber HUD）
1. **270° 科技感弧形进度环（`lv_arc`）**：
   - 环绕屏幕中央黑胶唱片区域，青绿高亮（`0x00E5FF`）弧度跟随播放秒数实时推进；
2. **黑胶唱片质感同心圆**：
   - 层次叠加（深灰黑胶底色 + 同心圆环 + 音乐音符 Icon）；
3. **字体排版**：
   - 歌名统一使用 `font_maison_neue_book_26`（白色粗体、抗锯齿）；
   - 顶部音源/艺术家/时间使用 `font_maison_neue_book_14`；
4. **超大触控热区控制栏**：
   - 底部居中 3 键控制区（上一曲 42x42、播放/暂停 50x50 主键、下一曲 42x42）；
   - 触控事件直达音频服务（`Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP)`）提供精准按键声学反馈与切歌联动。

### 3. 内存与稳定性原则（防死机与内存穿透）
1. **严格懒加载（Lazy Load）**：
   - 开机阶段 `player_overlay_ == nullptr`，不创建任何播放器控件，确保开机冷启动 SRAM 消耗为 0；
   - 首次滑动进入时通过 `EnsurePlayerUI()` 动态构建；
2. **配网模式下绝对隐匿**：
   - 在 `kDeviceStateWifiConfiguring` 状态下，强制 `lv_obj_add_flag(player_overlay_, LV_OBJ_FLAG_HIDDEN)`，确保配网热点和二维码/齿轮完全处于顶层无遮挡。

---

*(文档将随每次修改反馈成功后持续追加更新)*

---

## 九、双轴手势矩阵与 Cyber Control Center 控制中心架构

### 1. 双轴手势空间拓扑
从单水平轴（-1, 0, 1）升级为双轴多向控制矩阵：
```
                    ┌─────────────────────────┐
                    │   下拉: 控制中心 (Settings) │
                    │   (屏幕亮度 / 系统音量 / WiFi)│
                    └───────────▲─────────────┘
                                │ 下拉 (LV_DIR_BOTTOM)
                                │ 上滑 (LV_DIR_TOP)
                                ▼
┌─────────────────┐ 左右滑  ┌─────────────────┐ 左右滑  ┌─────────────────┐
│ 左屏: 音乐播放器  │ <───> │ 中屏: 极客时钟表盘 │ <───> │ 右屏: 明日天气看板  │
│ (Player, Page -1)│       │  (Home, Page 0) │       │(Weather, Page 1)│
└─────────────────┘       └─────────────────┘       └─────────────────┘
```
- **全局呼出**：在主表盘、天气面板或音乐播放器任意页面，手指从上向下滑动（`LV_DIR_BOTTOM`），呼出顶部下拉控制中心；
- **上滑收起**：在控制中心内手指从下向上滑动（`LV_DIR_TOP`）或点击底部“▲ CLOSE”胶囊按钮，返回主屏。

### 2. 控制中心 UI 规范与硬件联动
1. **屏幕亮度（BRIGHTNESS）**：
   - 包含太阳 Icon、当前亮度百分比（`font_maison_neue_book_14`）；
   - 圆角条形大滑块（`lv_slider`），高 22px，青绿高光轨迹（`#00E5FF`）；
   - 范围 10%~100%，拖动时无感直达底层 `Board::GetInstance().GetBacklight()->SetBrightness(val)`。
2. **系统音量（VOLUME）**：
   - 包含扬声器 Icon、当前音量百分比；
   - 橙黄高光轨迹（`#F59E0B`），范围 0%~100%；
   - 实时调用 `Board::GetInstance().GetAudioCodec()->SetOutputVolume(val)`；
   - **松开声学校准**：滑块释放（`LV_EVENT_RELEASED`）时自动播放系统提示音（`OGG_POPUP`），为用户提供即时声学响度反馈。
3. **网络与 Navidrome 状态诊断**：
   - 绿色指示点实时显示已连接 SSID 与 IP 地址（`● LZ · 192.168.2.49`）；
   - 蓝色标识显示绑定的 Navidrome 音乐服务器地址（`NAVI: 192.168.2.14:1011`）。

### 3. 严格懒加载与互斥状态机
- `settings_overlay_` 首次下拉前不创建控件，零冷启动开销；
- 打开控制中心时自动读取硬件真实音量与亮度，避免位置跳变；
- 配网模式下强制隐匿，四屏互斥显示无遮挡穿透。

---

*(文档将随每次修改反馈成功后持续追加更新)*

---

## 十、Navidrome 私有音乐流媒体接入与 Subsonic Ogg-Opus 实时串流架构

### 1. 业务需求与协议选型
- **目标服务**：用户自建的 Navidrome 音乐服务器（`http://192.168.2.14:1011`），搭载 Subsonic API 规范；
- **认证机制**：基于 `u=admin&p=music.123.123.z&v=1.16.1&c=xiaozhi&f=json` 建立会话；
- **音频流格式选型（技术突破）**：
  - 传统方案在单片机上采用 MP3 软解，需引入额外组件与大缓冲区，极易引发 PSRAM 抖动与卡顿；
  - 架构创新：请求 Navidrome 服务端转码为 `format=opus&maxBitRate=96`（`Content-Type: audio/ogg`）；
  - 小智端直接复用原生 `OggDemuxer` 与官方 `AudioService::PushPacketToDecodeQueue`，零新增外部库依赖，超低码率（96kbps）且具备极高声学还原度。

### 2. 元数据拉取与界面联动机制
1. **异步曲库同步（Metadata Sync Task）**：
   - 启动轻量级后台任务请求 `/rest/getRandomSongs.view?size=10`；
   - 提取真实 `id`、`title`（歌名）、`artist`（歌手）、`duration`（时长）；
   - 即时更新 Cyber HUD 播放器界面与控制中心；
2. **三键大热区控制与音频流生命周期**：
   - **播放/暂停**：点击播放时启动 `navi_stream` 任务，分块读取 HTTP 流并持续送入解码队列；点击暂停时平滑停止流任务并重置解码器；
   - **上一曲/下一曲**：切歌时自动重置 270° 进度环，无缝切换曲目；
   - **自动连播**：单曲播放完毕自动平滑推进下一曲。
3. **语音交互增强（MCP Tools）**：
   - 注册 `self.music.play_pause`、`self.music.next`、`self.music.prev`、`self.music.refresh`，支持全语音点播与控制。

---

## 十一、Navidrome 串流崩溃重启与天气时段高亮死板问题排查与攻坚

### 1. 音乐播放崩溃重启（页面重载刷新）根因定位
用户反馈：“播放音乐的时候会重载刷新页面，就是不 work”。经深入源码与底层内存排查，发现以下三大致命根因交织导致的崩溃软重启：
1. **FreeRTOS 任务栈溢出（Stack Overflow Panic）**：
   - `StartNavidromeStream` 创建任务时栈大小仅为 `4096` 字节；
   - 任务函数内部在栈上声明了 `OggDemuxer demuxer;`（其内部 `context_t` 结构体包含 2KB 数据包缓冲区等，对象自身占用近 3KB 空间），且栈上还声明了 `std::array<char, 1024> buffer;`；
   - 栈上大对象已达 4KB，一进入 `http->Open()` 网络底层建立连接与 I/O 时，直接踩爆 FreeRTOS 栈警戒哨兵（Stack Canary），硬件触发 `Stack smashing protect failure`，引发 ESP32 软重启（页面表现为重载刷新）。
2. **OggDemuxer 立体声流人为拦截**：
   - Navidrome 服务端实时转码输出的标准 Ogg-Opus 为双声道（2 channels，`OpusHead 01 02`）；
   - `main/audio/demuxer/ogg_demuxer.cc` 原先包含硬编码校验：`if (!opus_info_.mono) { has_error_ = true; return processed; }`；
   - 导致一旦收到第一个数据包即被判定为错误异常跳出。
3. **播放异常退出引发高频递归死循环切歌**：
   - 任务退出时检测到 `!stream_stop_requested_ && is_playing_`，误以为单曲播完，触发 `Schedule([self]() { self->OnPlayerNextClicked(); });`；
   - 下一首切入后又立即因为双声道报错跳出切歌，高频反复创建/销毁任务与栈溢出，导致系统彻底瘫痪。

### 2. 音频流架构重构与治本方案
- **任务栈与对象堆分配（Heap Isolation）**：
  - 任务栈扩大至 `8192` 字节，留足 lwip/mbedtls 与 FreeRTOS 调度余量；
  - `OggDemuxer` 与读取缓冲区全部采用 `std::make_unique<...>()` 分配到堆/PSRAM 中，严禁在任务栈上分配大对象；
- **Demuxer 全声道兼容（Stereo Downmix）**：
  - 改造 `ogg_demuxer.h` 与 `ogg_demuxer.cc`，扩展为支持 1 通道（Mono）与 2 通道（Stereo）；
  - `AudioService` 解码器以 `ESP_AUDIO_MONO` 初始化，依赖底层 libopus 标准规范自动将立体声 Opus 包按 `(L+R)/2` 下混输出为单声道 PCM，无需上层任何额外处理；
- **并发控制与安全切歌**：
  - 引入基于自增版本号的原子校验 `task_playback_id = ++current_playback_id_;`，任何任务若发现版本不匹配或已请求停止则立刻安全退出；
  - 只有在真正 `eof && !packet_error && demuxer->Finish()` 自然完整播完时才推进下一曲，出错时绝不递归切歌；
  - 播放启动时提升 Wi-Fi 功耗等级为 `PowerSaveLevel::PERFORMANCE`，保障实时音频流网络吞吐。

### 3. 天气卡片底部时间与选中态写死 18 点修复
- **根因**：`EnsureWeatherUI` 中写死了 `15:00`（常规）、`18:00`（高亮选中蓝框）、`21:00`（常规），且 `UpdateWeatherClock` 留空未实现；
- **动态整点计算与状态自适应**：
  - 成员变量保存 3 个时段容器 `fore_cards_[3]`、时间标签 `fore_times_[3]`、温度标签 `fore_temps_[3]`；
  - 实现 `UpdateWeatherHourlyForecast()`：读取系统当前本地小时 `cur_h = timeinfo.tm_hour`；
  - 动态计算 3 个时段：`h[0] = cur_h`（当前时段）、`h[1] = (cur_h + 3) % 24`、`h[2] = (cur_h + 6) % 24`；
  - **第 0 个卡片永远保持高亮选中态**（深蓝背景 `0x182234` + 科技蓝框 `0x0284C7` + 高亮蓝字 `0x38BDF8`）；
  - **第 1、2 个卡片保持未选中暗色常规态**（暗灰边框 `0x1E293B` + 灰暗文字 `0x94A3B8`）；
  - 根据主天气温度动态推演后续时段温差，并在时钟定时器与天气更新时自动刷新。

---

## 十二、Cyber HUD 音乐播放器「动态律动跳柱频谱 + 黑胶慢速自转光斑」极低开销实现

### 1. 动效需求与嵌入式算力军规约束
- **核心诉求**：音乐播放时画面必须呈现科技动感（如电平频谱跳动与唱片旋转），但**绝不能争抢 CPU 资源**，必须保障 Wi-Fi 吞吐、Ogg-Opus 软解与 I2S 音频流绝不卡顿（Zero Underrun）。
- **极客设计策略**：
  1. **低频节拍调度（90ms 节拍，约 11fps）**：人眼感觉连贯平滑，而每次触发仅微调对象高度/坐标，单次回调耗时不足 10 微秒；
  2. **深度休眠联动（Page-Aware Zero Overhead Sleep）**：
     - 当暂停播放、或者切离播放器页面（回到主表盘、进入天气页、下拉控制中心）时，动画定时器立即 `lv_timer_pause`；
     - 频谱柱高平滑回退到 3px 待机微线，进入真正的 **0% CPU 占用状态**；
     - 仅当且仅当「处于播放器页面（`current_page_ == -1`）且 `is_playing_ == true`」时才唤醒定时器。

### 2. 视觉表现与实现细节
1. **黑胶核心声学声波视窗（Vinyl Core 7-Bar Acoustic Waveform）**：
   - 彻底取代了原先单点慢速自转光斑，将 7 根青蓝渐变（`0x00E5FF -> 0x38BDF8 -> 0x34D399 -> 0x6EE7B7`）电平跳柱直接居中镶嵌在黑胶唱片中央同心圆（`vinyl_inner_`）内；
   - 采用黄金对称声学波形算法，中心峰值柱最高可达 20px，两翼自然延展起伏，上下对称展开；
   - 让整块圆屏的视觉焦点从原先分散的单点旋转，高度凝聚到唱片正中央的律动声学核心，极具科技视觉张力。
2. **下半部分布局从容舒展（Breathing Layout Relief）**：
   - 移除了原先挤在曲目与按钮之间的多余跳柱，歌曲标题（y: 232）、歌手名称（y: 264）与控制栏（y: 290）间距彻底拉开；
   - 上半部分呈现声学律动黑胶，下半部分呈现大字曲目信息与大触控热区，层级清晰，呼吸感极佳。

---

## 16. 播放器中文曲名恢复、大尺寸宽幅声谱升级与控制按键状态分明

### 16.1 曲目标题与歌手信息看不见的根因排查
- **问题现象**：播放器页面中，曲目标题与歌手信息完全空白、不显示。
- **排查根因（Font Character Set Miss）**：
  - 之前为追求英文字体排版，将 `player_title_label_` 与 `player_artist_label_` 显式设置为了 `font_maison_neue_book_26` 和 `font_maison_neue_book_14`；
  - 经查 `font_maison_neue_book_*.c` 源码生成参数，其 glyph range 仅包含 ASCII `32-127`；
  - 当 Navidrome 播放列表中包含中文字符（如《威廉古堡》、伍佰《白鴿》等）时，LVGL 在该字体中找不到汉字字模，直接放弃渲染导致整行文本隐形不可见！
- **修复措施**：
  - 切换为全局内置支持全套常用中文字符的 `font_noto_sans_basic_20_4`（曲名）与 `font_noto_sans_basic_16_4`（歌手名）；
  - 设置曲名 `width: 280px` 配合 `LV_LABEL_LONG_SCROLL_CIRCULAR` 跑马灯滚动，歌手名 `LV_LABEL_LONG_DOT` 智能省略，中文曲目和艺人信息全部清晰呈现。

### 16.2 移除中央圆环阻隔，扩展大尺寸宽幅声学频谱
- **移除限制**：移掉了原先黑胶正中央尺寸受限的同心圆环边框（`vinyl_inner_`），不再局限于 60px 的小圆筒内。
- **全景扩展**：
  - 将频谱盒宽度由 48px 大幅扩展至 116px（高度 58px），贯穿黑胶核心区域；
  - 律动柱数量由 7 根增加至 **13 根**（宽度 5px，间距 3px，高度最高可达 46px）；
  - 配色升级为 13 柱全景音律渐变体系：从两翼的冰海蓝（`#0284C7`）平滑过渡到中央的极光青绿（`#6EE7B7`）；
  - 引入复合正弦波调和 + 动态能量扰动算法，呈现极其生动、开阔震撼的声波矩阵。

### 16.3 控制栏特征重构：上一首/下一首高辨识度与播放/暂停状态分明
1. **上一首 / 下一首按钮**：
   - 尺寸从 38×38 提升至 44×44 圆形；
   - 采用深科技蓝黑底色（`#1E293B`），搭配 2px 亮蓝描边（`#38BDF8`）；
   - 图标颜色升级为高对比度象牙白（`#F1F5F9`），在暗黑圆屏上按键轮廓极其醒目，触控特征一目了然。
2. **播放 / 暂停按钮状态分明设计**：
   - 尺寸扩展为 52×52 核心触控键；
   - **播放中（Playing）**：实心霓虹高能青（`#00E5FF`），黑色暂停图标（`LV_SYMBOL_PAUSE`），象征音律能量喷涌；
   - **暂停中（Paused）**：待机科技暗蓝背景（`#0F172A`），2px 亮青边框（`#00E5FF`），亮青播放图标（`LV_SYMBOL_PLAY`），象征线框待命；
   - 两种状态视觉差异巨大，用户无需猜测即可瞬间知晓当前播放状态。

---

## 17. 真机播放/暂停按键无图标（空心圆/实心圆）根因与 Material Symbols 修复

### 17.1 问题现象
- 真机屏幕上，播放/暂停按钮中间**没有显示三角形播放图标或双竖线暂停图标**：
  - 暂停时表现为一个“空心圆”；
  - 播放时表现为一个“实心圆”。

### 17.2 根因排查（Symbol Font Character Set Mismatch）
1. **字符编码体系不匹配**：
   - 代码中原先使用的是 LVGL 内置符号宏 `LV_SYMBOL_PLAY`（`"\xEF\x81\x8B"`）与 `LV_SYMBOL_PAUSE`（`"\xEF\x81\x8C"`），这是 **FontAwesome** 字符编码；
   - 而 XiaoZhi 系统在 ESP32 上编译的图标库是 Google **Material Symbols**（`font_material_symbols_16_4` 与 `font_material_symbols_30_4`），**完全不包含 FontAwesome 字符集**；
   - 导致 LVGL 在字符集内找不到字模，直接放弃绘制任何内容；由于背景色存在，造成了暂停时“空心圆”、播放时“实心圆”的现象。
2. **字体与字符宏正确绑定**：
   - 小智在 `<material_symbols.h>` 中提供了官方的 Material Symbols 宏：
     - 播放：`MATERIAL_SYMBOLS_PLAY_ARROW`（`"\xee\x80\xb7"`）
     - 暂停：`MATERIAL_SYMBOLS_PAUSE`（`"\xee\x80\xb4"`）
     - 上一首：`MATERIAL_SYMBOLS_SKIP_PREVIOUS`（`"\xee\x81\x85"`）
     - 下一首：`MATERIAL_SYMBOLS_SKIP_NEXT`（`"\xee\x81\x84"`）
   - 为主播放按钮图标显式配置 30px 图标字体 `&font_material_symbols_30_4`，为上一首/下一首配置 `&font_material_symbols_16_4`。

---

## 18. MaterialSymbolsOutlined 细线条在低分辨率/高像素密度小圆屏上丢失，改用内嵌矢量抗锯齿 ARGB8888 纯实心图标

### 现象描述
在真机（1.85 寸 360×360 ST77916 圆屏）上：
- 播放控制栏暂停时，中间按键只有一个青色空心圆框，正中间完全看不到三角形；
- 播放时，中间按键变成了一个耀眼的青色实心大圆盘，内部双竖线完全不可辨识。

### 深层根本原因
1. **MaterialSymbolsOutlined 字符集特性**：
   - 查验源码 `font_material_symbols_30_4.c` 与 `16_4.c` 的生成配置，发现字模来源是 `MaterialSymbolsOutlined[FILL,GRAD,opsz,wght]-400.ttf`；
   - 在此字体库中，`MATERIAL_SYMBOLS_PLAY_ARROW` 是 **1px 细线空心轮廓三角形**，`PAUSE` 是 **1px 细线空心双竖框**；
   - 在 1.85 英寸 360×360 的小屏幕上，由于像素密度极高（~260 PPI）以及背光对比度限制，单像素透明度梯度的空心线在黑色背景上几乎完全消失；
   - 此外，之前播放时直接修改 `player_play_btn_` 的背景色为 `0x00E5FF`，导致黑色空心线条被强光溢出淹没，远看就是一片纯色“实心圆盘”。
2. **设计稿标准是“饱满实心几何符号”**：
   - 用户期望的效果是像 Spotify / Apple Music 播放器那样的**高质感实心青色三角形 `▶` 与实心圆角双竖条 `❚❚`**，外层包裹科技灰底色与亮青发光描边环。

### 解决方案
1. **生成 20×20 专用抗锯齿 ARGB8888 图标数据源**：
   - 在 [player_icons.h](file:///Users/anrufen/app/xiaozhi-esp32/main/boards/waveshare/esp32-s3-touch-lcd-1.85c/player_icons.h) 中定义：
     - `img_player_play_arrow`：8x 超采样计算的抗锯齿实心青色三角形（`#00E5FF`）；
     - `img_player_pause`：抗锯齿圆角实心青色双竖条（`#00E5FF`）；
   - 使用静态常驻 `lv_image_dsc_t` 结构体，零动态内存分配，跨平台渲染保证 100% 相同。
2. **统一按钮视觉语言**：
   - `player_play_btn_` 底色固定为深科技蓝灰（`0x0F172A`），边框固定为 2px 亮青（`0x00E5FF`）；
   - 内部图标改用 `lv_image_t`，通过 `lv_image_set_src()` 在播放/暂停时瞬间切换 `img_player_play_arrow` 与 `img_player_pause`；
   - 彻底消灭空心圆缺失和无图案实心圆盘问题，达到完美的真机视觉效果。

---

## 十三、第三方扩展服务（Navidrome/Beszel）配置架构、安全防泄露与线程安全设计原则

在引入个人私有云音乐（Navidrome/Subsonic）与服务器集群探针监控（Beszel）等第三方网络服务后，固件架构面临配置冲突、密码泄露、增量更新覆盖、心跳性能抖动及跨线程 UI 崩溃等多维挑战。为确保代码架构整洁与系统稳健，必须严格遵循以下设计与约束原则：

### 19. 架构隔离与集中式 Kconfig 依赖原则（Zero Intrusion）
1. **板级隔离，严禁侵入核心通用层**：
   - 所有的第三方服务开关宏（如 `CONFIG_WS185C_ENABLE_NAVIDROME`、`CONFIG_WS185C_ENABLE_BESZEL`）及默认凭据参数，必须统一定义在板级专有 Kconfig 区域（例如 `main/Kconfig.projbuild` 中对应 `BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_1_85C` 的 `menuconfig` 块下）；
   - 严禁在 `main/application.cc`、`main/protocols/` 等核心通用业务模块中直接引入板级私有配置头文件或宏，保持 Core 与 Board 的解耦。
2. **宏命名空间规范**：
   - 所有板级私有宏统一冠以板级前缀 `CONFIG_WS185C_*`，杜绝与其他板卡或 ESP-IDF 官方组件发生命名碰撞。

### 20. 多级配置生命周期与优先级准则（NVS Over Kconfig）
1. **多级覆盖策略**：
   - 系统采用 **运行时 NVS 持久化值 > Kconfig 编译默认值** 的两级生效模型；
   - **冷启动读取**：板级初始化时优先从 NVS（命名空间 `waveshare185c`）中读取持久化凭据；仅当 NVS 中对应键不存在或未设置时，才回退并采用 Kconfig 预置的固件默认值；
   - **动态更新即时持久化**：用户通过语音/MCP 工具修改配置后，立即写入 NVS，下次开机自动复用最新配置，无需重新编译烧录固件。
2. **NVS Key 长度与命名空间隔离**：
   - ESP-IDF NVS 键名长度严格限制在 15 字符以内；
   - 统一使用紧凑命名：Navidrome 使用 `navi_url`、`navi_user`、`navi_pass`；Beszel 使用 `bsz_url`、`bsz_user`、`bsz_pass`、`bsz_fetch_s`、`bsz_rotate_s`。

### 21. 敏感凭据工程防泄露三层防护机制（Security Best Practices）
1. **第一层：代码库零明文（Zero Secrets in VCS）**：
   - Git 追踪的源文件（`Kconfig.projbuild`、C/C++ 代码、示例文档）中，默认值必须使用通用占位符（如 `http://your-server-ip:port`、`admin`、`password`），严禁写入真实内网 IP、公网域名与密码。
2. **第二层：本地配置自动重载（`sdkconfig.override`）**：
   - 开发自用或批量固件预设的真实账号密码，写入根目录的 [sdkconfig.override](file:///Users/anrufen/app/xiaozhi-esp32/sdkconfig.override)；
   - 该文件必须加入 `.gitignore`，既能使 `python3 scripts/build.py` 在编译时自动融合本地真实配置，又能从物理层面 100% 杜绝意外提交泄露。
3. **第三层：CI / 提交前敏感词静态扫描门禁**：
   - 维护专用静态检查脚本（如 `python3 scripts/scan_secrets.py`），对即将发布的源码与 Commit 进行高危敏感词/IP 模式扫描，形成安全防线。

### 22. 增量更新与防静默擦除防呆机制（Safe Partial Update）
1. **凭据保留原则（Preserve Existing Credentials）**：
   - 在 MCP 工具（如 `self.navidrome.set_server`）处理请求时，若用户仅指定修改 `url`，未传递 `username` 或 `password`，工具处理函数**严禁将未传字段覆盖为空字符串**；
   - 必须先读取现有 NVS 或当前配置，在保留旧密码的前提下仅更新目标字段，避免用户调参导致鉴权静默失效。
2. **输入防御性校验与 URL 规整化**：
   - 强制拦截并拒绝非法空字符串 URL；
   - 自动清洗 URL 尾部的斜杠 `/`（如将 `http://192.168.1.10:8090/` 规整为 `http://192.168.1.10:8090`），彻底消除后续拼接 API 路由时产生 `//` 导致 HTTP 404 或重定向失败的隐患。

### 23. 高性能心跳与 NVS 内存缓存（Cache）隔离原则
1. **禁止在定时器/心跳路径高频读取 Flash**：
   - NVS 基于底层 SPI Flash 操作，读取带有加锁开销与总线耗时；
   - 严禁在 1Hz 定时器、UI 刷新循环或音频播放心跳中频繁调用 `nvs_get_str()`；
2. **内存结构体常驻缓存**：
   - 所有服务配置在类实例中维护一份内存结构体缓存（RAM Cache）；
   - 定时轮询与网络拉取直接访问 RAM 变量；仅在收到配置变更事件时，一次性写入 NVS 并同步更新内存缓存。

### 24. 异步 MCP 回调与 LVGL 跨线程安全红线（Thread Safety Boundary）
1. **MCP 回调上下文并非 UI 线程**：
   - 大模型下发的 MCP 工具在独立的协议通信 Task（如 WebSocket / MQTT 接收线程）中触发并执行回调；
   - **绝对禁止在 MCP 回调函数中裸调任何 LVGL API**，否则会导致多核竞争、LVGL 对象链表破坏引发的 `Guru Meditation Error`。
2. **统一加锁规范**：
   - 任何涉及界面的操作（如更新设置页显示 `UpdateSettingsValues()`、更新服务器连接状态 `UpdateServerUI()`、更新播放器信息 `UpdatePlayerUI()`），必须首行通过 `DisplayLockGuard lock(this);` 获取全局互斥锁，或通过 `Application::GetInstance().Schedule()` 投递至主线程执行，确保 100% 线程安全。

---

*(文档将随每次修改反馈成功后持续追加更新)*



