# 来源、贡献与许可证

上游：https://github.com/BlackZork/mqmgateway

审计基线：`b6f24d3b2c4596a19082e584326df70adffedbb6`。

| 范围 | 来源和本项目工作 |
| --- | --- |
| Modbus RTU/TCP、YAML配置、converter、原有线程/队列、MQTT双向通信 | 上游已有能力，保留原作者署名与许可 |
| `src/iot_gateway/` | 本项目新增SocketCAN/epoll、统一消息、命令路由、bounded queue、worker pool及流水线观测 |
| `src/serial/` | 本项目新增直接Termios RTU解析和通信验证 |
| `tests/`、新增单元测试 | 本项目新增设备模拟、功能、故障、负载、分段观测测试 |
| Buildroot、toolchain、QEMU运行器 | 本项目新增目标环境构建和实验基础设施 |
| `docs/`、`results/` | 本项目整理的设计、测量、失败记录和证据 |
| CMake入口/CTest注册 | 对上游构建入口做集成修改 |

这些工作在AI辅助开发与调试下完成。发布者应能够解释、复现和维护所提交内容。
修改上游代码的变量名或格式不会改变其来源。未给上游作品改署名；未声称整套
Modbus–MQTT业务由本项目从零实现。

根目录`LICENSE`保留上游AGPL-3.0文本。上游README说明其自身另有商业许可，
该说明不代表本扩展作者有权转授上游商业许可。本发布快照保留开源许可和第三方
许可文件：`argh/LICENSE`、`readerwriterqueue/LICENSE.md`等。

发布包是当前工作目录的源码/证据快照，不携带本机`.git`历史。来源可通过上游地址、
基线commit、上述映射及保留的源文件核验。下载的工具链、系统镜像和ExprTk头文件
未混入源码包；构建时按复现说明取得各依赖。
