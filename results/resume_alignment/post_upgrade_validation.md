# 架构升级后验收

更新 2026-10-01。运行目录 20260930T143425Z/。
用户最新要求：本地短验证，排除云端，不追加长时间benchmark。

本地架构验收通过；完整说明见 ../../docs/resume_architecture.md。
- mixed-final-faults/summary.json：单进程 30 秒，CAN 3000、RTU 273，独立观察者全部收到；无重复；双下行确认；完整5秒窗口都推进。
- lifecycle-final-retry/summary.json：启动失败和运行中串口断开均正确返回1。
- reliability-final/summary.txt：有界队列拒绝、期限超时、broker恢复通过。
- ctest.log：全量3/3；ctest-final-targeted.log：最终小修后2/2。
- qemu-final-local/host-run.txt：最终 ARM64 二进制本地集成通过。
- lint.log：当前 CMake 无 format-check 目标，不能标记格式工具通过。
- 原始 git diff --check 返回2，含既有CRLF/尾随空白；范围限定的 cr-at-eol 检查通过。
- 历史1043个结果文件的大小/mtime与预审计一致。详见 final-state.json。
- ARM64 stress probe不是CMake目标，先前多目标build末尾报错；网关/RTU目标当时已成功，最终独立构建通过。

所有相对测试路径均位于 20260930T143425Z/。
最终源码散列、修改文件及git状态见 20260930T143425Z/final-state.json。
