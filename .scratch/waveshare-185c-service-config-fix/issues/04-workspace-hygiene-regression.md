# 04 — 工作区敏感日志治理与全链路回归验证

**What to build:** 清理工作区中的调试杂物与敏感日志，完善 `.gitignore`，运行全部自动化测试并执行敏感凭据扫描，确保代码格式合规且系统健壮。

**Blocked by:** 02 — 运行时配置内存缓存与 UI 线程安全互斥, 03 — MCP 参数命名统一与使用者文档全面对齐

**Status:** ready-for-agent

- [x] 清理工作区包含旧真实 IP 的 `serial_monitor.log`，将 `*.log` 与 `compile_commands.json` 规则加入 `.gitignore`
- [x] 运行全套宿主机单元测试（`python3 -m unittest discover -s scripts/tests -v`），确保全部测试（包含新增测试）100% 通过
- [x] 运行 `scripts/scan_secrets.py` 全量扫描板级代码、文档、固件目录，验证零敏感凭据泄漏
- [x] 运行 `clang-format` 验证受修改源码的格式合规性
