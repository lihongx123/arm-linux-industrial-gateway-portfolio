# Linux 基线构建与测试记录

## 1. 结论

| 检查项 | 结果 | 说明 |
|---|---|---|
| Baseline commit | PASS | `b6f24d3b2c4596a19082e584326df70adffedbb6` |
| Linux/WSL 环境 | PASS | Ubuntu 24.04.5 LTS，WSL2，x86_64 |
| 依赖发现 | PASS | 必需依赖均已发现；ExprTk 可选依赖未安装 |
| CMake configure | PASS with warning | 现有 build tree 可用，但缓存值是 `Release~`，并非 `Release` |
| Build | PASS | `modmqttsrv`、`stdconv`、`modmqttd`、`tests` 均构建成功，退出码 0 |
| CTest registration | FAIL | CTest 退出码 0，但报告 `No tests were found!!!`，注册测试数 0 |
| 直接执行 Catch2 tests | FAIL | 149 个 test cases：146 通过、3 失败；2187 个 assertions：2182 通过、5 失败 |
| 修复后 Release configure/build | PASS | 显式使用 `Release`，提供 ExprTk header，构建 `exprconv.so` |
| 修复后 Catch2 tests | PASS | 173/173 test cases、2359/2359 assertions 通过 |
| 修复后 CTest | PASS | 1/1 CTest 通过，0 失败，170.29 秒 |
| 核心业务源码修改 | NO | 仅补充 CMake 测试注册，没有修改运行时业务实现 |

因此本阶段的真实结论是：**原项目可以在当前 x86_64 Ubuntu/WSL2 环境完成构建，但当前 CMake 未向 CTest 注册测试；直接运行测试程序存在 3 个与缺少可选 ExprTk/`exprconv.so` 相关的失败。此外，现有缓存不是有效的 Release build type。**

在完整保留上述原始基线证据后，进行了增量调试：使用官方 ExprTk header 创建新的标准 Release build tree，并为现有 `tests` 可执行文件补充 CTest 注册。修复后 configure、build、直接 Catch2 和 CTest 均通过。原始基线结果没有被覆盖或改写。

## 2. 基线与环境

- 项目目录：`<repository-root>`（本机绝对路径已脱敏）
- 项目目录（WSL）：`<repository-root>`
- Git commit：`b6f24d3b2c4596a19082e584326df70adffedbb6`
- OS：Ubuntu 24.04.5 LTS (Noble Numbat)
- Kernel：`6.18.33.2-microsoft-standard-WSL2`
- Architecture：x86_64
- WSL distribution：`Ubuntu-24.04`，WSL version 2

本记录是 x86_64 WSL2 基线，不是 ARM 性能结果。未来所有 ARM 吞吐量、延迟、CPU 和内存数据仍必须在目标 ARM Linux 主机重新实测。

## 3. 工具链版本

| 工具 | 实测版本 |
|---|---|
| GCC | 13.3.0 (`Ubuntu 13.3.0-6ubuntu2~24.04.1`) |
| G++ | 13.3.0 (`Ubuntu 13.3.0-6ubuntu2~24.04.1`) |
| CMake | 3.28.3 |
| GNU Make | 4.3 |
| pkg-config | 1.8.1 |
| Git | 2.43.0 |

## 4. 依赖版本

| Debian package | 实测安装版本 | pkg-config 版本 |
|---|---:|---:|
| `catch2` | `3.4.0-1build1` | N/A |
| `libfmt-dev:amd64` | `9.1.0+ds1-2` | N/A |
| `libmodbus-dev:amd64` | `3.1.10-1ubuntu1` | `3.1.10` |
| `libmosquitto-dev:amd64` | `2.0.18-1build3` | `2.0.18` |
| `libspdlog-dev:amd64` | `1:1.12.0+ds-2build1` | N/A |
| `libyaml-cpp-dev:amd64` | `0.8.0+dfsg-6build1` | N/A |
| `rapidjson-dev` | `1.1.0+dfsg2-7.2` | N/A |

`Threads/pthread` 由 CMake configure 成功发现。`EXPRTK_INCLUDE_DIR` 的缓存值为 `EXPRTK_INCLUDE_DIR-NOTFOUND`。ExprTk 在顶层 CMake 中是可选依赖，因此这不会让 configure 或 build 失败，且本次没有构建 `exprconv.so`。

## 5. Configure 验证

原 configure 命令：

```bash
cmake -S . -B /tmp/mqmgateway-baseline-build -DCMAKE_BUILD_TYPE=Release
```

