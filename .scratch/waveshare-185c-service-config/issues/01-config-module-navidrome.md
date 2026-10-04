# 01 — 配置模块 + Navidrome 端到端可配置

**What to build:** 设备的 Navidrome 连接参数（地址、账号、密码）不再写死在源码里。出厂默认值来自 menuconfig（凭据默认为空），设备上以 NVS 为准；通过 MCP 工具设置后立即生效，断电重启仍保留。这是整个 spec 的 tracer bullet：同时建立"服务配置"模块与它的宿主机测试 seam，并先让 Navidrome 一条链路从 Kconfig 走到真实请求。

**Blocked by:** None — can start immediately

**Status:** ready-for-agent

- [ ] 新增板级私有的服务配置模块，不依赖 LVGL/ESP-IDF 头文件，存储后端经小接口注入；生产实现对接仓库已有的 `Settings`，不另造 NVS 封装
- [ ] 取值规则为"NVS 有值用 NVS，否则用 Kconfig 默认值"，命名空间与键名严格遵循 SPEC 中的 NVS 约定
- [ ] 在仓库集中式 Kconfig 的 1.85C 依赖下新增 Navidrome 地址/账号/密码项，默认值全部为空（不得新建板级 Kconfig 文件，不得编辑 `sdkconfig*`）
- [ ] 地址规范化：去首尾空白与末尾 `/`；必须以 `http://` 或 `https://` 开头；空值视为未配置
- [ ] 现有 Navidrome 请求（拉取曲库、拉流）全部改为读取配置模块的当前值，源码与头文件中不再出现任何写死的 Navidrome 地址、账号、密码
- [ ] 现有 MCP 设置工具改为写入 NVS 并立即生效，且可设置账号与密码；非法输入返回错误；返回值与日志不回显密码
- [ ] 日志中不出现密码；原先会输出含密码的拉流地址的日志必须脱敏
- [ ] 设置页上显示的 Navidrome 地址来自当前生效配置，而不是写死的文字
- [ ] 宿主机单元测试覆盖：NVS 优先于默认值、NVS 为空回退、规范化（空白/尾斜杠/缺协议头/空值）、"已配置"判定，并纳入 `python3 -m unittest discover -s scripts/tests -v`
- [ ] 1.85C 变体构建通过；最终报告写明哪些需要真机验证（拉取曲库、拉流、MCP 改址后重启保留）
