import os
import shlex
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOARD_DIR = ROOT / "main" / "boards" / "waveshare" / "esp32-s3-touch-lcd-1.85c"


class ServiceConfigTest(unittest.TestCase):
    def test_service_config_behavior(self):
        driver = r"""
            #include <cassert>
            #include <iostream>
            #include <map>
            #include <memory>
            #include <string>

            #include "service_config.h"

            using namespace waveshare185c;

            class MemoryKeyValueStore : public KeyValueStore {
            public:
                std::map<std::string, std::string> strings;
                std::map<std::string, int32_t> ints;

                std::string GetString(const std::string& key, const std::string& default_val = "") override {
                    auto it = strings.find(key);
                    return (it != strings.end()) ? it->second : default_val;
                }

                void SetString(const std::string& key, const std::string& value) override {
                    strings[key] = value;
                }

                int32_t GetInt(const std::string& key, int32_t default_val = 0) override {
                    auto it = ints.find(key);
                    return (it != ints.end()) ? it->second : default_val;
                }

                void SetInt(const std::string& key, int32_t value) override {
                    ints[key] = value;
                }
            };

            void TestUrlNormalization() {
                std::string out;
                // allow_empty = true: 空白或空串 -> 空输出，返回 true (视为未配置)
                assert(ServiceConfig::NormalizeAndValidateUrl("", out, true));
                assert(out == "");
                assert(ServiceConfig::NormalizeAndValidateUrl("   \t\r\n  ", out, true));
                assert(out == "");

                // allow_empty = false (默认): 空白或空串直接拒绝返回 false
                assert(!ServiceConfig::NormalizeAndValidateUrl("", out));
                assert(!ServiceConfig::NormalizeAndValidateUrl("   ", out));

                // 正常 URL
                assert(ServiceConfig::NormalizeAndValidateUrl("http://192.168.1.1:1011", out));
                assert(out == "http://192.168.1.1:1011");

                // 末尾多余的斜杠必须被清除
                assert(ServiceConfig::NormalizeAndValidateUrl("http://music.local:4533/", out));
                assert(out == "http://music.local:4533");
                assert(ServiceConfig::NormalizeAndValidateUrl("https://example.com///", out));
                assert(out == "https://example.com");

                // 首尾有空格需清除
                assert(ServiceConfig::NormalizeAndValidateUrl("  https://my-vps.com:8090/  ", out));
                assert(out == "https://my-vps.com:8090");

                // 非法协议头
                assert(!ServiceConfig::NormalizeAndValidateUrl("ftp://example.com", out));
                assert(!ServiceConfig::NormalizeAndValidateUrl("192.168.1.100:1011", out));

                // 仅协议头无主机
                assert(!ServiceConfig::NormalizeAndValidateUrl("http://", out));
                assert(!ServiceConfig::NormalizeAndValidateUrl("https:///", out));
            }

            void TestUrlRedaction() {
                assert(ServiceConfig::RedactUrl("http://hub.com") == "http://hub.com");
                assert(ServiceConfig::RedactUrl("http://hub.com/stream?id=123&u=admin&p=secret123&v=1") ==
                       "http://hub.com/stream?id=123&u=admin&p=***&v=1");
                assert(ServiceConfig::RedactUrl("http://hub.com/stream?p=topsecret") ==
                       "http://hub.com/stream?p=***");
            }

            void TestNavidromeConfigPrecedenceAndIncremental() {
                auto store = std::make_shared<MemoryKeyValueStore>();
                ServiceConfig cfg(store);

                // 初始状态，无 NVS 且默认值为空
                NavidromeConfig navi = cfg.GetNavidromeConfig();
                assert(navi.url.empty());
                assert(navi.user.empty());
                assert(navi.pass.empty());
                assert(!navi.IsConfigured());

                // 设置 Kconfig 默认值，带斜杠的默认值应被自动规范化
                NavidromeConfig defaults{"http://default.local:4533///", "def_user", "def_pass"};
                BeszelConfig bsz_defaults;
                cfg.SetKconfigDefaults(defaults, bsz_defaults);

                // NVS 为空时回退到默认值，斜杠已被清除
                navi = cfg.GetNavidromeConfig();
                assert(navi.url == "http://default.local:4533");
                assert(navi.user == "def_user");
                assert(navi.pass == "def_pass");
                assert(navi.IsConfigured());

                // 拒绝空 URL 输入
                std::string err;
                assert(!cfg.SetNavidromeConfig("", "user", "pass", err));
                assert(!err.empty());

                // 写入 NVS，NVS 优先于默认值
                err.clear();
                assert(cfg.SetNavidromeConfig("https://custom.server.com:4533/", "my_user", "my_pass", err));
                assert(err.empty());

                navi = cfg.GetNavidromeConfig();
                assert(navi.url == "https://custom.server.com:4533");
                assert(navi.user == "my_user");
                assert(navi.pass == "my_pass");
                assert(navi.IsConfigured());

                // 增量更新：仅更新 URL，未传入 user 与 pass 时自动保留既有值
                assert(cfg.SetNavidromeConfig("http://new.server.com:80/", "", "", err));
                navi = cfg.GetNavidromeConfig();
                assert(navi.url == "http://new.server.com:80");
                assert(navi.user == "my_user");
                assert(navi.pass == "my_pass");

                // 测试非法输入报错
                assert(!cfg.SetNavidromeConfig("invalid_url_without_http", "", "", err));
                assert(!err.empty());
            }

            void TestBeszelConfigAndIntervalsAndIncremental() {
                auto store = std::make_shared<MemoryKeyValueStore>();
                ServiceConfig cfg(store);

                // Kconfig 默认带斜杠自动去除
                BeszelConfig bsz_defaults{"http://hub.default:8090/", "admin@def", "pass123", 20, 8};
                NavidromeConfig navi_defaults;
                cfg.SetKconfigDefaults(navi_defaults, bsz_defaults);

                // 初始回退默认值
                BeszelConfig bsz = cfg.GetBeszelConfig();
                assert(bsz.url == "http://hub.default:8090");
                assert(bsz.user == "admin@def");
                assert(bsz.pass == "pass123");
                assert(bsz.fetch_interval_s == 20);
                assert(bsz.rotate_interval_s == 8);
                assert(bsz.IsConfigured());

                // 拒绝空 URL 输入
                std::string err;
                assert(!cfg.SetBeszelConfig("", "u", "p", 10, 5, err));

                // 写入合法 NVS 配置
                assert(cfg.SetBeszelConfig("http://vps.hub:8090", "user@test", "testpass", 10, 3, err));
                bsz = cfg.GetBeszelConfig();
                assert(bsz.url == "http://vps.hub:8090");
                assert(bsz.user == "user@test");
                assert(bsz.pass == "testpass");
                assert(bsz.fetch_interval_s == 10);
                assert(bsz.rotate_interval_s == 3);

                // 增量更新：仅改 URL，账号密码传空、间隔传 0，自动保留原有设置
                assert(cfg.SetBeszelConfig("https://vps.hub.new:8443/", "", "", 0, 0, err));
                bsz = cfg.GetBeszelConfig();
                assert(bsz.url == "https://vps.hub.new:8443");
                assert(bsz.user == "user@test");
                assert(bsz.pass == "testpass");
                assert(bsz.fetch_interval_s == 10);
                assert(bsz.rotate_interval_s == 3);

                // 测试显式传入非法间隔：>86400
                assert(!cfg.SetBeszelConfig("http://vps.hub:8090", "user@test", "testpass", 999999, 3, err));

                // 如果 NVS 里的整数被意外写入非法值（如 -1），读取时回退到默认值
                store->SetInt(ServiceConfig::kKeyBszFetch, -1);
                store->SetInt(ServiceConfig::kKeyBszRotate, 0);
                bsz = cfg.GetBeszelConfig();
                assert(bsz.fetch_interval_s == 20);
                assert(bsz.rotate_interval_s == 8);
            }

            void TestIsConfiguredMatrix() {
                NavidromeConfig navi{"", "", ""};
                assert(!navi.IsConfigured());
                navi = {"http://localhost:4533", "", ""};
                assert(!navi.IsConfigured());
                navi = {"http://localhost:4533", "user", ""};
                assert(!navi.IsConfigured());
                navi = {"http://localhost:4533", "user", "pass"};
                assert(navi.IsConfigured());

                BeszelConfig bsz{"", "", ""};
                assert(!bsz.IsConfigured());
                bsz = {"http://hub:8090", "user", ""};
                assert(!bsz.IsConfigured());
                bsz = {"http://hub:8090", "user", "pass"};
                assert(bsz.IsConfigured());
            }

            int main() {
                TestUrlNormalization();
                TestUrlRedaction();
                TestNavidromeConfigPrecedenceAndIncremental();
                TestBeszelConfigAndIntervalsAndIncremental();
                TestIsConfiguredMatrix();
                std::cout << "All service config tests passed!" << std::endl;
                return 0;
            }
        """

        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            source = build_dir / "service_config_test.cc"
            source.write_text(textwrap.dedent(driver), encoding="utf-8")
            executable = build_dir / "service_config_test"
            command = shlex.split(os.environ.get("CXX", "c++")) + [
                "-std=c++20",
                f"-I{BOARD_DIR}",
                str(source),
                str(BOARD_DIR / "service_config.cc"),
                "-o",
                str(executable),
            ]
            subprocess.run(command, check=True, cwd=build_dir)
            subprocess.run([executable], check=True, cwd=build_dir)


if __name__ == "__main__":
    unittest.main()