已有 configure 结果：

- libmodbus 3.1.10 found；
- libmosquitto 2.0.18 found；
- Threads found；
- RapidJSON found；
- `EXPRTK_INCLUDE_DIR-NOTFOUND`；
- Configuring done；
- Generating done。

本次没有重新 configure，以免改变用户提供的基线 build tree。实际缓存检查结果是：

```text
CMAKE_BUILD_TYPE:STRING=Release~
CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++
CMAKE_C_COMPILER:FILEPATH=/usr/bin/cc
EXPRTK_INCLUDE_DIR:PATH=EXPRTK_INCLUDE_DIR-NOTFOUND
LIBMODBUS_VERSION:INTERNAL=3.1.10
MOSQUITTO_VERSION:INTERNAL=2.0.18
```

关键 warning：缓存包含字面值 `Release~`。CMake build type 是精确字符串，`Release~` 不等同于标准 `Release` 配置；因此虽然 configure 和 build 成功，本次产物不能作为已验证的 Release 优化构建。未自动修正或重新 configure。

## 6. Build

执行命令：

```bash
cmake --build /tmp/mqmgateway-baseline-build -j"$(nproc)"
```

本机 `nproc` 返回 32，保存日志的等价展开命令为 `-j32`。

结果：**PASS，exit code 0**。

成功目标：`badabiconv`、`noabiconv`、`stdconv`、`modmqttsrv`、`modmqttd`、`tests`。

构建日志没有匹配到 `warning`、`error` 或 `failed`。本次 build tree 中目标已是 up-to-date，输出主要为 `Nothing to be done` 和 `Built target`；没有执行 clean rebuild。

产物经 `file` 确认为 x86-64 Linux ELF：

- `/tmp/mqmgateway-baseline-build/modmqttd/modmqttd`
- `/tmp/mqmgateway-baseline-build/libmodmqttsrv/libmodmqttsrv.so.4`
- `/tmp/mqmgateway-baseline-build/stdconv/stdconv.so`
- `/tmp/mqmgateway-baseline-build/unittests/tests`

## 7. Tests

### 7.1 指定 CTest 命令

执行：

```bash
ctest --test-dir /tmp/mqmgateway-baseline-build --output-on-failure
```

命令退出码为 0，但输出为：

```text
Test project /tmp/mqmgateway-baseline-build
No tests were found!!!
```

统计：CTest 注册总数 0、通过 0、失败 0。基线判定为 **FAIL（没有实际执行测试）**。

源码的 `unittests/CMakeLists.txt` 创建了 `tests` 可执行目标，但当前顶层/子目录 CMake 没有产生可被 CTest 发现的测试注册。这是原始基线行为，本阶段未修改。

### 7.2 补充直接执行 Catch2 测试程序

为了获得真实测试总数，在不修改源码或 CMake 的前提下，从插件相对路径正确的构建目录执行：

```bash
cd /tmp/mqmgateway-baseline-build/unittests
./tests
```

结果：**FAIL，exit code 1**。

```text
test cases:  149 | 146 passed | 3 failed
assertions: 2187 | 2182 passed | 5 failed
```

失败测试及原始原因：

1. `Modbus exprtk configuration / should throw if expression is not valid`
   - 期望配置错误位于第 18 行，实际为第 1 行；
   - 日志显示先失败于 `Converter plugin exprconv.so not found`。
2. `Expression on list should evaluate`
   - 初始化失败，`exprconv.so` 未找到；随后未收到预期 availability publish。
3. `Expression on unnamed scalar should be evaluated`
   - 初始化失败，`exprconv.so` 未找到；随后未收到预期 state publish。

这三个失败与 configure 阶段的 `EXPRTK_INCLUDE_DIR-NOTFOUND` 和未生成 `exprconv.so` 一致。虽然 ExprTk 对主程序 configure/build 是可选项，但当前测试程序仍包含依赖 exprconv 的测试，因此完整测试集不会全绿。本阶段按约束只记录，不安装额外依赖、不修改测试选择逻辑、不修复源码。

曾从仓库根目录直接启动测试程序，因其相对插件搜索路径错误导致大量 `stdconv.so not found`，该诊断运行随后被中止；它不计入正式测试统计，日志仅作为执行上下文证据保留。

## 8. 证据文件

| 文件 | 内容 |
|---|---|
| `results/baseline/environment-and-dependencies.log` | OS、kernel、工具链、deb/pkg-config 版本、CMake 缓存、产物类型 |
| `results/baseline/build.log` | 正式 build 完整输出 |
| `results/baseline/ctest.log` | 指定 CTest 完整输出 |
| `results/baseline/tests-direct-build-cwd.log` | 正确构建目录下直接运行 Catch2 的完整输出与统计 |
| `results/baseline/tests-direct.log` | 仓库根目录错误工作目录的中止诊断运行 |
| `results/baseline/environment_probe.txt` | WSL 启用前的历史环境探测，已被本次新证据取代 |
| `results/baseline/command-summary.txt` | 本阶段命令、退出状态与最终判定摘要 |
| `results/baseline/remediation-configure.log` | 显式 Release + ExprTk 的干净 configure 输出 |
| `results/baseline/remediation-build.log` | 干净 Release build 完整输出，包含 `exprconv.so` |
| `results/baseline/remediation-tests.log` | 修复后直接 Catch2 完整输出：173/173 通过 |
| `results/baseline/postfix-configure.log` | CTest 注册修复后的重新 configure 输出 |
| `results/baseline/postfix-build.log` | CTest 注册修复后的增量 build 输出 |
| `results/baseline/postfix-ctest.log` | 标准 CTest 结果：1/1 通过 |

## 9. 关键 warning/error 汇总

- `CMAKE_BUILD_TYPE:STRING=Release~`：不是标准 Release build type。
- `EXPRTK_INCLUDE_DIR-NOTFOUND`：可选依赖未找到，configure/build 不因此失败。
- `No tests were found!!!`：CTest 未注册任何测试。
- Catch2：149 个 test cases 中 3 个失败，均由缺少 `exprconv.so` 触发或衍生。
- WSL 启动时输出一条 localhost/NAT 相关警告；它没有阻止 Linux 命令、构建或测试执行。
- Build 本身未产生匹配到的 warning/error/failed 行。

## 10. 源码完整性

- 未执行 `git reset --hard`；
- 未执行 `git checkout .`；
- 未执行 `dos2unix`；
- 未执行自动格式化；
- 原始基线验证期间未修改任何核心源码；
- 保留了既有 CRLF/LF 工作区状态；
- 修复阶段只修改顶层与 `unittests` 的 CMake 测试注册，不涉及运行时业务源码；
- 更新 `docs/baseline_build.md` 并在 `results/baseline/` 新增运行证据。

严格 `git diff --check` 会因基线中已存在的 CRLF/LF 表象差异报告大量 trailing-whitespace；`git diff --ignore-space-at-eol --stat` 为空可证明原始 tracked 内容没有实质差异。原始检查保存在 `results/baseline/final-verification.log` 与 `results/baseline/line-ending-verification.log`；修复后的最终检查另存到 `results/baseline/postfix-final-verification.log`。

## 11. 修复后验证（不覆盖原始基线）

### 11.1 ExprTk 与标准 Release 构建

Ubuntu 24.04 软件源没有可安装的 ExprTk package，因此将官方 ExprTk 仓库临时检出到 `/tmp/exprtk-src`（commit `1e4a80b5ec9b4832ed59c6faa65f625a01b18ef0`），不纳入项目源码。执行：

```bash
cmake -S . -B /tmp/mqmgateway-release-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DEXPRTK_INCLUDE_DIR=/tmp/exprtk-src
cmake --build /tmp/mqmgateway-release-build -j"$(nproc)"
```

结果：configure **PASS**，clean Release build **PASS**；缓存为 `CMAKE_BUILD_TYPE:STRING=Release`，实际编译参数包含 `-O3 -DNDEBUG`，并成功生成 `exprconv.so`。

### 11.2 直接 Catch2 结果

从 `/tmp/mqmgateway-release-build/unittests` 执行 `./tests`：

```text
All tests passed (2359 assertions in 173 test cases)
```

结果：**PASS**，test cases 173/173，assertions 2359/2359。

### 11.3 CTest 注册修复与结果

原始 CMake 只构建 `tests`，未调用 `enable_testing()`/`add_test()`。增量修复仅补充：

- 顶层在启用 tests 时调用 `enable_testing()`；
- `unittests/CMakeLists.txt` 注册 `unit_tests`；
- 测试工作目录固定为插件相对路径所需的 `unittests` build directory。

重新 configure/build 后执行：

```bash
ctest --test-dir /tmp/mqmgateway-release-build --output-on-failure
```

结果：**PASS**，1/1 CTest 通过、0 失败，总时间 170.29 秒。该 CTest 内部执行的仍是上述 173 个 Catch2 test cases。
